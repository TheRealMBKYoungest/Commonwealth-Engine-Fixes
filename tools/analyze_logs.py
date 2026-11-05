"""Commonwealth Engine Fixes (CEF) log analyzer, 1.0.2.

Reads, on this PC only:
  - CEF crash logs      Documents\\My Games\\Fallout4\\F4SE\\CEF-Crash-*.log   (or Buffout 4's crash-*.log if you use Buffout)
  - CEF plugin logs     FCStruct2.log, FCShadow2.log, CEFCrashLog.log, f4se.log   (same folder)
  - the Papyrus log     Documents\\My Games\\Fallout4\\Logs\\Script\\Papyrus.0.log

and explains in plain English what they show. It only reads text files. It uses the Python standard library only and
contains no network code: nothing is uploaded, sent or downloaded. The result is printed and saved next to the logs as
CEF-Diagnosis.txt, with your Windows user name replaced by <user>.

Run:  double-click "Run CEF Diagnostics.bat", or:  python analyze_logs.py
Options: --crash FILE   analyze this crash log instead of the newest one
         --papyrus FILE use this Papyrus log
         --logs DIR     folder that holds the F4SE logs (default: found automatically)
         --no-save      print only, don't write CEF-Diagnosis.txt
"""
import argparse
import collections
import ctypes
import glob
import os
import re
import sys

VERSION = '1.0.2'

# ------------------------------------------------------------------ knowledge base
EXCEPTIONS = {
    0xC0000005: ('Access violation',
                 'The game tried to read or write memory it does not own. This is the most common crash. It usually means '
                 'some code used an object that was already gone or never existed: a missing or broken mod file, a mod '
                 'that edits something another mod removed, or a plugin DLL that does not match your game version.'),
    0xC00000FD: ('Stack overflow',
                 'A function kept calling itself until the thread ran out of stack space. This usually comes from a loop in '
                 'game data (for example two records pointing at each other) or a plugin DLL bug.'),
    0xC000001D: ('Illegal instruction',
                 'The CPU was asked to run something that is not valid code. Usually a plugin DLL that patched the game '
                 'incorrectly, or a DLL built for a different game version.'),
    0xC0000374: ('Heap corruption',
                 'Memory bookkeeping was damaged earlier by some code, and Windows noticed later. The module where it was '
                 'noticed is usually NOT the one that caused it.'),
    0xC0000409: ('Stack buffer overrun / fast fail',
                 'A security check stopped the game on purpose. Often a corrupted save, a broken plugin DLL, or the C runtime '
                 'reacting to an invalid parameter.'),
    0xE06D7363: ('Uncaught C++ exception',
                 'Some code raised an error and nothing handled it. Common causes: running out of memory (std::bad_alloc) '
                 'or a plugin DLL reporting a problem it could not recover from.'),
    0xC0000006: ('In-page error',
                 'Windows could not read part of a file from disk while the game was using it. Check the drive the game '
                 'or your page file is on (disk errors, an unplugged external drive, or a full disk).'),
    0xC0000094: ('Integer divide by zero', 'Code divided by zero. Usually a plugin DLL bug or a broken value in game data.'),
    0x80000003: ('Breakpoint', 'Code stopped itself on purpose (an assertion). Usually a plugin DLL reporting a fatal problem.'),
    0xC0000017: ('Out of memory', 'Windows could not give the game more memory. See the memory section below.'),
}

