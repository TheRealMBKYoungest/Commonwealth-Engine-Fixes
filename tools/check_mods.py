"""Commonwealth Engine Fixes (CEF) Mod Doctor, 1.0.4.

Checks your Fallout 4 mod setup on this PC, before you play:
  - every plugin (.esm / .esp / .esl) in the Data folder, and which ones are active (plugins.txt)
  - missing masters: a plugin needs another plugin that is not installed, not active, or loads after it
  - plugin limits (254 full plugins, 4096 light plugins)
  - F4SE plugin DLLs in Data\\F4SE\\Plugins, what F4SE said about each one at the last launch (for new ones: whether F4SE
    will load them, read from the DLL's F4SE version data), and Address Library
  - known start-up crash files on the next-gen game: an old Interface\\MainMenu.swf, loose Meshes\\AnimTextData folders
  - game settings mods depend on: loose files enabled (F4SE's scripts are loose files) and plugin selection enabled
  - NPC outfits: leveled lists a later mod emptied while another mod's outfit still uses them (naked NPCs), outfit items
    no active plugin defines, and missing armor meshes (invisible clothes)

It only reads files. It uses the Python standard library only and contains no network code: nothing is uploaded, sent or
downloaded. The result is printed and saved as Documents\\My Games\\Fallout4\\F4SE\\CEF-ModDoctor.txt, with your Windows user
name replaced by <user>.

Run:  double-click "Run Mod Doctor.bat", or:  python check_mods.py
Options: --data DIR          the game's Data folder (default: found automatically)
         --plugins-txt FILE  the plugins.txt to read (default: %LOCALAPPDATA%\\Fallout4\\plugins.txt)
         --mygames DIR       Documents\\My Games\\Fallout4 (default: found automatically)
         --no-save           print only, don't write CEF-ModDoctor.txt
Mod Organizer 2: add "Run Mod Doctor.bat" as an executable in MO2 and start it from there, so it sees MO2's virtual Data
folder and the active profile's plugins.txt.
"""
import argparse
import ctypes
import json
import lzma
import os
import re
import struct
import sys
import textwrap
import zlib

VERSION = '1.0.4'

# Loaded by the engine itself, in this order, whenever the file exists (not listed in plugins.txt)
HARDCODED = ['Fallout4.esm', 'DLCRobot.esm', 'DLCworkshop01.esm', 'DLCCoast.esm', 'DLCworkshop02.esm',
             'DLCworkshop03.esm', 'DLCNukaWorld.esm', 'DLCUltraHighResolution.esm']
FLAG_MASTER, FLAG_LIGHT = 0x1, 0x200          # TES4 record flags
MAX_FULL, MAX_LIGHT = 254, 4096               # load order slots 00-FD; FE holds the light plugins


# ------------------------------------------------------------------ helpers (same as analyze_logs.py)
def documents_folder():
    """The real Documents folder (handles OneDrive redirection) via SHGetKnownFolderPath."""
    try:
        class GUID(ctypes.Structure):
            _fields_ = [('a', ctypes.c_uint32), ('b', ctypes.c_uint16), ('c', ctypes.c_uint16), ('d', ctypes.c_ubyte * 8)]
        fid = GUID(0xFDD39AD0, 0x238F, 0x46AF, (ctypes.c_ubyte * 8)(0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7))
        p = ctypes.c_wchar_p()
        if ctypes.windll.shell32.SHGetKnownFolderPath(ctypes.byref(fid), 0, None, ctypes.byref(p)) == 0:
            path = p.value
            ctypes.windll.ole32.CoTaskMemFree(p)
            return path
    except (AttributeError, OSError):
        pass
    for cand in (os.path.expanduser(r'~\OneDrive\Documents'), os.path.expanduser(r'~\Documents')):
        if os.path.isdir(os.path.join(cand, 'My Games', 'Fallout4')):
            return cand
    return os.path.expanduser(r'~\Documents')


def read_text(path, limit=64 * 1024 * 1024):
    try:
        with open(path, 'rb') as f:
            data = f.read(limit)
        return data.decode('utf-8', errors='replace')
    except OSError:
        return None


class Report:
    def __init__(self):
        self.lines = []

    def h(self, title):
        self.lines += ['', '=' * 78, title, '=' * 78]

    def p(self, text='', indent=0):
        if not text:
            self.lines.append('')
            return
        for line in textwrap.wrap(text, 78 - indent) or ['']:
            self.lines.append(' ' * indent + line)

    def raw(self, line):
        self.lines.append(line)

    def text(self):
        return '\n'.join(self.lines) + '\n'


# ------------------------------------------------------------------ finding things
def find_data_folder():
    # This script ships in Data\F4SE\Plugins\CEF
    here = os.path.dirname(os.path.abspath(__file__))
    cand = os.path.normpath(os.path.join(here, '..', '..', '..'))
    if os.path.isfile(os.path.join(cand, 'Fallout4.esm')):
        return cand
    try:
        import winreg
        for key in (r'SOFTWARE\WOW6432Node\Bethesda Softworks\Fallout4', r'SOFTWARE\Bethesda Softworks\Fallout4'):
            try:
                with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key) as k:
                    path = os.path.join(winreg.QueryValueEx(k, 'installed path')[0], 'Data')
                    if os.path.isfile(os.path.join(path, 'Fallout4.esm')):
                        return path
            except OSError:
                pass
    except ImportError:
        pass
    return None


