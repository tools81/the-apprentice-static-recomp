"""Per-game configuration (game.json in the project root).

{
  "title":    window title / messages,
  "exe_name": name of the built executable,
  "tv":       "ntsc" (525 lines, 60 Hz, default) or "pal" (625 lines, 50 Hz),
  "version":  release version (x.y.z), compared with the latest release,
  "update_repo": GitHub "owner/name" whose latest release the program checks
              for updates on start ("" = no update check),
  "modules":  program modules to recompile.  The first one is the main
              program the runtime starts.  Each entry:
      "file":          module file name on the disc, or "auto" for the
                       application named in the disc label (main program)
      "base":          fixed load address (hex string).  Every module needs
                       its own non-overlapping range below 0x140000 (the heap
                       starts there); the runtime loads it at this address.
      "extra_entries": code addresses the analyzer cannot find on its own
                       (hex strings, absolute, i.e. base + offset)
      "a5_table":      true / false / "auto": the program uses an MPW-style
                       A5 jump table ("jsr N(a5)" calls)
      "interpret":     true: load the file at "base" but do not recompile it
                       (it runs in the interpreter; for small hand-written
                       programs that mix data into their code)
      "skip":          names of modules in the file that are not recompiled
      "stb_skip":      names in the module's .stb symbol table that are data
                       inside the code, not entry points
    A file may hold several modules; each code module in it is recompiled at
    base + its offset in the file, and the runtime loads the whole file at base.
}
"""
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _hex(v):
    return int(v, 16) if isinstance(v, str) else int(v)


def load():
    with open(os.path.join(ROOT, 'game.json')) as f:
        cfg = json.load(f)
    cfg.setdefault('title', 'CD-i Game')
    cfg.setdefault('exe_name', 'cdigame')
    cfg.setdefault('tv', 'ntsc')
    cfg.setdefault('version', '0.0.0')
    cfg.setdefault('update_repo', '')
    cfg.setdefault('invincibility', None)
    if cfg['tv'] not in ('ntsc', 'pal'):
        raise SystemExit('game.json: "tv" must be "ntsc" or "pal"')
    mods = []
    for m in cfg.get('modules', []):
        mods.append({
            'file': m.get('file', 'auto'),
            'base': _hex(m.get('base', '0x100000')),
            'extra_entries': [_hex(x) for x in m.get('extra_entries', [])],
            'a5_table': m.get('a5_table', 'auto'),
            'a6_table': m.get('a6_table', 'auto'),
            'interpret': bool(m.get('interpret', False)),
            'skip': list(m.get('skip', [])),
            'stb_skip': list(m.get('stb_skip', [])),
        })
    if not mods:
        mods.append({'file': 'auto', 'base': 0x100000, 'extra_entries': [], 'a5_table': 'auto', 'a6_table': 'auto',
                     'interpret': False, 'skip': [], 'stb_skip': []})
    cfg['modules'] = mods
    return cfg


def app_name():
    """main program name recorded by tools/cdi_extract.py from the disc label"""
    p = os.path.join(ROOT, 'extracted', '_app.txt')
    if os.path.exists(p):
        return open(p).read().strip()
    return None


def module_file(m):
    f = m['file']
    if f == 'auto':
        f = app_name()
        if not f:
            raise SystemExit('cannot determine the main program: run tools/cdi_extract.py first')
    return f