# module name (lower case, prefix match) -> explanation
MODULES = [
    ('fcstruct2', 'CEF FCStruct2 (face painting optimizer)',
     'This is CEF itself. Set bEnable=0 under [General] in FCStruct2.ini and play the same spot again. If the crash stops, '
     'CEF is involved: please report the summary from this tool (the text, not files).'),
    ('fcshadow2', 'CEF FCShadow2 (multi-threaded shadow culling)',
     'This is CEF itself. Press F7 in game (or set bStartOn=0 in FCShadow2.ini) and play the same spot again. If the '
     'crash stops, FCShadow2 is involved: please report the summary from this tool (the text, not files).'),
    ('fcrender2', 'FCRender2 (not part of CEF 1.0.0)',
     'FCRender2 was removed from CEF 1.0.0 because it could freeze the game in large crowds. Delete FCRender2.dll and '
     'FCRender2.ini from Data\\F4SE\\Plugins.'),
    ('cefcrashlog', 'CEF crash logger',
     'This is CEF\'s crash logger. A crash inside it is a CEF bug: set bEnable=0 in CEFCrashLog.ini and please report the '
     'summary from this tool (the text, not files).'),
    ('fallout4.exe', 'the game engine (Fallout4.exe)',
     'The crash happened inside the game itself. That does not mean the game is at fault: engine code usually crashes '
     'because of something a mod gave it (a broken mesh, a bad record, a missing master). Look at the other modules on the '
     'call stack and at what you were doing.'),
    ('nvwgf2um', 'NVIDIA graphics driver',
     'Crashes in the graphics driver usually mean the GPU ran out of video memory or the driver hit a bad texture or mesh. '
     'Try lower texture settings or smaller texture packs, check the VRAM numbers below, and update or cleanly reinstall '
     'the driver.'),
    ('nvldumd', 'NVIDIA graphics driver', 'See the graphics driver advice above: video memory, textures, driver version.'),
    ('amdxx64', 'AMD graphics driver', 'Usually video memory, a bad texture/mesh, or the driver. Try lower texture settings and update the driver.'),
    ('atidxx64', 'AMD graphics driver', 'Usually video memory, a bad texture/mesh, or the driver. Try lower texture settings and update the driver.'),
    ('amdxc64', 'AMD graphics driver', 'Usually video memory, a bad texture/mesh, or the driver. Try lower texture settings and update the driver.'),
    ('igxe', 'Intel graphics driver', 'Make sure the game runs on your dedicated GPU, not the integrated Intel one, and update the driver.'),
    ('igd10', 'Intel graphics driver', 'Make sure the game runs on your dedicated GPU, not the integrated Intel one, and update the driver.'),
    ('d3d11', 'DirectX 11', 'Usually a graphics driver or video memory problem, or an ENB/ReShade d3d11.dll in the game folder.'),
    ('dxgi', 'DirectX (DXGI)', 'Often an overlay, ENB/ReShade, or a graphics driver problem.'),
    ('d3dcompiler', 'DirectX shader compiler', 'Often a broken shader from an ENB/ReShade preset or a mod.'),
    ('flexrelease', 'NVIDIA FleX (Weapon Debris)',
     'A well-known Fallout 4 crash. Turn Weapon Debris off in the game launcher options.'),
    ('flexextrelease', 'NVIDIA FleX (Weapon Debris)', 'A well-known Fallout 4 crash. Turn Weapon Debris off in the launcher options.'),
    ('xaudio2', 'audio (XAudio2)', 'Often a broken or wrongly formatted sound file from a mod, or an audio device that disconnected.'),
    ('f4se_', 'F4SE (Script Extender)', 'Make sure F4SE matches your game version exactly, and that its scripts were installed too.'),
    ('buffout4', 'Buffout 4', 'Buffout 4 is reporting, or a Buffout patch was involved. Check Buffout\'s settings and version.'),
    ('gameoverlayrenderer', 'Steam overlay', 'Try turning the Steam overlay off for Fallout 4.'),
    ('discordhook', 'Discord overlay', 'Try turning the Discord overlay off.'),
    ('rtsshooks', 'RivaTuner overlay', 'Try closing RivaTuner / MSI Afterburner.'),
    ('ntdll', 'Windows core (ntdll)', 'Windows itself rarely crashes; it usually reports memory damage done earlier by other code.'),
    ('kernelbase', 'Windows core (KernelBase)', 'Windows reported an error raised by other code; look at the next modules down the stack.'),
    ('ucrtbase', 'C runtime', 'The C runtime reported an error raised by other code; look at the next modules down the stack.'),
    ('vcruntime', 'C runtime', 'The C runtime reported an error raised by other code; look at the next modules down the stack.'),
]
SYSTEM_PREFIXES = ('ntdll', 'kernelbase', 'kernel32', 'ucrtbase', 'vcruntime', 'msvcp', 'user32', 'win32u', 'combase')