def exe_version(path):
    """File version of Fallout4.exe as (major, minor, build, private), or None."""
    try:
        ver = ctypes.windll.version
        size = ver.GetFileVersionInfoSizeW(path, None)
        if not size:
            return None
        buf = ctypes.create_string_buffer(size)
        if not ver.GetFileVersionInfoW(path, 0, size, buf):
            return None
        p, n = ctypes.c_void_p(), ctypes.c_uint()
        if not ver.VerQueryValueW(buf, '\\', ctypes.byref(p), ctypes.byref(n)):
            return None
        ffi = ctypes.string_at(p.value, n.value)
        ms, ls = struct.unpack_from('<II', ffi, 8)
        return ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF
    except (AttributeError, OSError):
        return None


def read_header(path):
    """TES4 header of a plugin: dict(flags, masters, author) or a string saying what is wrong."""
    try:
        with open(path, 'rb') as f:
            head = f.read(24)
            if len(head) < 24 or head[:4] != b'TES4':
                return 'not a valid plugin file (no TES4 header)'
            size, flags = struct.unpack_from('<II', head, 4)
            data = f.read(size)
    except OSError as e:
        return 'could not be read (%s)' % e.strerror
    if len(data) < size:
        return 'is cut off (file shorter than its header says)'
    masters, author, pos, big = [], '', 0, None
    while pos + 6 <= len(data):
        typ, n = data[pos:pos + 4], struct.unpack_from('<H', data, pos + 4)[0]
        pos += 6
        if big is not None:                       # XXXX: the next field is longer than 65535 bytes
            n, big = big, None
        field = data[pos:pos + n]
        pos += n
        if typ == b'XXXX' and n == 4:
            big = struct.unpack('<I', field)[0]
        elif typ == b'MAST':
            masters.append(field.split(b'\0', 1)[0].decode('cp1252', errors='replace'))
        elif typ == b'CNAM':
            author = field.split(b'\0', 1)[0].decode('cp1252', errors='replace')
    return {'flags': flags, 'masters': masters, 'author': author}


def read_plugins_txt(path):
    """Active and inactive entries of plugins.txt, in file order: list of (name, active)."""
    text = read_text(path)
    if text is None:
        return None
    out = []
    for line in text.lstrip('\ufeff').splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        out.append((line[1:].strip(), True) if line.startswith('*') else (line, False))
    return out


# ------------------------------------------------------------------ load order
def build_load_order(data, plugins_txt):
    """Returns (order, info, notes). order: names in the engine's load order. info: lower name -> dict."""
    files = {}
    for f in os.listdir(data):
        if f.lower().endswith(('.esm', '.esp', '.esl')) and os.path.isfile(os.path.join(data, f)):
            files[f.lower()] = f
    info, notes = {}, []
    for low, name in files.items():
        h = read_header(os.path.join(data, name))
        d = {'name': name, 'bad': None, 'masters': [], 'author': '', 'master': False, 'light': False, 'why': ''}
        if isinstance(h, str):
            d['bad'] = h
        else:
            d.update(masters=h['masters'], author=h['author'])
            d['master'] = bool(h['flags'] & FLAG_MASTER) or low.endswith(('.esm', '.esl'))
            d['light'] = bool(h['flags'] & FLAG_LIGHT) or low.endswith('.esl')
        info[low] = d

    implicit = [n for n in HARDCODED if n.lower() in files]
    ccc = read_text(os.path.join(os.path.dirname(data), 'Fallout4.ccc')) or ''
    implicit += [n.strip() for n in ccc.splitlines() if n.strip() and n.strip().lower() in files
                 and n.strip().lower() not in {i.lower() for i in implicit}]
    for n in implicit:
        info[n.lower()]['why'] = 'game' if n in HARDCODED else 'Creation Club'

    seen = {n.lower() for n in implicit}
    listed = []
    for name, active in plugins_txt or []:
        low = name.lower()
        if low in seen:
            continue
        seen.add(low)
        if low not in files:
            if active:
                notes.append(('warn', '%s is switched on in plugins.txt but the file is not in the Data folder. The game '
                                      'skips it. If you uninstalled that mod, untick it in your mod manager.' % name))
            continue
        if active:
            listed.append(files[low])
            info[low]['why'] = 'active'
        else:
            info[low]['why'] = 'inactive'

    # The engine loads master-flagged plugins (.esm, .esl, ESM-flagged .esp) before all other plugins
    masters = [n for n in listed if info[n.lower()]['master']]
    others = [n for n in listed if not info[n.lower()]['master']]
    if listed != masters + others:
        moved = [n for n in masters if listed.index(n) > min((listed.index(o) for o in others), default=len(listed))]
        notes.append(('info', 'plugins.txt lists %d master plugin(s) after normal plugins (%s). The game always loads masters '
                              'first, so their real position is earlier than plugins.txt shows. Usually harmless; LOOT '
                              'sorts this out.' % (len(moved), ', '.join(moved[:5]) + (' ...' if len(moved) > 5 else ''))))
    order = implicit + masters + others
    for low, d in info.items():
        if not d['why']:
            d['why'] = 'not in plugins.txt'
    return order, info, notes


def check_masters(order, info):
    """Missing / disabled / late masters for every loaded plugin: list of (plugin, master, problem)."""
    pos = {n.lower(): i for i, n in enumerate(order)}
    problems = []
    for i, name in enumerate(order):
        for m in info[name.lower()]['masters']:
            ml = m.lower()
            if ml not in info:
                problems.append((name, m, 'missing'))
            elif ml not in pos:
                problems.append((name, m, 'off'))
            elif pos[ml] > i:
                problems.append((name, m, 'late'))
    return problems


