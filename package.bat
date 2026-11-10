@echo off
rem Build the release packages: dist\Apprentice-<VERSION>-portable.zip and -Setup.exe
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\package.ps1" %*