# Address Library IDs (same on every game version) of functions CEF redirects calls to
CEF_IDS = {
    2209308: 'UsePreprocessedFaceGen', 2209533: 'FaceGen head NIF load', 2209317: 'face-paint request enqueue',
    2209525: 'StartFaceCustomizationGenerationForNPC', 2209527: 'face tint GPU merge', 2209523: 'CreateMergeTintTextures',
    2209309: 'BSFaceGenManager::Update', 2228913: 'main frame function',
    2319338: 'sun-shadow cascade culling', 2275918: 'shadow cull', 2275926: 'shadow visibility walk',
}

# Address Library IDs of the game's world teardown, which only runs when the game quits (TES destructor and its
# deleting wrapper; the TES destructor unloads every cell)
SHUTDOWN_IDS = {2192015, 2192235}

PAPYRUS_PATTERNS = [
    ('none_call', re.compile(r'Cannot call (\w+)\(\) on a None object', re.I),
     'scripts tried to use an object that does not exist (a deleted reference, an uninstalled mod, or a property never filled)'),
    ('missing_script', re.compile(r'Cannot open store for class "([^"]+)"|Unable to bind script (\S+)', re.I),
     'scripts are missing or do not match their records. A mod was removed mid-playthrough, or its scripts were not installed'),
    ('property', re.compile(r'Property (\S+) on script (\S+).*?(cannot be bound|not found|could not be found)', re.I),
     'script properties point at records that are missing or of the wrong type (a missing master or a removed mod)'),
    ('none_assign', re.compile(r'Assigning None to a non-object property', re.I),
     'scripts stored "nothing" where an object was expected (usually harmless, but a sign of missing data)'),
    ('overload', re.compile(r'Suspended stack count is over our warning threshold|Dumping stacks', re.I),
     'THE SCRIPT ENGINE WAS OVERLOADED: more script work than it could keep up with. This causes script lag (quests not '
     'starting, settlers not moving, delayed effects) and can lead to freezes'),
    ('stack_error', re.compile(r'^\[[^\]]+\] error: ', re.I), 'script errors in total'),
]


# ------------------------------------------------------------------ helpers
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


def module_info(name):
    low = (name or '').lower()
    for prefix, title, advice in MODULES:
        if low.startswith(prefix):
            return title, advice
    if low.endswith('.dll'):
        return ('a DLL plugin or program named %s' % name,
                'This DLL is not part of the game. If it belongs to a mod (look for it in Data\\F4SE\\Plugins), update or '
                'remove that mod and test again. If it belongs to another program (an overlay, a recorder, antivirus), '
                'close that program and test again.')
    return ('unknown code', 'The crash address is not inside any loaded module, which usually means a broken function pointer.')


class Report:
    def __init__(self):
        self.lines = []

    def h(self, title):
        self.lines += ['', '=' * 78, title, '=' * 78]

    def p(self, text='', indent=0):
        import textwrap
        if not text:
            self.lines.append('')
            return
        for line in textwrap.wrap(text, 78 - indent) or ['']:
            self.lines.append(' ' * indent + line)

    def text(self):
        return '\n'.join(self.lines) + '\n'