# ------------------------------------------------------------------ F4SE plugins
F4SE_LINE = re.compile(r'^plugin (\S+\.dll) \((\w+) (.*?) (\w+)\) (.+?)(?: -?\d+)?(?: \(handle \d+\))?$', re.I)


def pe_export(path, name):
    """(file bytes, file offset of the named export) of a 64-bit DLL; offset is None if absent. Raises on a bad file."""
    b = open(path, 'rb').read()
    e = struct.unpack_from('<I', b, 0x3C)[0]
    if b[:2] != b'MZ' or b[e:e + 4] != b'PE\0\0':
        raise ValueError('not a DLL')
    nsec, optsz = struct.unpack_from('<H', b, e + 6)[0], struct.unpack_from('<H', b, e + 20)[0]
    opt = e + 24
    exp_rva = struct.unpack_from('<I', b, opt + 112)[0]
    secs = [struct.unpack_from('<IIII', b, opt + optsz + i * 40 + 8) for i in range(nsec)]

    def off(rva):
        for vsz, va, rsz, raw in secs:
            if va <= rva < va + max(vsz, rsz):
                return rva - va + raw
        raise ValueError('bad address')
    if not exp_rva:
        return b, None
    d = off(exp_rva)
    nnames, afun, aname, aord = struct.unpack_from('<IIII', b, d + 24)
    for i in range(nnames):
        p = off(struct.unpack_from('<I', b, off(aname) + 4 * i)[0])
        if b[p:b.index(b'\0', p)] == name.encode():
            o = struct.unpack_from('<H', b, off(aord) + 2 * i)[0]
            return b, off(struct.unpack_from('<I', b, off(afun) + 4 * o)[0])
    return b, None


def f4se_will_load(path, game_ver):
    """Predict F4SE 0.7.x's verdict from the DLL's F4SEPlugin_Version block, using the same rule as F4SE's
    PluginManager: (True, '') = loads, (False, reason) = skipped, (None, reason) = can't tell."""
    try:
        b, p = pe_export(path, 'F4SEPlugin_Version')
    except (OSError, ValueError, struct.error, IndexError):
        return None, 'not a readable DLL'
    if p is None:
        return False, ('made for the old game (before the 2024 next-gen update): it has no F4SE version data, so '
                       'this F4SE skips it ("no version data")')
    data_ver = struct.unpack_from('<I', b, p)[0]
    addr, structs = struct.unpack_from('<II', b, p + 520)
    runtimes = [v for v in struct.unpack_from('<16I', b, p + 528) if v]
    breaking = struct.unpack_from('<I', b, p + 600)[0]
    if not data_ver:
        return False, 'bad F4SE version data'
    # kAddressIndependence_Signatures | _AddressLibrary_1_11_137, kStructureIndependence_NoStructs | _1_11_137Layout
    if addr & 0x5 and structs & 0x5 and not breaking:
        return True, ''
    if not game_ver:
        return None, 'game version unknown'
    runtime = (game_ver[0] << 24) | (game_ver[1] << 16) | (game_ver[2] << 4)
    for v in runtimes:
        if v == runtime:
            return True, ''
    made_for = ', '.join(sorted({'%d.%d.%d' % (v >> 24, (v >> 16) & 0xFF, (v >> 4) & 0xFFF) for v in runtimes}))
    return False, 'made for game version %s, not yours' % (made_for or 'unknown')


