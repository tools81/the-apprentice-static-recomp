/* Update check.  At start the program asks GitHub for the latest release of
   UPDATE_REPO (game.json).  When it is newer than APP_VERSION, the release's
   portable zip is downloaded and a small PowerShell script, started after
   the game quits, copies the new files over the program's folder (keeping
   saves\ and the .ini) and starts the game again.

   apprentice.ini: update_check=1 asks first (default), auto installs without
   asking, 0 turns the check off.  --no-update skips it for one run. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "game_config.h"

static char up_dir[512];
static int up_auto;

/* GET a URL (redirects followed); returns 0 and a malloc'd body */
static int http_get(const char *url, char **body, size_t *len) {
    wchar_t wurl[1024], host[256], path[1024];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024);
    URL_COMPONENTSW uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return -1;
    int r = -1;
    HINTERNET s = WinHttpOpen(L"" EXE_NAME "-updater/" APP_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return -1;
    WinHttpSetTimeouts(s, 5000, 5000, 10000, 60000);
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET q = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
    if (q && WinHttpSendRequest(q, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L, NULL, 0, 0, 0) &&
        WinHttpReceiveResponse(q, NULL)) {
        DWORD status = 0, sl = sizeof status;
        WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &sl, NULL);
        if (status == 200) {
            size_t cap = 1 << 16, n = 0;
            char *b = (char *)malloc(cap + 1);
            DWORD got;
            while (b && WinHttpReadData(q, b + n, (DWORD)(cap - n), &got) && got) {
                n += got;
                if (n == cap) { cap *= 2; char *nb = (char *)realloc(b, cap + 1); if (!nb) { free(b); b = NULL; } else b = nb; }
            }
            if (b) { b[n] = 0; *body = b; *len = n; r = 0; }
        }
    }
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return r;
}

/* "key": "value" in a JSON text, from p on; returns the end of the value */
static const char *json_str(const char *p, const char *key, char *out, size_t n) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *k = strstr(p, pat);
    if (!k) return NULL;
    const char *v = strchr(k + strlen(pat), '"');
    if (!v) return NULL;
    const char *e = strchr(v + 1, '"');
    if (!e) return NULL;
    size_t L = (size_t)(e - v - 1);
    if (L >= n) L = n - 1;
    memcpy(out, v + 1, L);
    out[L] = 0;
    return e + 1;
}

/* compare "1.2.3" style versions (a leading v is ignored) */
static int ver_cmp(const char *a, const char *b) {
    if (*a == 'v' || *a == 'V') a++;
    if (*b == 'v' || *b == 'V') b++;
    for (int i = 0; i < 4; i++) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

static const char *updater_ps1 =
    "param([int]$WaitPid, [string]$Zip, [string]$Dest, [string]$Exe)\r\n"
    "$ErrorActionPreference = 'Stop'\r\n"
    "try { Wait-Process -Id $WaitPid -Timeout 60 -ErrorAction SilentlyContinue } catch {}\r\n"
    "$tmp = Join-Path $env:TEMP ('apprentice_update_' + [guid]::NewGuid())\r\n"
    "try {\r\n"
    "  Expand-Archive -LiteralPath $Zip -DestinationPath $tmp -Force\r\n"
    "  $src = $tmp\r\n"
    "  $items = @(Get-ChildItem -LiteralPath $tmp)\r\n"
    "  if ($items.Count -eq 1 -and $items[0].PSIsContainer) { $src = $items[0].FullName }\r\n"
    "  Get-ChildItem -LiteralPath $src -Force | Where-Object { $_.Name -ne 'saves' -and $_.Extension -ne '.ini' } |\r\n"
    "    Copy-Item -Destination $Dest -Recurse -Force\r\n"
    "} catch {\r\n"
    "  Add-Type -AssemblyName PresentationFramework\r\n"
    "  [System.Windows.MessageBox]::Show(\"The update could not be installed: $($_.Exception.Message)\", 'Update') | Out-Null\r\n"
    "} finally {\r\n"
    "  Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue\r\n"
    "  Remove-Item -LiteralPath $Zip -Force -ErrorAction SilentlyContinue\r\n"
    "}\r\n"
    "Start-Process -FilePath $Exe -WorkingDirectory $Dest\r\n";

static DWORD WINAPI update_thread(LPVOID unused) {
    (void)unused;
    char url[512], *json = NULL;
    size_t len = 0;
    snprintf(url, sizeof url, "https://api.github.com/repos/%s/releases/latest", UPDATE_REPO);
    if (http_get(url, &json, &len)) { LOG("update check: no answer from GitHub"); return 0; }
    char tag[64] = "", asset[512] = "";
    json_str(json, "tag_name", tag, sizeof tag);
    for (const char *p = json; (p = json_str(p, "browser_download_url", url, sizeof url)) != NULL;) {
        size_t L = strlen(url);
        if (L > 13 && !_stricmp(url + L - 13, "-portable.zip")) { snprintf(asset, sizeof asset, "%s", url); break; }
    }
    free(json);
    if (!tag[0] || ver_cmp(tag, APP_VERSION) <= 0) { LOG("update check: %s is the latest version", APP_VERSION); return 0; }
    LOG("update check: version %s is available (this is %s)", tag, APP_VERSION);
    if (!asset[0]) return 0;
    if (!up_auto) {
        char msg[512];
        snprintf(msg, sizeof msg,
                 "Version %s of " GAME_TITLE " is available (you have %s).\n\n"
                 "Download and install it now? The game restarts when it is done.\n"
                 "Your saves and settings are kept.", tag + (tag[0] == 'v'), APP_VERSION);
        if (MessageBoxA(NULL, msg, GAME_TITLE " update", MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST) != IDYES)
            return 0;
    }
    char *zip = NULL;
    size_t zlen = 0;
    if (http_get(asset, &zip, &zlen)) {
        MessageBoxA(NULL, "The update could not be downloaded.", GAME_TITLE " update", MB_ICONWARNING);
        return 0;
    }
    char tmp[MAX_PATH], zpath[MAX_PATH], ps1[MAX_PATH], exe[MAX_PATH], cmd[4096];
    GetTempPathA(sizeof tmp, tmp);
    snprintf(zpath, sizeof zpath, "%s%s-update.zip", tmp, EXE_NAME);
    snprintf(ps1, sizeof ps1, "%s%s-update.ps1", tmp, EXE_NAME);
    FILE *f = fopen(zpath, "wb");
    int ok = f && fwrite(zip, 1, zlen, f) == zlen;
    if (f) fclose(f);
    free(zip);
    f = fopen(ps1, "wb");
    ok = ok && f && fputs(updater_ps1, f) >= 0;
    if (f) fclose(f);
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(cmd, sizeof cmd,
             "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"%s\" -WaitPid %lu -Zip \"%s\" -Dest \"%s\" -Exe \"%s\"",
             ps1, GetCurrentProcessId(), zpath, up_dir, exe);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!ok || !CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        MessageBoxA(NULL, "The update could not be started.", GAME_TITLE " update", MB_ICONWARNING);
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    LOG("update check: installing %s, the game restarts", tag);
    input.quit = 1;     /* the run loop ends the program; the script takes over */
    return 0;
}

void update_check_start(const char *exe_dir, int automatic) {
    if (!UPDATE_REPO[0]) return;
    snprintf(up_dir, sizeof up_dir, "%s", exe_dir);
    up_auto = automatic;
    HANDLE t = CreateThread(NULL, 0, update_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}