# ------------------------------------------------------------------ CEF plugin logs
def analyze_cef_logs(rep, logdir):
    rep.h('1. CEF status')
    s2 = read_text(os.path.join(logdir, 'FCStruct2.log'))
    if s2 is None:
        rep.p('FCStruct2.log not found: FCStruct2 did not run in the last session (not installed, or the game was not '
              'started through f4se_loader.exe).')
    else:
        if 'WARNING: game executable' in s2:
            m = re.search(r'WARNING: (.*?)\. CEF disabled', s2)
            rep.p('FCStruct2 DISABLED ITSELF: your game version is not the one this CEF build was verified for. Nothing '
                  'was patched; the game ran unmodified. Wait for a CEF update for your game version.')
            if m:
                rep.p('Details: ' + m.group(1), 2)
        elif 'not installed; the game runs unmodified' in s2:
            m = re.search(r'ERROR: (.*)', s2)
            rep.p('FCStruct2 found unexpected game code and patched nothing (another mod may patch the same spot).')
            if m:
                rep.p('Details: ' + m.group(1).strip(), 2)
        elif 'disabled in FCStruct2.ini' in s2:
            rep.p('FCStruct2 is turned off in FCStruct2.ini (bEnable=0).')
        elif 'installed:' in s2:
            rep.p('FCStruct2 installed normally.')
        stats = list(re.finditer(r'starts (\d+), ABORTED (\d+).*?retry ok (\d+) / failed (\d+).*?LOST layers (\d+).*?'
                                 r'avg ([\d.]+) ms max ([\d.]+) ms.*?blocking (\d+) max ([\d.]+) ms.*?\| queue (-?\d+) \| '
                                 r'VRAM (\d+) / (\d+) MB', s2))
        if stats:
            starts = sum(int(m.group(1)) for m in stats)
            aborted = sum(int(m.group(2)) for m in stats)
            rok = sum(int(m.group(3)) for m in stats)
            rfail = sum(int(m.group(4)) for m in stats)
            lost = sum(int(m.group(5)) for m in stats)
            pmax = max(float(m.group(7)) for m in stats)
            qmax = max(int(m.group(10)) for m in stats)
            vpeak = max((int(m.group(11)), int(m.group(12))) for m in stats)
            rep.p('Faces painted: %d. Failed paint jobs: %d (retried OK: %d, left plain: %d). Lost layers: %d. Longest '
                  'paint: %.0f ms. Longest face queue: %d.' % (starts, aborted, rok, rfail, lost, pmax, qmax))
            rep.p('Peak video memory use: %d of %d MB (%.0f%%).' % (vpeak[0], vpeak[1], 100.0 * vpeak[0] / max(vpeak[1], 1)))
            if vpeak[0] > 0.9 * vpeak[1]:
                rep.p('-> Video memory was almost full. That is the most common cause of dark or missing textures, '
                      'stutter and graphics-driver crashes. Use smaller texture packs or lower texture quality.', 2)
            if rfail:
                rep.p('-> Some faces failed twice and were left plain (unpainted) instead of Rust Face. That points at video '
                      'memory pressure or a broken face texture from a mod.', 2)
            if lost:
                rep.p('-> Some face layers never loaded. Usually missing or broken face tint textures from a mod.', 2)
        guard = s2.count('VRAM guard ENGAGED')
        if guard:
            rep.p('The VRAM guard engaged %d time(s): free video memory dropped below iPoolMinFreeVramMB, so CEF released '
                  'its face texture cache. This is the guard working as intended. If it happens often, your video memory '
                  'is under pressure.' % guard)
    sh = read_text(os.path.join(logdir, 'FCShadow2.log'))
    if sh is not None:
        if 'CEF disabled itself' in sh:
            rep.p('FCShadow2 DISABLED ITSELF: your game version is not the one this CEF build was verified for. Nothing '
                  'was patched.')
        elif 'not installed; the game runs unmodified' in sh:
            m = re.search(r'ERROR: (.*)', sh)
            rep.p('FCShadow2 found unexpected game code and patched nothing (another mod may patch the same spot).')
            if m:
                rep.p('Details: ' + m.group(1).strip(), 2)
        elif 'disabled in FCShadow2.ini' in sh:
            rep.p('FCShadow2 is turned off in FCShadow2.ini (bEnable=0).')
        elif 'installed:' in sh:
            on = len(re.findall(r'\| parallel culls [1-9]', sh))
            rep.p('FCShadow2 installed normally%s.' % (' and spread shadow culling over extra CPU threads' if on else
                                                      ' (it was not switched on during this session; F7 toggles it)'))
        if 'HANG SUSPECTED' in sh:
            rep.p('-> FCShadow2 reported a long wait for its helper threads (HANG SUSPECTED). Set bEnable=0 in '
                  'FCShadow2.ini and please report the summary from this tool.', 2)
        if 'VERIFY MISMATCH' in sh or re.search(r'ERROR: loop A', sh):
            rep.p('-> FCShadow2 found a difference from the game\'s own result and switched itself off for the rest of '
                  'that session. Please report the summary from this tool.', 2)
    cl = read_text(os.path.join(logdir, 'CEFCrashLog.log'))
    if cl is None:
        rep.p('CEFCrashLog.log not found: the CEF crash logger did not run in the last session.')
    elif 'stands down' in cl:
        rep.p('Crash logger: Buffout 4 is installed, so crash logs come from Buffout (crash-*.log). This tool reads them too.')
    elif 'armed' in cl:
        rep.p('Crash logger: armed.')
    if os.path.exists(os.path.join(logdir, 'FCRender2.log')):
        rep.p('NOTE: FCRender2.log exists. FCRender2 is NOT part of CEF 1.0.0 (it could freeze the game in large crowds '
              'during testing). If FCRender2.dll is still in Data\\F4SE\\Plugins, delete it and FCRender2.ini.')