def check_f4se(rep, data, logdir, game_ver):
    rep.h('3. F4SE plugins (Data\\F4SE\\Plugins)')
    game = os.path.dirname(data)
    plug = os.path.join(data, 'F4SE', 'Plugins')
    problems = 0
    loader = os.path.isfile(os.path.join(game, 'f4se_loader.exe'))
    vstr = '%d_%d_%d' % game_ver[:3] if game_ver else None
    core = vstr and os.path.isfile(os.path.join(game, 'f4se_%s.dll' % vstr))
    rep.p('Game version: %s.' % ('.'.join(map(str, game_ver)) if game_ver else 'unknown (could not read Fallout4.exe)'))
    if not loader:
        rep.p('PROBLEM: f4se_loader.exe is not in the game folder, so F4SE is not installed. F4SE plugins (including CEF) '
              'cannot load without it.')
        problems += 1
    elif vstr and not core:
        rep.p('PROBLEM: F4SE is installed but has no f4se_%s.dll for this game version. Install the F4SE build made for '
              'this exact game version.' % vstr)
        problems += 1
    else:
        rep.p('F4SE: installed%s. Remember to start the game with f4se_loader.exe (or your mod manager\'s F4SE entry).'
              % (' for this game version' if core else ''))
    if game_ver:
        abin = 'version-%d-%d-%d-%d.bin' % game_ver
        if os.path.isfile(os.path.join(plug, abin)):
            rep.p('Address Library: installed (%s).' % abin)
        else:
            others = [f for f in os.listdir(plug) if f.lower().startswith('version-')] if os.path.isdir(plug) else []
            rep.p('PROBLEM: Address Library for this game version (%s) is missing%s. Most F4SE plugins, CEF included, '
                  'need it.' % (abin, ' (found only: %s)' % ', '.join(others) if others else ''))
            problems += 1

    dlls = sorted(f for f in os.listdir(plug) if f.lower().endswith('.dll')) if os.path.isdir(plug) else []
    log = os.path.join(logdir, 'f4se.log')
    text = read_text(log)
    status = {}
    scanned = set()
    for line in (text or '').splitlines():
        m = re.match(r'^checking plugin (.+\.dll)$', line.strip(), re.I)
        if m:
            scanned.add(os.path.basename(m.group(1)).lower())
        m = F4SE_LINE.match(line.strip())
        if m:
            status[m.group(1).lower()] = (m.group(3), m.group(4), m.group(5))
        m = re.match(r"^couldn't load plugin (.+)$", line.strip(), re.I)
        if m:
            status[os.path.basename(m.group(1)).lower()] = ('', '', "couldn't be loaded by Windows")
    rep.p('')
    if not dlls:
        rep.p('No F4SE plugin DLLs installed.')
    else:
        rep.p('%d DLL(s). OK / PROBLEM "F4SE: ..." is what F4SE wrote in f4se.log at the last launch%s; for DLLs F4SE '
              'hasn\'t seen yet, Mod Doctor reads the DLL\'s F4SE version data and predicts the result:'
              % (len(dlls), '' if text is not None else ' (no f4se.log yet)'))
        for f in dlls:
            st = status.get(f.lower())
            if st and st[2].lower().startswith('loaded correctly'):
                line = 'OK       %s  (%s, version %s)' % (f, st[0], st[1])
            elif st:
                line = 'PROBLEM  %s  F4SE: "%s"' % (f, st[2])
                problems += 1
            elif f.lower() in scanned:
                line = 'PROBLEM  %s  F4SE checked it at the last launch but did not load it' % f
                problems += 1
            else:
                ok, why = f4se_will_load(os.path.join(plug, f), game_ver)
                if ok:
                    line = 'NEW      %s  not launched yet; its version data says F4SE will load it' % f
                elif ok is None:
                    line = 'NEW      %s  not launched yet; can\'t predict (%s)' % (f, why)
                else:
                    line = 'PROBLEM  %s  will NOT load: %s' % (f, why)
                    problems += 1
            rep.raw('  ' + line)
        if problems:
            rep.p('')
            rep.p('A DLL that doesn\'t load is skipped, not a crash, but the mod that needs it won\'t work (often its '
                  'menus, hotkeys or script functions fail). "incompatible with current version of the game" or "made '
                  'for ..." = built for a different game version; "requires newer script extender" = update F4SE. Look '
                  'for the version of that mod made for your game version, or remove it.')
    stray = [f for f in os.listdir(os.path.join(data, 'F4SE')) if f.lower().endswith('.dll')] \
        if os.path.isdir(os.path.join(data, 'F4SE')) else []
    for f in stray:
        rep.p('WARNING: %s is in Data\\F4SE, not in Data\\F4SE\\Plugins, so F4SE ignores it. Move it into Plugins if it is '
              'an F4SE plugin.' % f)
    return problems


# ------------------------------------------------------------------ known start-up crash files
# Both proven with crash logs and launch tests on game 1.11.240 (CEF tests, 2026-11-02); both are next-gen problems.
NG_MAINMENU_NAMES = ('InstalledContentPanel_mc', 'InstalledContentList_mc', 'Upsell_mc')


def swf_body(b):
    """Uncompressed bytes of a Flash movie (FWS plain, CWS zlib, ZWS LZMA)."""
    if b[:3] == b'FWS':
        return b
    if b[:3] == b'CWS':
        return b[:8] + zlib.decompress(b[8:])
    if b[:3] == b'ZWS':   # LZMA props at 12..16, data from 17; rebuild an .lzma ("alone") header for the decoder
        size = struct.unpack_from('<I', b, 4)[0] - 8
        return b[:8] + lzma.decompress(b[12:17] + struct.pack('<Q', size) + b[17:], format=lzma.FORMAT_ALONE)
    raise ValueError('not a Flash file')


def vortex_owners(data):
    """{relative path (lower case): Vortex mod name} from Vortex's deployment record, or {} without Vortex."""
    try:
        with open(os.path.join(data, 'vortex.deployment.json'), encoding='utf-8') as f:
            return {e['relPath'].lower(): e['source'] for e in json.load(f).get('files', [])}
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return {}


