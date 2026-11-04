#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Shadow of the Colossus (CUSA08809 EU 1.01 or CUSA08034 US 1.00) on PC: prepares the game image
and starts it.

    python sotc.py                      (or double-click SotC.bat)

Settings (environment variables, or sotc.ini beside this script as NAME=value lines):
    SOTC_GAME_DIR     the dumped game folder (the one with eboot.bin); default ../CUSA08809,
                      else ../CUSA08034
    SOTC_SYS_MODULES  folder with YOUR OWN console's decrypted libSceNgs2.sprx and
                      libSceUlt.sprx (shadPS4's user/sys_modules). Without it the game is silent.
                      These files are Sony firmware: they are never distributed with the port.
    SOTC_USER_DIR     saves, shader/pipeline caches; default ./user
    SOTC_UPDATE_DIR   optional: the game update; by default <game>-UPDATE or <game>-patch next to
                      the dump (as shadPS4); its files take the place of the base game's
    SOTC_DLC_DIR      optional: the game's DLC (one folder per add-on with sce_sys/param.sfo,
                      or shadPS4's addcont folder holding CUSA08809 / CUSA08034); only DLC of the
                      game's own region is listed
    SOTC_MODES        optional, comma-separated: af16 (16x anisotropic filtering),
                      fps60 (removes the 30 FPS cap; experimental, vblank at 120 Hz),
                      fps60game (the game's own 60 FPS target, as the PS4 Pro Performance mode;
                      with SOTC_FPS_CAP=off)
    SOTC_RENDER_RES   optional: the game renders at WxH instead of 1920x1080 (e.g. 2560x1080 for
                      21:9, a wider view); the output follows its aspect; PS4 mode only
    SOTC_CVARS        engine settings, NAME=VALUE separated by spaces (e.g. r_motionBlurEnable=0)
    BB_GAMEPAD        the gamepad to use (its name or part of it), when several are connected
    key.<input>=Keys, pad.<input>=buttons   remap the controls (README.md, Controles)
    SOTC_LAB          1: SotC Lab (F2 menu, free camera, control pipe, engine commands). Turns on
                      SOTC_FREECAM, SOTC_ENGINE_CMD and SOTC_CTL unless set, and exports the
                      tuning table to out/tuning.tsv (SOTC_TUNING_TSV)
    SOTC_BOOT_BOSS    N (1..16): boots straight into colossus N (a route_bossjump.txt plan is
                      written into the game folder for the launch and deleted afterwards)
    SOTC_LAB_ROUTE    with SOTC_LAB: a route file (tools/routes/x.json, user/routes/x.json) played
                      by itself once gameplay begins (SOTC_LAB_ROUTE_DELAY s after it, default 8);
                      the game closes SOTC_LAB_ROUTE_EXIT s after the route ends (default 5,
                      negative keeps it open). The log gets "SOTC LAB ROUTE done: ..." with the stats
    user/lab_pending_env.json  restart-only settings queued by the lab ({"env": {..}, "boss": n});
                      merged into this launch once, then deleted
    SOTC_DETERMINISTIC  1: the offline test mode (docs/sotc/OFFLINE-TESTS.md). Sets the defaults of
                      SOTC_DET_CLOCK, SOTC_DET_FLIP, SOTC_HEADLESS (2: a small monitor window that shows
                      the FPS overlay; 1: no window), SOTC_FPS (1: the FPS overlay and its counters),
                      SOTC_DET_ROUTE, SOTC_DET_REALTIME (1: never faster than real time), SOTC_NULL_AUDIO,
                      SOTC_DET_IO and SOTC_LAB (with
                      SOTC_LAB_ROUTE=user/routes/new-route.json), and turns the 30 FPS cap off
                      (SOTC_FPS_CAP=off). Each one is a setdefault: an explicit value (environment or
                      sotc.ini) still wins.
    SOTC_AUTO_PREPARE  0 (default): the shaders are prepared inside the game window when it opens (the port's
                      loading screen, "Preparando os shaders do jogo", SOTC_PRETRANSLATE's boot hold). 1: a
                      play launch first runs a separate headless preparation (--precompile, below) when the
                      port binary, the GPU driver, the game image or a shader-relevant setting changed since
                      the last run that finished; the launcher shows it as "Preparando shaders". Not for
                      SOTC_DETERMINISTIC runs, boss boots, or runs with neither a console nor the launcher.
    --precompile      translates every shader the game loads and exits (the launcher's "Preparar shaders"
                      runs --prepare, the same run with the stamp written)
    --prepare         prepares the shaders now (SOTC_AUTO_PREPARE's run, forced) and exits
The image is prepared again only when the game, the modules or the scripts change.
Extra arguments go to bb-probe.exe. Run from a console, the session is also saved to
<user>/last_run.log (the 4 before it as last_run.1.log ... last_run.4.log; a crash's log is
copied to <user>/crashes).
"""
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import shutil
import subprocess
import sys
import threading
import time

PORT = Path(__file__).resolve().parent
SCRIPTS = PORT / 'scripts'
SYS_MODULES = ('libSceUlt.sprx', 'libSceNgs2.sprx')
PENDING_FILE = 'lab_pending_env.json'  # in the user folder, written by the game (SotC Lab "pending")
BOSS_PLAN = 'route_bossjump.txt'       # in the game folder while a boss boot runs
BOSS_CVARS = 'nointro RetailBuild=0 automate_loadfile(route_bossjump.txt) automate_go'
BOSS_PAD = 'lab_boss_pad.txt'          # the pad file (BB_PAD_FILE) that holds Cross for the boss boot
GAMEFLOW_MARK = 'st_gameflow() starting game'
# SOTC_DETERMINISTIC=1 (the offline test mode, docs/sotc/OFFLINE-TESTS.md): defaults only, so an explicit
# value in the environment or sotc.ini wins.
DETERMINISTIC_DEFAULTS = {
    'SOTC_DET_CLOCK': '1', 'SOTC_DET_FLIP': '1', 'SOTC_HEADLESS': '2', 'SOTC_FPS': '1',
    'SOTC_DET_ROUTE': '1', 'SOTC_NULL_AUDIO': '1', 'SOTC_DET_IO': '1', 'SOTC_LAB': '1',
    'SOTC_DET_REALTIME': '1', 'SOTC_LAB_ROUTE': 'user/routes/new-route.json', 'SOTC_FPS_CAP': 'off',
}


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def load_ini():
    ini = PORT / 'sotc.ini'
    if ini.is_file():
        for line in ini.read_text(encoding='utf-8', errors='replace').splitlines():
            line = line.strip()
            if line and not line.startswith(('#', ';')) and '=' in line:
                name, value = line.split('=', 1)
                os.environ.setdefault(name.strip(), value.strip())


def no_console():
    """Started without a console (the launcher): console programs must not open their own."""
    import ctypes
    return 0 if ctypes.windll.kernel32.GetConsoleWindow() else subprocess.CREATE_NO_WINDOW


def find_probe():
    for candidate in (PORT / 'bin' / 'bb-probe.exe', PORT / 'out' / 'bb-probe.exe'):
        if candidate.is_file():
            return candidate
    fail('bb-probe.exe not found: build it with build.sh (MSYS2 CLANG64) or use a packaged build.')


def run_script(name, *args, required=True, failure='the engine hooks stay off'):
    """Runs a preparation script; a failure stops the launch unless required is False (then it
    returns False and the caller goes on without that script's output)."""
    # Inside the packaged SotC.exe (PyInstaller) the executable runs the scripts itself.
    script = ['--script'] if getattr(sys, 'frozen', False) else []
    result = subprocess.run([sys.executable, *script, str(SCRIPTS / name), *map(str, args)], stdin=subprocess.DEVNULL,
                            creationflags=no_console())
    if result.returncode:
        if required:
            sys.exit(result.returncode)
        print(f'sotc: {name} failed (see above): {failure}', file=sys.stderr)
        return False
    return True


def fingerprint(game, modules):
    """Inputs of the prepared image: eboot, bundled modules, the user's modules, the scripts."""
    digest = hashlib.sha256()
    files = [game / 'eboot.bin', *sorted((game / 'sce_module').glob('*.prx')), *modules,
             *sorted(SCRIPTS.glob('*.py'))]
    for path in files:
        stat = path.stat()
        digest.update(f'{path.resolve()}|{stat.st_size}|{stat.st_mtime_ns}\n'.encode())
    return digest.hexdigest()


def game_update(game):
    """The game update next to the dump, as shadPS4 uses it (<game>-UPDATE or <game>-patch), or
    SOTC_UPDATE_DIR; None without one."""
    named = os.environ.get('SOTC_UPDATE_DIR')
    if named:
        return Path(named).resolve()
    for suffix in ('-UPDATE', '-patch'):
        folder = game.with_name(game.name + suffix)
        if folder.is_dir():
            return folder
    return None


def merged_source(game, update, out):
    """The files the image is prepared from when an update is present: its eboot.bin, the base
    modules with the update's on top, its param.sfo (out/app-update, refreshed when they change)."""
    merged = out / 'app-update'
    pairs = [(update / 'eboot.bin', merged / 'eboot.bin')]
    for folder in (game / 'sce_module', update / 'sce_module'):
        if folder.is_dir():
            pairs += [(p, merged / 'sce_module' / p.name) for p in folder.iterdir() if p.is_file()]
    for folder in (game / 'sce_sys', update / 'sce_sys'):
        if (folder / 'param.sfo').is_file():
            pairs.append((folder / 'param.sfo', merged / 'sce_sys' / 'param.sfo'))
    for source, target in pairs:
        stat = source.stat()
        current = target.stat() if target.is_file() else None
        if not current or current.st_size != stat.st_size or current.st_mtime_ns != stat.st_mtime_ns:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
    return merged


def valid_boss(value):
    """A colossus number 1..16 (0 for none); ValueError otherwise."""
    number = int(value or 0)
    if number and not 1 <= number <= 16:
        raise ValueError(f'boss {number}: colossi are numbered 1 to 16')
    return number


def apply_pending_env(path, env):
    """Merges the restart-only settings the lab queued (path: user/lab_pending_env.json) into env,
    once: the file is deleted after it is read, even when it cannot be parsed. Returns the boss
    number queued with them (0 for none), 0 without a file."""
    if not path.is_file():
        return 0
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError) as error:
        print(f'sotc: {path.name} is not readable ({error}); ignored', file=sys.stderr)
        path.unlink(missing_ok=True)
        return 0
    path.unlink(missing_ok=True)
    for name, value in (data.get('env') or {}).items():
        if isinstance(name, str) and re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', name):
            env[name] = str(value)
    try:
        return valid_boss(data.get('boss'))
    except (TypeError, ValueError):
        print(f'sotc: {path.name}: bad boss value {data.get("boss")!r}; ignored', file=sys.stderr)
        return 0


def boss_plan(boss):
    """The route_bossjump.txt plan: jump to colossus BOSS (the shadow_jumpToBoss index is 0-based),
    then warp the horse to Wander (not for colossi 7 and 12, which have no horse)."""
    lines = ['*stage "JumpToBoss"', 'waitForGameToStart', 'wait 8', f'shadow_jumpToBoss {boss - 1}',
             'wait 6', 'waitForAreaLoader', 'wait 4', 'printPlayerPos', '*endstage']
    if boss not in (7, 12):
        lines += ['*stage "Horse"', 'shadow_horseDebugWarpToPlayer', 'wait 2', '*endstage']
    return '\n'.join(lines) + '\n'


def hold_cross(pad, log, stop):
    """Holds Cross on the pad file until the second "starting game" line of the session log (the
    title and the menus, then the shrine cutscene's skip), then releases it. Reads the log
    incrementally: it grows for the whole session."""
    marker = GAMEFLOW_MARK.encode()
    pad.write_text('cross')
    position, carry, count = 0, b'', 0
    while not stop.is_set() and count < 2:
        time.sleep(0.5)
        try:
            with open(log, 'rb') as handle:
                handle.seek(position)
                data = handle.read()
                position = handle.tell()
        except OSError:
            continue
        buffer = carry + data
        start = 0
        while True:
            hit = buffer.find(marker, start)
            if hit < 0:
                break
            if hit + len(marker) > len(carry):  # a match that ends in the new data
                count += 1
            start = hit + 1
        carry = buffer[-(len(marker) - 1):]
    pad.write_text('')


# sotc.py --precompile (the launcher's "Preparar shaders" runs --prepare): the shader preparation run. The game boots
# without a window or sound, every shader it loads is translated with all its variants into the shader cache, and the
# port leaves by itself when it is done ("SOTC PRECOMPILE done"). The shader switches come from sotc.ini as for play,
# so the cache is the one the next sessions use.
PRECOMPILE_DEFAULTS = {'SOTC_HEADLESS': '1', 'SOTC_NULL_AUDIO': '1', 'SOTC_LAB': '0', 'SOTC_FPS_CAP': 'off'}

# SOTC_AUTO_PREPARE (see the docstring): before a play launch the shaders are prepared when the fingerprint (prepare_key)
# differs from the stamp of the game folder in user/cache/prepared.json. The stamp is written only after a run that
# printed "SOTC PRECOMPILE done" and exited with 0.
PREPARE_STAMP = 'prepared.json'
PREPARE_IDLE_S = 300  # a run with no output for this long is stuck: it is stopped and the game starts without it
# Settings that never change a shader or a pipeline (runtime switches, paths, dumps, the run's own labels): a change
# to them does not ask for a new preparation. Prefixes of SOTC_* and BB_* names. Every other switch counts.
PREPARE_IGNORED = (
    'SOTC_FPS', 'SOTC_LAB', 'SOTC_TRACE', 'SOTC_PROFILE', 'SOTC_EVENT_LOG', 'SOTC_RUN_', 'SOTC_SHOT_', 'SOTC_DET',
    'SOTC_AUTO_PREPARE', 'SOTC_PRECOMPILE', 'SOTC_NOTE', 'SOTC_WRAP', 'SOTC_WINDOW_', 'SOTC_LAUNCHER_SETTINGS',
    'SOTC_USER_DIR', 'SOTC_GAME_DIR', 'SOTC_GAME_VERSION', 'SOTC_UPDATE_DIR', 'SOTC_DLC_DIR', 'SOTC_SYS_MODULES',
    'SOTC_PREDICT_TABLE', 'SOTC_PREDICT_LAYOUTS', 'SOTC_TUNING_TSV', 'SOTC_HEADLESS', 'SOTC_NULL_AUDIO',
    'SOTC_RDOC_', 'SOTC_DUMP', 'SOTC_TEX_DUMP', 'SOTC_CS_DUMP', 'SOTC_CSDR_', 'SOTC_MINIDUMP', 'SOTC_CTL',
    'SOTC_ENGINE_CMD', 'SOTC_FREECAM', 'SOTC_VOLUME', 'SOTC_MOUSE', 'SOTC_RUMBLE',
    'BB_CONFIG', 'BB_GPU_USER_DIR', 'BB_PAD_', 'BB_DUMP_DIR', 'BB_MODS_DIR', 'BB_LANGUAGE', 'BB_USER_NAME',
    'BB_GAMEPAD', 'BB_FULLSCREEN', 'BB_PRESENT_MODE', 'BB_VBLANK_HZ', 'BB_TOGGLE_FILE', 'BB_RESTART_COMMAND',
    'BB_WRITE_LOG', 'BB_GPU_LOG', 'BB_MEMORY_LOG', 'BB_TRACE', 'BB_AUDIO', 'BB_FRAME_', 'BB_STACKS_TRIGGER',
    'BB_FAULT_RING', 'BB_OVERLAY_MENU', 'BB_IME_TEXT', 'BB_MOD_TRACE', 'BB_LIST_IMPORTS', 'BB_STRICT_HANDLES',
)


def display_drivers():
    """The display adapters and their driver versions, from the registry's display class: a driver update or a
    change of graphics card changes it. 'unknown' where the registry cannot be read (no winreg, no key)."""
    try:
        import winreg
    except ImportError:
        return 'unknown'
    base = r'SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
    found = []
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, base) as classes:
            index = 0
            while True:
                try:
                    name = winreg.EnumKey(classes, index)
                except OSError:
                    break  # no more subkeys
                index += 1
                if not name.isdigit():
                    continue  # Properties, Configuration...
                try:
                    with winreg.OpenKey(classes, name) as adapter:
                        desc = winreg.QueryValueEx(adapter, 'DriverDesc')[0]
                        version = winreg.QueryValueEx(adapter, 'DriverVersion')[0]
                        found.append(f'{desc}|{version}')
                except OSError:
                    continue
    except OSError:
        return 'unknown'
    return ';'.join(sorted(found)) or 'unknown'


def prepare_key(env, probe, image):
    """The fingerprint of the shaders a play launch would use: the port binary (size and mtime), the game image
    (its fingerprint, fingerprint()), the display drivers, and every SOTC_*/BB_* setting not in PREPARE_IGNORED."""
    digest = hashlib.sha256()
    stat = probe.stat()
    digest.update(f'probe {stat.st_size} {stat.st_mtime_ns}\n'.encode())
    digest.update(f'image {image}\n'.encode())
    digest.update(f'gpu {display_drivers()}\n'.encode())
    for name in sorted(env):
        if name.startswith(('SOTC_', 'BB_')) and not name.startswith(PREPARE_IGNORED):
            digest.update(f'{name}={env[name]}\n'.encode())
    return digest.hexdigest()


def stamp_matches(stamp, game_name, key):
    try:
        data = json.loads(stamp.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return False
    return isinstance(data, dict) and data.get(game_name) == key


def write_stamp(stamp, game_name, key):
    """Records KEY for the game folder GAME_NAME; the other games' entries are kept."""
    try:
        data = json.loads(stamp.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        data = {}
    if not isinstance(data, dict):
        data = {}
    data[game_name] = key
    stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp.write_text(json.dumps(data, indent=1) + '\n', encoding='utf-8')


def read_lines(stream, lines):
    """Reader thread: the child's output, one decoded line at a time, then None."""
    for raw in iter(stream.readline, b''):
        lines.put(raw.decode('utf-8', errors='replace'))
    lines.put(None)


def prepare_shaders(env, game, user, image, probe, force=False):
    """Runs sotc.py --precompile and waits for it, forwarding its output (the launcher reads the progress lines
    "SOTC PRECOMPILE: N of M" between "SOTC PREPARE start" and "SOTC PREPARE end"). Returns True when the run
    finished; a run that did not is not stamped, so the next play prepares again. FORCE: run even when the stamp
    matches (--prepare)."""
    stamp = user / 'cache' / PREPARE_STAMP
    key = prepare_key(env, probe, image)
    if not force and stamp_matches(stamp, game.name, key):
        return True
    print('SOTC PREPARE start: preparing the shaders before play (first start, or after a change of the port, '
          'the GPU driver or the settings)', flush=True)
    child_env = dict(env)
    child_env['SOTC_LAB'] = '0'  # the preparation runs no lab (its restart settings stay for the play launch)
    child_env.pop('SOTC_BOOT_BOSS', None)
    if getattr(sys, 'frozen', False):
        command = [sys.executable, '--run', '--precompile']  # the packaged SotC.exe runs sotc.py itself
    else:
        command = [sys.executable, str(PORT / 'sotc.py'), '--precompile']
    try:
        process = subprocess.Popen(command, cwd=str(PORT), env=child_env, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=no_console())
    except OSError as error:
        print(f'sotc: the shader preparation did not start: {error}', file=sys.stderr)
        print('SOTC PREPARE end: not finished', flush=True)
        return False
    lines = queue.Queue()
    threading.Thread(target=read_lines, args=(process.stdout, lines), daemon=True).start()
    finished = False
    last_output = time.monotonic()
    while True:
        try:
            line = lines.get(timeout=1.0)
        except queue.Empty:
            if time.monotonic() - last_output > PREPARE_IDLE_S:
                print(f'sotc: the shader preparation printed nothing for {PREPARE_IDLE_S} s: stopped',
                      file=sys.stderr)
                process.kill()
                break
            continue
        if line is None:
            break
        last_output = time.monotonic()
        if line.startswith('SOTC PRECOMPILE done'):
            finished = True
        sys.stdout.write(line)
        sys.stdout.flush()
    code = process.wait()
    ok = finished and code == 0
    if ok:
        write_stamp(stamp, game.name, key)
    print(f'SOTC PREPARE end: {"done" if ok else "not finished (the game compiles the rest on first use)"}',
          flush=True)
    return ok


def main():
    load_ini()
    env = os.environ
    prepare_only = '--prepare' in sys.argv
    if prepare_only:
        sys.argv.remove('--prepare')
    precompile_run = '--precompile' in sys.argv
    if precompile_run:
        sys.argv.remove('--precompile')
        env['SOTC_PRECOMPILE'] = '1'
        for name, value in PRECOMPILE_DEFAULTS.items():
            env.setdefault(name, value)
        # A measurement left in sotc.ini (profiler, trace) only slows the preparation down.
        for name in ('SOTC_PROFILE', 'SOTC_PROFILE_OUT', 'SOTC_TRACE'):
            env.pop(name, None)
    if env.get('SOTC_DETERMINISTIC', '0') not in ('', '0'):
        # Before the lab switches below (SOTC_LAB is one of the defaults).
        for name, value in DETERMINISTIC_DEFAULTS.items():
            env.setdefault(name, value)
    os.chdir(PORT)
    out = PORT / 'out'
    out.mkdir(exist_ok=True)
    # The EU dump (CUSA08809) next to the port, else the US one (CUSA08034).
    default = next((PORT.parent / g for g in ('CUSA08809', 'CUSA08034') if (PORT.parent / g / 'eboot.bin').is_file()),
                   PORT.parent / 'CUSA08809')
    game = Path(env.get('SOTC_GAME_DIR', default)).resolve()
    if not (game / 'eboot.bin').is_file():
        fail(f'No eboot.bin in {game}: set SOTC_GAME_DIR (environment or sotc.ini) to the CUSA08809 (EU) '
             'or CUSA08034 (US) dump.')

    modules = []
    sys_modules = env.get('SOTC_SYS_MODULES')
    if sys_modules:
        modules = [Path(sys_modules) / name for name in SYS_MODULES]
        missing = [m.name for m in modules if not m.is_file()]
        if missing:
            print(f'SOTC_SYS_MODULES: {", ".join(missing)} not found in {sys_modules}; no sound', file=sys.stderr)
            modules = []
    else:
        print('SOTC_SYS_MODULES not set: no sound (see sotc.py)', file=sys.stderr)

    # A complete update (shadPS4's <game>-UPDATE): the image comes from its eboot, and its files
    # take the place of the base game's while the game runs (src/runtime_file.c).
    update = game_update(game)
    for folder in (game, update):
        if folder and (folder / 'eboot.bin').is_file() and (folder / 'eboot.bin').stat().st_size == 0:
            fail(f'{folder / "eboot.bin"} is empty (0 bytes): the copy was interrupted. Copy the file again.')
    source = game
    if update:
        print(f'Game update: {update}')
        env['SOTC_UPDATE_DIR'] = str(update)
        if (update / 'eboot.bin').is_file():
            source = merged_source(game, update, out)
    stamp = out / 'prepared.txt'
    wanted = fingerprint(source, modules)
    images = [out / name for name in ('boot-linked.bin', 'content.bin', 'sotc-cpu.bin')]
    if not stamp.is_file() or stamp.read_text().strip() != wanted or not all(p.is_file() for p in images):
        print('Preparing the game image...')
        run_script('prepare.py', source, '--out', out)
        link = ['link_modules.py', source, '--out', out]
        if modules:
            link += ['--sys-modules', modules[0].parent]
        run_script(*link)
        run_script('content_profile.py', source, '--out', out)
        run_script('sotc_cpu.py', source, '--out', out)
        stamp.write_text(wanted + '\n')

    # SotC Lab (docs/sotc/LAB.md): restart-only settings queued by the lab, merged once (before the
    # lab switches below read the environment); then the boss boot, if one is asked for.
    user = Path(env.get('SOTC_USER_DIR', PORT / 'user')).resolve()
    boss = apply_pending_env(user / PENDING_FILE, env)
    if not boss:
        try:
            boss = valid_boss(env.get('SOTC_BOOT_BOSS'))
        except ValueError as error:
            fail(f'SOTC_BOOT_BOSS: {error}')
    lab = env.get('SOTC_LAB', '0') not in ('', '0')
    if lab:
        for name in ('SOTC_FREECAM', 'SOTC_ENGINE_CMD', 'SOTC_CTL'):
            env.setdefault(name, '1')
        # The tuning table of this build (the lab's tuning page reads it from SOTC_TUNING_TSV).
        tsv = out / 'tuning.tsv'
        tsv.unlink(missing_ok=True)
        run_script('tuning_export.py', '--out', str(tsv), required=False,
                   failure='the lab tuning table is missing')
        if tsv.is_file():
            env['SOTC_TUNING_TSV'] = str(tsv.resolve())
    modes = env.get('SOTC_MODES', '')
    cvars = env.get('SOTC_CVARS')
    if cvars is None:
        # Without the launcher: the culling settings of Pink-shadPS4 (same image, fewer GPU waits).
        cvars = 'doIndexBufferCulling=0 doSunShadowIndexBufferCulling=0'
    if env.get('SOTC_FRAME_AHEAD') == '1' and 'gameMaxDeltaTime' not in cvars:
        # One frame of command-processor lead (gnmdriver.cpp FrameAheadActive): a burst frame then runs
        # over two frames; the clamp keeps the game's simulation step from jumping (default 0.1 s).
        cvars = ' '.join(filter(None, (cvars, 'gameMaxDeltaTime=0.05')))
        env['SOTC_CVARS'] = cvars
    if lab and 'RetailBuild=' not in cvars:
        # The engine's debug console commands the lab sends (shadow_jumpToBoss, toggleDebugCam,
        # SetCameraPos...) need the developer build flag, as in PR AjncSystem#345.
        cvars = ' '.join(filter(None, (cvars, 'RetailBuild=0')))
        env['SOTC_CVARS'] = cvars
    if boss:
        # The plan the game runs on this launch (automate_loadfile); deleted when the game ends.
        plan = game / BOSS_PLAN
        try:
            plan.write_text(boss_plan(boss), encoding='utf-8')
        except OSError as error:
            fail(f'Could not write {plan} for the boss boot: {error}')
        cvars = ' '.join(filter(None, (cvars, BOSS_CVARS)))
        env['SOTC_CVARS'] = cvars
        print(f'sotc: boss boot: colossus {boss}', file=sys.stderr)
    if env.get('SOTC_NOTE'):
        print(f"sotc: {env['SOTC_NOTE']}", file=sys.stderr)
    render = env.get('SOTC_RENDER_RES', '').strip()
    if render == '1920x1080':
        # A 16:9 output above 1080p renders at 1080p as before: no patches, no extra memory.
        render = ''
    if render:
        env['SOTC_RENDER_RES'] = render  # the GPU library reads it (bilinear 2D art above 1080p)
    else:
        env.pop('SOTC_RENDER_RES', None)
    run_script('sotc_modes.py', source, '--out', out, '--modes', modes, '--cvars', cvars,
               *(['--render', render] if render else []))
    # Engine hooks (docs/HOOK-DESIGN.md): the declarations of scripts/hooks.d, on every launch. A broken
    # declaration turns the hooks off (no hooks.bin); it does not stop the game.
    # The previous launch's file never survives: a run that exits without writing leaves none.
    (out / 'hooks.bin').unlink(missing_ok=True)
    if not run_script('sotc_hooks.py', source, '--out', out, required=False):
        (out / 'hooks.bin').unlink(missing_ok=True)
    if render:
        # The output keeps the render size's aspect (a 16:9 output would stretch a 21:9 image).
        try:
            rw, rh = map(int, render.split('x'))
            ow, oh = map(int, env.get('SOTC_OUTPUT_RES', '').split('x'))
            same_aspect = abs(ow * rh - oh * rw) <= oh * rw // 100
        except ValueError:
            same_aspect = False
        if not same_aspect:
            env['SOTC_OUTPUT_RES'] = render
        # Larger render targets: the game's heaps take what direct memory there is (run.sh).
        env.setdefault('BB_DMEM_MB', '9152')
    # The game's direct memory allocator (a 64 KiB page bitmap over all of it) filled up and read
    # past the bitmap (guest offset 0x1a371, read of -1): in PS4 Pro mode with 5696 MiB when a save
    # loaded, and in PS4 mode with 5056 MiB on loading the last colossus' area (user logs, 19
    # Oct; high quality textures keep more fine mips resident than on a PS4). Every mode gets the
    # 9152 MiB the wide render sizes already used.
    env.setdefault('BB_DMEM_MB', '9152')
    # The game waits for vblanks: at 60 Hz an uncapped frame (fps60, or F7 in game) still rounds
    # up to 30 FPS. With the cap, 120 Hz only shows a late frame sooner (and SOTC_FPS_LIMIT counts
    # in 120 Hz vblanks).
    env.setdefault('BB_VBLANK_HZ', '120')
    # The per-frame arena read by the GPU in host memory (SOTC_PC_RING=8, gpu/sotc/pc_ring.h): same FPS,
    # max frame ~140 -> ~67 ms and a third fewer write faults on the Lab route (1 Nov). It needs the
    # allocator probe (the arena blocks) and the frame flip hook (the flip wait). SOTC_PC_RING=0 turns it off.
    # The native memory model's mask hooks (scripts/hooks.d/native_results.py) patch the same bytes as the XPR
    # probe; with both on, the later hook is refused.
    if env.get('SOTC_NATIVE', '0') not in ('', '0') and env.pop('SOTC_HOOK_XPR_PROBE', None):
        print('sotc: SOTC_HOOK_XPR_PROBE off (SOTC_NATIVE hooks the same functions)', file=sys.stderr)
    env.setdefault('SOTC_PC_RING', '8')
    if env['SOTC_PC_RING'] == '8':
        env.setdefault('SOTC_HOOK_ALLOC_PROBE', '1')
        env.setdefault('SOTC_HOOK_FRAME_FLIP', '1')
    # SOTC_NATIVE: the allocator probe stays on for its FREE_BLOCK entry (SOTC_PC_POOL_NATIVE's controller needs it,
    # gpu/sotc/pc_pool.cpp NativeEntry); the memtype probe's post phase announces the RenderSingleFrameCPU blocks
    # (gpu/sotc/probe_memtype.cpp, NoteFrameBlock). SOTC_PC_RING_DISPATCH hooks 0x12820 too: do not set both.
    if env.get('SOTC_NATIVE', '0') not in ('', '0'):
        env.setdefault('SOTC_HOOK_ALLOC_PROBE', '1')
        env.setdefault('SOTC_HOOK_MEMTYPE_PROBE', '1')
    # Particle stats from the last completed GPU copy (gpu/sotc/native/native_particles.h): the stats hook
    # serves in mode 3. Needs SOTC_NATIVE (the frame ticket); its observe modes 1 and 2 are replaced.
    if env.get('SOTC_NATIVE_PARTICLES') == 'prev':
        if env.get('SOTC_PARTICLE_PREV', '3') != '3':
            print('sotc: SOTC_PARTICLE_PREV=3 (SOTC_NATIVE_PARTICLES=prev serves through the same hook)',
                  file=sys.stderr)
        env['SOTC_PARTICLE_PREV'] = '3'

    probe = find_probe()
    # Development builds run from the MSYS2 tree: its CLANG64 DLLs (SDL3, FFmpeg, ...).
    if not (probe.parent / 'SDL3.dll').is_file():
        clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'
    user = Path(env.get('SOTC_USER_DIR', PORT / 'user')).resolve()
    user.mkdir(parents=True, exist_ok=True)
    env.setdefault('BB_GPU_USER_DIR', str(user))
    # The resource loader hook (RES_LOADED) for the predict probe, the extraction probe and the pre-translation; the
    # pre-translation reads the program table and the pixel layouts of user/shader_predict (written from the game's
    # own files and sessions, never shipped).
    def on(name):
        return env.get(name, '0') not in ('', '0')
    if any(on(n) for n in ('SOTC_CSDR_PROBE', 'SOTC_PREDICT_PROBE', 'SOTC_PRETRANSLATE', 'SOTC_PRETRANSLATE_COMPUTE', 'SOTC_RES_DUMP',
                           'SOTC_PRECOMPILE')):
        env.setdefault('SOTC_RES_LOADED', '1')
    if on('SOTC_PRETRANSLATE') or on('SOTC_PRECOMPILE'):
        predict = user / 'shader_predict'
        predict.mkdir(exist_ok=True)
        env.setdefault('SOTC_PREDICT_TABLE', str(predict / 'predict_table.tsv'))
        env.setdefault('SOTC_PREDICT_LAYOUTS', str(predict / 'pass_layouts.txt'))
        if not Path(env['SOTC_PREDICT_TABLE']).is_file():
            print(f'sotc: no shader table at {env["SOTC_PREDICT_TABLE"]}: the pixel layouts of the pre-translation '
                  'are guessed', file=sys.stderr)
    # Controls: key.<input>= / pad.<input>= lines of sotc.ini remap keys and buttons (src/runtime_pad.c).
    env.setdefault('BB_CONFIG', str(PORT / 'sotc.ini'))
    # SOTC_AUTO_PREPARE (prepare_shaders): the shaders are prepared before the game starts. Not for the offline test
    # mode, a boss boot, the preparation run itself, or a run with neither a console nor the launcher (a bench script).
    deterministic = env.get('SOTC_DETERMINISTIC', '0') not in ('', '0')
    interactive = bool(env.get('SOTC_LAUNCHER_SETTINGS')) or sys.stdout.isatty()
    # The default is the in-window preparation (the boot hold of the GPU side), so the separate run is opt-in.
    auto = env.get('SOTC_AUTO_PREPARE', '0') not in ('', '0') and not (deterministic or boss or precompile_run)
    if prepare_only or (auto and interactive):
        ok = prepare_shaders(env, game, user, wanted, probe, force=prepare_only)
        if prepare_only:
            return 0 if ok else 1
    command = [str(probe), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
               '--sotc-cpu', str(out / 'sotc-cpu.bin'), '--patches', str(out / 'patches.bin'),
               *(['--sotc-cmdline', str(out / 'cmdline.bin')] if (out / 'cmdline.bin').is_file() else []),
               *(['--sotc-hooks', str(out / 'hooks.bin')] if (out / 'hooks.bin').is_file() else []),
               '--app0', str(game), '--user', str(user), *sys.argv[1:]]
    # SOTC_WRAP: a JSON list put in front of the game, e.g. RenderDoc
    # ["C:/Program Files/RenderDoc/renderdoccmd.exe", "capture", "-w", "-c", "<file>"]; F12 then
    # captures the frame (SOTC_RDOC_SUBMITS submissions).
    if env.get('SOTC_WRAP'):
        import json
        command = json.loads(env['SOTC_WRAP']) + command
    # In a console the session also goes to user/last_run.log (as the launcher does; the launcher
    # reads this script's output into its own log, so a pipe means it is not written here). The
    # lab and the boss boot read that log too, so they log here when no launcher does.
    logged = sys.stdout.isatty() or ((boss or lab) and not env.get('SOTC_LAUNCHER_SETTINGS'))
    if not logged and not boss:
        return subprocess.run(command, stdin=subprocess.DEVNULL, creationflags=no_console()).returncode
    session_log = user / 'last_run.log'
    if boss:
        pad = user / BOSS_PAD
        env['BB_PAD_FILE'] = str(pad)
        pad.write_text('')
    with (open_session_log(user) if logged else open(os.devnull, 'wb')) as log:
        process = subprocess.Popen(command, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT)
        stop = threading.Event()
        if boss:
            threading.Thread(target=hold_cross, args=(pad, session_log, stop), daemon=True).start()
        try:
            for line in process.stdout:
                sys.stdout.buffer.write(line)
                sys.stdout.buffer.flush()
                log.write(line)
        finally:
            code = process.wait()
            stop.set()
            if boss:
                (game / BOSS_PLAN).unlink(missing_ok=True)
    if logged:
        keep_crash_log(user, code)
    return code


LOGS_KEPT = 5        # last_run.log and last_run.1.log ... last_run.4.log
CRASH_LOGS_KEPT = 20


def open_session_log(user):
    """A fresh user/last_run.log; the previous sessions' logs move to last_run.1.log (the one
    before) ... last_run.4.log. Same files as the launcher (launcher/sotc_launcher_win.py)."""
    for i in range(LOGS_KEPT - 1, 0, -1):
        older = user / ('last_run.log' if i == 1 else f'last_run.{i - 1}.log')
        if older.is_file():
            try:
                os.replace(older, user / f'last_run.{i}.log')
            except OSError:
                pass
    return open(user / 'last_run.log', 'wb', buffering=0)


def keep_crash_log(user, code):
    """After a session that ended with an error code, copies its log to
    user/crashes/<date>_<time>_<code>.log (the last 20, and the last 5 minidumps, are kept)."""
    if code == 0:
        return
    import time
    crashes = user / 'crashes'
    try:
        crashes.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime('%Y-%m-%d_%H-%M-%S')
        kept = crashes / f'{stamp}_{code & 0xffffffff:#x}.log'
        shutil.copy2(user / 'last_run.log', kept)
        # Dev builds: the host stacks of the report, symbolized at the end of the copy (needs llvm-symbolizer).
        symbolizer = PORT / 'tools' / 'crash_symbolize.py'
        if symbolizer.is_file() and (PORT / 'out' / 'gpu' / 'bb-probe.exe').is_file():
            try:
                subprocess.run([sys.executable, str(symbolizer), str(kept), '--append'], timeout=180,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            except (OSError, subprocess.SubprocessError):
                pass
        for old in sorted(crashes.glob('*.log'))[:-CRASH_LOGS_KEPT]:
            old.unlink()
        for old in sorted(crashes.glob('*.dmp'))[:-5]:
            old.unlink()
    except OSError:
        pass


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