# ------------------------------------------------------------------ crash logs
def newest_crash(logdir):
    cands = glob.glob(os.path.join(logdir, 'CEF-Crash-*.log')) + glob.glob(os.path.join(logdir, 'crash-*.log'))
    return max(cands, key=os.path.getmtime) if cands else None


def parse_cef_crash(text):
    c = {'frames': [], 'kind': 'CEF'}
    m = re.search(r'^exception: ([0-9A-F]{8})', text, re.M)
    c['code'] = int(m.group(1), 16) if m else None
    m = re.search(r'^access: (read of|write to|execute at) address 0x([0-9A-F]+)', text, re.M)
    c['access'] = (m.group(1), int(m.group(2), 16)) if m else None
    m = re.search(r'^at: (\S+)', text, re.M)
    c['at'] = m.group(1) if m else None
    c['main'] = bool(re.search(r'^thread: \d+ \(main thread\)', text, re.M))
    m = re.search(r'^time: (.*)$', text, re.M)
    c['time'] = m.group(1) if m else '?'
    m = re.search(r'^game: runtime (\S+),', text, re.M)
    c['game'] = m.group(1) if m else '?'
    m = re.search(r'commit (\d+) / (\d+) MB used', text)
    c['commit'] = (int(m.group(1)), int(m.group(2))) if m else None
    m = re.search(r'physical (\d+) / (\d+) MB used', text)
    c['phys'] = (int(m.group(1)), int(m.group(2))) if m else None
    stack = text.split('call stack', 1)[1].split('stack scan', 1)[0] if 'call stack' in text else ''
    for fm in re.finditer(r'^\s+\d+\s+(\S+)(.*)$', stack, re.M):
        idm = re.search(r'Address Library ID (\d+)', fm.group(2))
        c['frames'].append((fm.group(1), int(idm.group(1)) if idm else None))
    return c


def parse_buffout_crash(text):
    c = {'frames': [], 'kind': 'Buffout 4', 'access': None, 'commit': None, 'phys': None, 'main': False, 'time': '?',
         'game': '?'}
    m = re.search(r'Unhandled exception "([^"]+)" at 0x[0-9A-F]+ (\S+)', text)
    names = {'EXCEPTION_ACCESS_VIOLATION': 0xC0000005, 'EXCEPTION_STACK_OVERFLOW': 0xC00000FD,
             'EXCEPTION_ILLEGAL_INSTRUCTION': 0xC000001D, 'EXCEPTION_INT_DIVIDE_BY_ZERO': 0xC0000094,
             'EXCEPTION_BREAKPOINT': 0x80000003, 'EXCEPTION_IN_PAGE_ERROR': 0xC0000006}
    c['code'] = names.get(m.group(1)) if m else None
    c['at'] = m.group(2) if m else None
    m = re.search(r'Fallout 4 v(\S+)', text)
    c['game'] = m.group(1) if m else '?'
    stack = text.split('PROBABLE CALL STACK:', 1)[1].split('REGISTERS:', 1)[0] if 'PROBABLE CALL STACK:' in text else ''
    for fm in re.finditer(r'^\s*\[\s*\d+\]\s+0x[0-9A-F]+\s+(\S+)(.*)$', stack, re.M):
        idm = re.search(r'->\s*(\d+)\+', fm.group(2))   # Buffout 4: "Fallout4.exe+06EC1A2 -> 2209525+0xD2" (nearest ID)
        c['frames'].append((fm.group(1), int(idm.group(1)) if idm else None))
    return c