def check_crash_files(rep, data, game_ver):
    rep.h('4. Known start-up crash files')
    if game_ver and tuple(game_ver[:3]) < (1, 10, 980):
        rep.p('Skipped: these checks are for the next-gen game (1.10.980 and newer).')
        return 0
    problems = 0
    owners = vortex_owners(data)

    def made_by(rels):
        mods = sorted({owners[r.lower()] for r in rels if r.lower() in owners})
        return ' (from %s)' % ', '.join(mods) if mods else ''

    rel = os.path.join('Interface', 'MainMenu.swf')
    menu = os.path.join(data, rel)
    if os.path.isfile(menu):
        try:
            with open(menu, 'rb') as f:
                body = swf_body(f.read())
            missing = [n for n in NG_MAINMENU_NAMES if n.encode() not in body]
        except (OSError, ValueError, zlib.error, lzma.LZMAError, struct.error) as e:
            missing = None
            rep.p('NOTE: a mod replaces the main menu with Data\\%s%s, but it could not be checked (%s).'
                  % (rel, made_by([rel]), e))
        if missing:
            problems += 1
            rep.p('PROBLEM: Data\\%s%s is an old main menu made for the pre-next-gen game: it lacks %s. The game looks '
                  'those up when it builds the main menu, so it crashes at start-up (seen on game 1.11.240). Remove that '
                  'file (BackportedBA2Support ships one); the game then uses its own main menu.'
                  % (rel, made_by([rel]), ', '.join(missing)))
        elif missing == []:
            rep.p('Main menu replaced by a mod with Data\\%s%s: it has the next-gen parts. OK.' % (rel, made_by([rel])))

    atd = os.path.join(data, 'Meshes', 'AnimTextData')
    subs = sorted(d for d in os.listdir(atd) if os.path.isdir(os.path.join(atd, d))) if os.path.isdir(atd) else []
    if subs:
        problems += 1
        files = [os.path.relpath(os.path.join(dp, f), data) for dp, _, fs in os.walk(atd) for f in fs]
        files = [f for f in files if not f.endswith('__folder_managed_by_vortex')]
        rep.p('PROBLEM: loose animation data in Data\\Meshes\\AnimTextData\\ (%s; %d file(s))%s. On the next-gen game '
              'any folder here, even an empty one, makes the game crash at start-up (seen on game 1.11.240); the same '
              'files inside a mod\'s .ba2 archive are fine. Remove these folders (that mod\'s weapons or creatures may '
              'then animate wrongly), or use a version of the mod that packs them in a .ba2. Mod managers can leave the '
              'folders behind empty after you uninstall a mod: delete those too.'
              % (', '.join(subs), len(files), made_by(files)))
    if not problems:
        rep.p('No known start-up crash files found (old main menu, loose animation data).')
    return problems


# ------------------------------------------------------------------ game settings
def ini_value(paths, section, key):
    """Last value of section/key across the INIs in load order (later files win), or None."""
    val = None
    for path in paths:
        cur = None
        for line in (read_text(path) or '').splitlines():
            line = line.strip()
            if line.startswith('[') and line.endswith(']'):
                cur = line[1:-1].strip().lower()
            elif cur == section.lower() and '=' in line and not line.startswith(';'):
                k, v = line.split('=', 1)
                if k.strip().lower() == key.lower():
                    val = v.strip()
    return val


def check_settings(rep, data, mygames):
    rep.h('5. Game settings (Documents\\My Games\\Fallout4)')
    inis = [os.path.join(mygames, n) for n in ('Fallout4.ini', 'Fallout4Prefs.ini', 'Fallout4Custom.ini')]
    problems = 0
    if not os.path.isfile(inis[0]):
        rep.p('Fallout4.ini not found in %s. Start the game once from the launcher so it creates its settings.' % mygames)
        return 0
    invalidate = ini_value(inis, 'Archive', 'bInvalidateOlderFiles')
    final = ini_value(inis, 'Archive', 'sResourceDataDirsFinal')
    scripts = os.path.join(data, 'Scripts')
    loose = os.path.isdir(scripts) and any(f.lower().endswith('.pex') for f in os.listdir(scripts))
    if invalidate == '1' and final == '':
        rep.p('Loose files: enabled (bInvalidateOlderFiles=1, sResourceDataDirsFinal empty). Good.')
    else:
        problems += loose
        rep.p('%s: loose files are not enabled (bInvalidateOlderFiles=%s, sResourceDataDirsFinal=%s). The game then ignores '
              'loose mod files, including F4SE\'s scripts in Data\\Scripts, so mods that use F4SE script functions (Sim '
              'Settlements 2, Workshop Framework, MCM...) break. Add to Fallout4Custom.ini:'
              % ('PROBLEM' if loose else 'NOTE', invalidate if invalidate is not None else '(not set)',
                 final if final is not None else '(not set)'))
        for line in ('[Archive]', 'bInvalidateOlderFiles=1', 'sResourceDataDirsFinal='):
            rep.p(line, 4)
    if ini_value(inis, 'Launcher', 'bEnableFileSelection') != '1':
        rep.p('NOTE: bEnableFileSelection=1 is not set under [Launcher] in Fallout4Prefs.ini. Without it the game may '
              'ignore plugins.txt and switch your mods off. Mod managers normally set it.')
    else:
        rep.p('Plugin selection: enabled (bEnableFileSelection=1). Good.')
    if ini_value(inis, 'Papyrus', 'bEnableLogging') == '1':
        rep.p('Papyrus script logging: ON (for the CEF diagnostics; costs a little performance).')
    return problems


# ------------------------------------------------------------------ NPC outfits
# Outfit (OTFT) -> leveled lists (LVLI) -> armor (ARMO) -> armor addon (ARMA) -> mesh. Found with the naked-Minutemen case
# (2026-11-02): a later mod emptied a leveled list another mod's outfit still used for its uniform.
OUTFIT_TYPES = (b'NPC_', b'OTFT', b'LVLI', b'ARMO', b'ARMA')
HUMAN_RACE = ('fallout4.esm', 0x013746)
ARCHIVE_KEYS = ('sresourcearchivelist', 'sresourcearchivelist2', 'sresourceindexfilelist', 'sresourcestartuparchivelist',
                'sresourcearchivememorycachelist')


def fields(d):
    """Fields (subrecords) of one record's data: list of (type, bytes)."""
    out, pos, big = [], 0, None
    while pos + 6 <= len(d):
        typ, n = d[pos:pos + 4], struct.unpack_from('<H', d, pos + 4)[0]
        pos += 6
        if big is not None:
            n, big = big, None
        if typ == b'XXXX' and n == 4:
            big = struct.unpack_from('<I', d, pos)[0]
        else:
            out.append((typ, d[pos:pos + n]))
        pos += n
    return out