def analyze_crash(rep, path):
    rep.h('2. Last crash')
    if not path:
        rep.p('No crash log found. Either the game has not crashed since CEF was installed, or it froze instead of '
              'crashing (a freeze leaves no crash log; see the Papyrus section for script overload).')
        return
    text = read_text(path) or ''
    buffout = 'PROBABLE CALL STACK:' in text or 'Unhandled exception "' in text
    c = parse_buffout_crash(text) if buffout else parse_cef_crash(text)
    rep.p('File: %s (%s crash log), time %s, game %s.' % (os.path.basename(path), c['kind'], c['time'], c['game']))
    if c['code'] is None:
        rep.p('Could not read the exception from this file.')
        return
    title, why = EXCEPTIONS.get(c['code'], ('Exception %08X' % c['code'], 'An uncommon error code.'))
    rep.p('WHAT HAPPENED: %s (code %08X).' % (title, c['code']))
    rep.p(why, 2)
    if c['access']:
        kind, addr = c['access']
        if addr < 0x10000:
            rep.p('The game tried to %s a near-zero address (0x%X): a "null pointer". Something expected an object that '
                  'was missing. Typical causes: a missing master or removed mod, a broken mesh, or an object deleted '
                  'while still in use.' % ('read' if kind == 'read of' else 'write' if kind == 'write to' else 'run', addr), 2)
        else:
            rep.p('Bad %s address 0x%X: the object there was already freed or memory was damaged.'
                  % ('read' if kind == 'read of' else 'write' if kind == 'write to' else 'execute', addr), 2)

    mod = (c['at'] or '').split('+')[0]
    mtitle, madvice = module_info(mod)
    rep.p('')
    rep.p('WHERE: %s, in %s.' % (c['at'], mtitle))
    rep.p(madvice, 2)

    first_mod = None
    for f, _ in c['frames']:
        name = f.split('+')[0]
        if not name.lower().startswith(SYSTEM_PREFIXES):
            first_mod = name
            break
    if first_mod and first_mod.lower() != mod.lower():
        t2, a2 = module_info(first_mod)
        rep.p('')
        rep.p('The first non-Windows code on the call stack is %s (%s).' % (first_mod, t2))
        rep.p(a2, 2)

    names = {f.split('+')[0].lower() for f, _ in c['frames']}
    cef_frames = [f for f, _ in c['frames'] if f.lower().startswith(('fcstruct2', 'fcshadow2', 'fcrender2', 'cefcrashlog'))]
    cef_funcs = sorted({CEF_IDS[i] for _, i in c['frames'] if i in CEF_IDS})
    rep.p('')
    if cef_frames:
        inis = sorted({f.split('.')[0] + '.ini' for f in cef_frames})
        rep.p('CEF CODE WAS ON THE CALL STACK (%s). CEF may be involved. Set bEnable=0 under [General] in %s and test '
              'the same situation again.' % (', '.join(cef_frames[:3]), ' and '.join(inis)))
    elif cef_funcs:
        rep.p('The stack passes through game functions whose calls CEF redirects (%s), but no CEF code was running at the '
              'time. CEF is probably not the cause; to be sure, test once with that CEF part off.' % ', '.join(cef_funcs))
    else:
        rep.p('No CEF code and no CEF-hooked function on the call stack: this crash is very unlikely to be caused by CEF.')

    if any(i in SHUTDOWN_IDS for _, i in c['frames']):
        rep.p('')
        rep.p('WHEN: while the game was shutting down. The world was being unloaded after you chose Quit. Crashes '
              'during exit are a known Fallout 4 weakness (on the CEF test PC they also happen with CEF switched off). The '
              'game had already finished: nothing is lost, and saves made before quitting are not affected. You can '
              'ignore it unless it also happens before you quit.')

    if c['commit']:
        used, total = c['commit']
        if used > 0.92 * total:
            rep.p('')
            rep.p('MEMORY: %d of %d MB of Windows commit memory was in use (%.0f%%). The PC was nearly out of memory. '
                  'Make the Windows page file larger (or let Windows manage it) and close other programs.'
                  % (used, total, 100.0 * used / total))
    if c['main']:
        rep.p('')
        rep.p('It happened on the main game thread (game logic, scripts, loading).')
    if any(n.startswith('xaudio2') for n in names):
        rep.p('Audio code was on the stack: check recently added sound mods.')