class Forms:
    """Every form the active plugins define, and every version (in load order) of the records outfits are made of:
    versions[form] = [(plugin, its masters + itself, fields, record type)]. A form is (lower-case plugin that created
    it, 24-bit object id)."""

    def __init__(self, data, order, info):
        self.defined, self.versions, self.names = set(), {}, {}
        for name in order:
            files = [m.lower() for m in info[name.lower()]['masters']] + [name.lower()]
            self.names[name.lower()] = name
            try:
                with open(os.path.join(data, name), 'rb') as f:
                    b = f.read()
                self._walk(b, 24 + struct.unpack_from('<I', b, 4)[0], len(b), name.lower(), files, True)
            except (OSError, struct.error):
                continue

    @staticmethod
    def key(files, fid):
        # Like the engine, an index past the masters list means the plugin itself (some mods ship such IDs)
        return files[min(fid >> 24, len(files) - 1)], fid & 0xFFFFFF

    def _walk(self, b, pos, end, plugin, files, top):
        while pos + 24 <= end:
            typ, size = b[pos:pos + 4], struct.unpack_from('<I', b, pos + 4)[0]
            if typ == b'GRUP':
                self._walk(b, pos + 24, pos + size, plugin, files, False)
                pos += size
                continue
            flags, fid = struct.unpack_from('<II', b, pos + 8)
            k = self.key(files, fid)
            (self.defined.discard if flags & 0x20 else self.defined.add)(k)        # 0x20: deleted record
            if typ in OUTFIT_TYPES:
                d = b[pos + 24:pos + 24 + size]
                if flags & 0x40000:                                                 # compressed
                    try:
                        d = zlib.decompress(d[4:])
                    except zlib.error:
                        d = b''
                self.versions.setdefault(k, []).append((plugin, files, [] if flags & 0x20 else fields(d), typ))
            pos += 24 + size

    def win(self, k):
        v = self.versions.get(k)
        return v[-1] if v else None

    def ref(self, rec, raw):
        fid = struct.unpack_from('<I', raw)[0]
        return self.key(rec[1], fid) if fid else None

    def edid(self, k):
        rec = self.win(k)
        for typ, v in rec[2] if rec else []:
            if typ == b'EDID':
                return v.split(b'\0', 1)[0].decode('cp1252', errors='replace')
        return ''

    def label(self, k):
        """'EditorID (Plugin.esp 0B23D2)'."""
        return '%s (%s %06X)' % (self.edid(k) or 'no editor ID', self.names.get(k[0], k[0]), k[1])


def ba2_names(path):
    """File names inside a general (non-texture) .ba2 archive, lower case; [] for texture archives or bad files."""
    try:
        with open(path, 'rb') as f:
            head = f.read(24)
            if head[:4] != b'BTDX' or head[8:12] != b'GNRL':
                return []
            count, names_at = struct.unpack_from('<IQ', head, 12)
            f.seek(names_at)
            blob = f.read()
        out, pos = [], 0
        for _ in range(count):
            n = struct.unpack_from('<H', blob, pos)[0]
            out.append(blob[pos + 2:pos + 2 + n].decode('cp1252', errors='replace').lower().replace('/', '\\'))
            pos += 2 + n
        return out
    except (OSError, struct.error):
        return []


def visible_meshes(data, order, mygames):
    """Mesh paths the game can load (Data-relative, lower case): loose files plus the archives it opens, which are the
    ones named in the INIs, every "Fallout4 - *.ba2", and "<plugin> - *.ba2" for active plugins."""
    present = set()
    for dp, _, fs in os.walk(os.path.join(data, 'Meshes')):
        rel = os.path.relpath(dp, data).lower()
        present.update(rel + '\\' + f.lower() for f in fs)
    listed = set()
    for path in [os.path.join(os.path.dirname(data), 'Fallout4_Default.ini')] + \
            [os.path.join(mygames, n) for n in ('Fallout4.ini', 'Fallout4Custom.ini')]:
        for line in (read_text(path) or '').splitlines():
            k, _, v = line.partition('=')
            if k.strip().lower() in ARCHIVE_KEYS:
                listed.update(x.strip().lower() for x in v.split(',') if x.strip())
    stems = [os.path.splitext(n)[0].lower() + ' - ' for n in order]
    for f in os.listdir(data):
        low = f.lower()
        if low.endswith('.ba2') and (low in listed or low.startswith('fallout4 - ') or low.startswith(tuple(stems))):
            present.update(n for n in ba2_names(os.path.join(data, f)) if n.startswith('meshes\\'))
    return present


def mesh_path(raw):
    p = raw.split(b'\0', 1)[0].decode('cp1252', errors='replace').strip().replace('/', '\\').lower().lstrip('\\')
    if p.startswith('data\\'):
        p = p[5:]
    return p if p.startswith('meshes\\') else 'meshes\\' + p


def check_outfits(rep, data, order, info, mygames):
    rep.h('6. NPC outfits (naked NPCs, invisible clothes)')
    forms = Forms(data, order, info)
    present = visible_meshes(data, order, mygames)
    worn = {}                                   # outfit -> NPC editor IDs
    for k, vers in forms.versions.items():
        rec = vers[-1]
        if rec[3] == b'NPC_':
            for typ, v in rec[2]:
                if typ in (b'DOFT', b'SOFT') and len(v) == 4:
                    o = forms.ref(rec, v)
                    if o:
                        worn.setdefault(o, []).append(forms.edid(k))
    emptied, missing, meshes = {}, {}, {}       # problem -> outfits it affects

    def follow(k, holder, outfit, seen, depth=0):
        if k is None or k in seen or depth > 20:
            return
        seen.add(k)
        if k not in forms.defined:
            missing.setdefault((holder, k), set()).add(outfit)
            return
        rec = forms.win(k)
        if not rec:
            return
        flds = rec[2]
        if rec[3] == b'LVLI':
            entries = [forms.ref(rec, v[4:8]) for t, v in flds if t == b'LVLO' and len(v) >= 8]
            if not entries and any(any(t == b'LVLO' for t, _ in v[2]) for v in forms.versions[k][:-1]):
                emptied.setdefault(k, set()).add(outfit)
            for e in entries:
                follow(e, k, outfit, seen, depth + 1)
        elif rec[3] == b'ARMO':
            for t, v in flds:
                if t != b'MODL' or len(v) != 4:
                    continue
                addon = forms.ref(rec, v)
                if addon not in forms.defined:
                    missing.setdefault((k, addon), set()).add(outfit)
                    continue
                arec = forms.win(addon)
                if not arec or arec[3] != b'ARMA' or HUMAN_RACE not in [forms.ref(arec, x) for tt, x in arec[2] if tt in (b'RNAM', b'MODL')
                                                  and len(x) == 4]:
                    continue
                for tt, x in arec[2]:
                    if tt in (b'MOD2', b'MOD3') and x.strip(b'\0 ') and mesh_path(x) not in present:
                        meshes.setdefault((k, 'male' if tt == b'MOD2' else 'female', mesh_path(x)), set()).add(outfit)

    for o in worn:
        rec = forms.win(o)
        if o not in forms.defined:
            missing.setdefault((None, o), set()).add(o)
            continue
        seen = set()
        for t, v in rec[2] if rec else []:
            if t == b'INAM':
                for i in range(0, len(v) - 3, 4):
                    follow(forms.ref(rec, v[i:i + 4]), o, o, seen)

    def who(outfits):
        outs = sorted(outfits, key=forms.edid)
        npcs = sorted({n or '?' for o in outs for n in worn.get(o, [])})
        return ('outfit%s %s, worn by %d NPC record(s): %s' % (
            's' if len(outs) > 1 else '', ', '.join(forms.label(o) for o in outs[:3]) + (' ...' if len(outs) > 3 else ''),
            len(npcs), ', '.join(npcs[:5]) + (' ...' if len(npcs) > 5 else '')))

    warnings = 0
    for k, outs in sorted(emptied.items(), key=lambda kv: forms.label(kv[0])):
        vers = forms.versions[k]
        filled = [forms.names.get(p, p) for p, _, f, _ in vers[:-1] if any(t == b'LVLO' for t, _ in f)]
        rep.p('WARNING: EMPTIED LIST: %s empties the leveled list %s, which %s filled. It is still used by %s. Those NPCs '
              'get nothing from that list; if it held their uniform, they are naked. Look for a compatibility patch for '
              'these mods, or keep only one of them.' % (forms.names.get(vers[-1][0], vers[-1][0]), forms.label(k),
                                                          ', '.join(filled), who(outs)))
        warnings += 1
    for (holder, k), outs in sorted(missing.items(), key=lambda kv: str(kv[0])):
        what = 'form %06X from %s' % (k[1], forms.names.get(k[0], k[0]))
        rep.p('WARNING: MISSING ITEM: %s points at %s, which no active plugin defines (the plugin was removed or loads '
              'in a different order than the mod expects). Affects %s.' % (forms.label(holder) if holder else 'an NPC',
                                                                         what, who(outs)))
        warnings += 1
    shown = sorted(meshes.items(), key=lambda kv: forms.label(kv[0][0]))
    for (armor, sex, path), outs in shown[:15]:
        rep.p('WARNING: MISSING MESH: armor %s has no %s mesh (%s is not loose or in a loaded archive), so it is '
              'invisible on %s NPCs. Affects %s. Reinstall that mod (all its files).' % (forms.label(armor), sex, path, sex,
                                                                                        who(outs)))
        warnings += 1
    if len(shown) > 15:
        rep.p('... and %d more missing armor meshes of the same kind.' % (len(shown) - 15))
        warnings += len(shown) - 15
    if not warnings:
        rep.p('Checked %d outfits worn by NPCs: no emptied leveled lists, missing items or missing armor meshes.'
              % len(worn))
    else:
        rep.p('')
        rep.p('Checked %d outfits worn by NPCs. These don\'t crash the game; they leave NPCs naked or wearing invisible '
              'items. Outfits a mod leaves empty on purpose (captives, corpses) are not reported.' % len(worn))
    return warnings