# ------------------------------------------------------------------ Papyrus
def analyze_papyrus(rep, path):
    rep.h('3. Papyrus (script) log')
    text = read_text(path) if path else None
    if text is None:
        rep.p('Papyrus.0.log not found. Script logging is off by default. To turn it on, add these lines to '
              'Documents\\My Games\\Fallout4\\Fallout4Custom.ini (see INSTALL_GUIDE.md), play until the problem happens, '
              'then run this tool again:')
        rep.p('[Papyrus]', 4)
        rep.p('bEnableLogging=1', 4)
        rep.p('bEnableTrace=1', 4)
        rep.p('bLoadDebugInformation=1', 4)
        rep.p('Turn it off again afterwards: logging costs a little performance.')
        return
    lines = text.splitlines()
    rep.p('%s: %d lines.' % (os.path.basename(path), len(lines)))
    counts = collections.Counter()
    by_script = collections.Counter()
    examples = {}
    pending = False                               # an error whose script is named on a following "stack:" line
    for line in lines:
        if pending:
            sm = re.search(r'\]\.([A-Za-z0-9_:]+)\.[A-Za-z0-9_]+\(', line)
            if sm:
                by_script[sm.group(1)] += 1
                pending = False
            elif line.startswith('['):
                pending = False
        for key, rx, _ in PAPYRUS_PATTERNS:
            if rx.search(line):
                counts[key] += 1
                examples.setdefault(key, line.strip()[:200])
                if key == 'stack_error':
                    continue
                if key == 'missing_script' and re.search(r'(class "|bind script )cc[A-Za-z]{3}FO4', line, re.I):
                    counts['cc_script'] += 1          # Bethesda's own Creation Club content, not a user-installed mod
                sm = re.search(r'class "([^"]+)"', line) or re.search(r'on script (\S+)', line) \
                    or re.search(r'Unable to bind script (\S+)', line)
                if sm:
                    by_script[sm.group(1).strip('.,')] += 1
                else:
                    pending = True
    if not counts:
        rep.p('No known script problems found.')
        return
    for key, _, meaning in PAPYRUS_PATTERNS:
        if counts[key]:
            rep.p('- %d x %s.' % (counts[key], meaning), 0)
            rep.p('example: ' + examples[key], 4)
    if by_script:
        rep.p('')
        rep.p('Scripts mentioned most often (their name usually tells you which mod they belong to):')
        for name, n in by_script.most_common(8):
            rep.p('%6d  %s' % (n, name), 2)
    rep.p('')
    # F4SE installs EquipSlot/MatSwap as loose scripts; "cannot open store" for them means loose files are ignored
    if re.search(r'Cannot open store for class "(EquipSlot|MatSwap)"', text):
        rep.p('VERDICT: F4SE\'s script functions are NOT working: the game can\'t see F4SE\'s loose script files. Mods that '
              'use F4SE script functions (Sim Settlements 2, Workshop Framework, MCM and many more) will misbehave. Fix: '
              'add these lines to Documents\\My Games\\Fallout4\\Fallout4Custom.ini (mod managers normally do this), and '
              'make sure F4SE\'s Data\\Scripts files are installed:')
        rep.p('[Archive]', 4)
        rep.p('bInvalidateOlderFiles=1', 4)
        rep.p('sResourceDataDirsFinal=', 4)
    elif counts['overload']:
        rep.p('VERDICT: the script engine was overloaded. Common causes: many script-heavy mods at once, a mod stuck in '
              'a loop, or a save that still carries scripts of removed mods. CEF does not touch scripts.')
    elif counts['missing_script'] - counts['cc_script'] > 0 or counts['property']:
        rep.p('VERDICT: scripts reference missing mods or files. If you removed a mod mid-playthrough, load a save from '
              'before you installed it, or reinstall it.')
    else:
        if counts['cc_script']:
            rep.p('All %d script binding error(s) come from Bethesda\'s own Creation Club content (script names starting '
                  'with "cc"), not from a mod you installed. Nothing to fix there.' % counts['cc_script'])
        rep.p('VERDICT: some script errors, which is normal for a modded game (even vanilla logs some). On their own they '
              'rarely cause crashes.')