# ------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser(description='Check Fallout 4 plugins, masters and F4SE plugins. Reads local files only.')
    ap.add_argument('--data'); ap.add_argument('--plugins-txt'); ap.add_argument('--mygames'); ap.add_argument('--no-save', action='store_true')
    a = ap.parse_args()

    rep = Report()
    rep.lines += ['CEF Mod Doctor %s - local check of your mod setup. Nothing was sent anywhere.' % VERSION]
    data = a.data or find_data_folder()
    if not data or not os.path.isfile(os.path.join(data, 'Fallout4.esm')):
        rep.p('Could not find the Fallout 4 Data folder (no Fallout4.esm). Run this from Data\\F4SE\\Plugins\\CEF, or use '
              '--data "C:\\path\\to\\Fallout 4\\Data".')
        print(rep.text())
        return 1
    ptxt = a.plugins_txt or os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Fallout4', 'plugins.txt')
    entries = read_plugins_txt(ptxt)
    mygames = a.mygames or os.path.join(documents_folder(), 'My Games', 'Fallout4')
    logdir = os.path.join(mygames, 'F4SE')
    rep.lines += ['Data folder:  %s' % data, 'plugins.txt:  %s%s' % (ptxt, '' if entries is not None else ' (not found)')]

    order, info, notes = build_load_order(data, entries)
    master_problems = check_masters(order, info)
    bad_files = [d for d in info.values() if d['bad']]
    full = [n for n in order if not info[n.lower()]['light']]
    light = [n for n in order if info[n.lower()]['light']]

    # 1. problems
    rep.h('1. Problems')
    errors = 0
    for plugin, m, kind in master_problems:
        errors += 1
        if kind == 'missing':
            rep.p('MISSING MASTER: %s needs %s, which is not installed. Install %s (or remove %s). Without it the game '
                  'usually crashes at start-up or freezes on the loading screen.' % (plugin, m, m, plugin))
        elif kind == 'off':
            rep.p('MASTER SWITCHED OFF: %s needs %s, which is installed but not active. Switch %s on in your mod manager '
                  '(or switch %s off).' % (plugin, m, m, plugin))
        else:
            rep.p('MASTER LOADS TOO LATE: %s needs %s, but %s loads after it. Move %s above %s (LOOT can sort this).'
                  % (plugin, m, m, m, plugin))
    for d in bad_files:
        loaded = d['name'] in order
        errors += loaded
        rep.p('%s: %s %s.%s' % ('BROKEN PLUGIN' if loaded else 'WARNING', d['name'], d['bad'],
                                ' Reinstall that mod.' if loaded else ' It is not active, so the game ignores it.'))
    if len(full) > MAX_FULL:
        errors += 1
        rep.p('TOO MANY PLUGINS: %d full (non-light) plugins are active; the game can load at most %d. Merge plugins, use '
              'ESL-flagged versions, or remove some.' % (len(full), MAX_FULL))
    if len(light) > MAX_LIGHT:
        errors += 1
        rep.p('TOO MANY LIGHT PLUGINS: %d active; the limit is %d.' % (len(light), MAX_LIGHT))
    for kind, text in notes:
        rep.p(('WARNING: ' if kind == 'warn' else 'NOTE: ') + text)
    if not errors and not notes:
        rep.p('No problems found in your plugins.')
    elif not errors:
        rep.p('No serious problems found in your plugins.')

    # 2. load order
    rep.h('2. Load order (%d active: %d full, %d light)' % (len(order), len(full), len(light)))
    missing_by_plugin = {}
    for plugin, m, kind in master_problems:
        missing_by_plugin.setdefault(plugin, []).append(m)
    fi = li = 0
    for n in order:
        d = info[n.lower()]
        if d['light']:
            idx, li = 'FE:%03X' % li, li + 1
        else:
            idx, fi = '%02X    ' % fi, fi + 1
        ext = n[-3:].upper()                      # file type, plus the flag that changes how it loads
        kind = ext + ('(ESL)' if d['light'] and ext != 'ESL' else '(ESM)' if d['master'] and ext == 'ESP' else '')
        tag = {'game': '  [game]', 'Creation Club': '  [Creation Club]'}.get(d['why'], '')
        flag = '  <-- needs %s' % ', '.join(missing_by_plugin[n]) if n in missing_by_plugin else ''
        rep.raw('  %s  %-8s %s%s%s' % (idx, kind, n, tag, flag))
    off = sorted((d['name'] for d in info.values() if d['name'] not in order), key=str.lower)
    if off:
        rep.p('')
        rep.p('Installed but not active (%d): %s' % (len(off), ', '.join(off)))

    # 3. F4SE
    game_ver = exe_version(os.path.join(os.path.dirname(data), 'Fallout4.exe'))
    errors += check_f4se(rep, data, logdir, game_ver)

    errors += check_crash_files(rep, data, game_ver)

    errors += check_settings(rep, data, mygames)

    warnings = check_outfits(rep, data, order, info, mygames)

    rep.h('Summary')
    rep.p('%s' % ('All checks passed.' if not errors else '%d problem(s) found: fix the PROBLEM / MISSING lines above '
                                                           'before you play.' % errors))
    if warnings:
        rep.p('%d outfit warning(s) in section 6: they won\'t stop the game, but some NPCs will be naked or wear invisible '
              'items.' % warnings)
    rep.p('This checks files and load order only. It can\'t tell whether two mods conflict inside the game; for crashes and '
          'script errors while playing, run "Run CEF Diagnostics.bat".')

    out = rep.text()
    user = os.environ.get('USERNAME')
    if user and len(user) > 2:
        out = re.sub(re.escape(user), '<user>', out, flags=re.I)
    out = out.encode('ascii', 'replace').decode('ascii')
    print(out)
    if not a.no_save and os.path.isdir(logdir):
        target = os.path.join(logdir, 'CEF-ModDoctor.txt')
        try:
            with open(target, 'w', encoding='ascii', errors='replace') as f:
                f.write(out)
            print('Saved: %s' % re.sub(re.escape(user or '\0'), '<user>', target, flags=re.I))
        except OSError as e:
            print('Could not save the report: %s' % e)
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