# ------------------------------------------------------------------ F4SE log
def analyze_f4se(rep, logdir):
    text = read_text(os.path.join(logdir, 'f4se.log'))
    if text is None:
        return
    rep.h('4. F4SE plugin loading')
    bad = [l.strip() for l in text.splitlines()
           if re.search(r"couldn't load|incompatible|disabled|unable to load|failed", l, re.I)]
    if not bad:
        rep.p('All F4SE plugins loaded without errors.')
        return
    rep.p('These F4SE plugin lines report a problem (a plugin built for a different game version, or a missing file):')
    for l in bad[:15]:
        rep.p(re.sub(r'[A-Za-z]:\\[^ ]*\\', '', l)[:200], 2)


# ------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser(description='Explain Fallout 4 / CEF logs in plain English. Reads local files only.')
    ap.add_argument('--crash'); ap.add_argument('--papyrus'); ap.add_argument('--logs')
    ap.add_argument('--no-save', action='store_true')
    a = ap.parse_args()

    docs = documents_folder()
    logdir = a.logs or os.path.join(docs, 'My Games', 'Fallout4', 'F4SE')
    papyrus = a.papyrus or os.path.join(docs, 'My Games', 'Fallout4', 'Logs', 'Script', 'Papyrus.0.log')

    rep = Report()
    rep.lines += ['CEF log analyzer %s - local diagnosis. Nothing was sent anywhere.' % VERSION,
                  'Logs read from: %s' % logdir]
    analyze_cef_logs(rep, logdir)
    analyze_crash(rep, a.crash or newest_crash(logdir))
    analyze_papyrus(rep, papyrus)
    analyze_f4se(rep, logdir)
    rep.h('Notes')
    rep.p('This is an automatic reading of the logs, based on common failure patterns. It points you in a direction; it '
          'is not proof. Change one thing at a time and test again.')
    rep.p('If you ask someone for help, paste this text. Never run files or programs that strangers send you.')

    out = rep.text()
    user = os.environ.get('USERNAME')
    if user and len(user) > 2:
        out = re.sub(re.escape(user), '<user>', out, flags=re.I)
    out = out.encode('ascii', 'replace').decode('ascii')
    print(out)
    if not a.no_save and os.path.isdir(logdir):
        target = os.path.join(logdir, 'CEF-Diagnosis.txt')
        try:
            with open(target, 'w', encoding='ascii', errors='replace') as f:
                f.write(out)
            print('Saved: %s' % re.sub(re.escape(user or '\0'), '<user>', target, flags=re.I))
        except OSError as e:
            print('Could not save the report: %s' % e)
    return 0


if __name__ == '__main__':
    sys.exit(main())
