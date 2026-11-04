"""User-triggered local testing with an existing Linux server package through WSL."""
from __future__ import annotations
import ipaddress
import json
import os
from pathlib import Path
import subprocess
import time
import uuid


def wsl_prefix(distribution='', user=''):
    args = ['wsl.exe']
    if distribution: args += ['--distribution', distribution]
    if user: args += ['--user', user]
    return args + ['--exec']


def query(prefix, *arguments):
    result = subprocess.run([*prefix, *arguments], capture_output=True, text=True, encoding='utf-8', errors='replace',
        timeout=15, creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode: raise RuntimeError(result.stderr.strip() or f'The WSL command failed (exit {result.returncode})')
    return result.stdout.strip()


def status(kit):
    """Inspect only the Kit-owned server ticket; do not probe or claim ownership of unrelated servers."""
    ticket = Path(kit) / 'Saved/ModKit/TestSessions/server.json'
    if not ticket.is_file():
        return {'state': 'stopped', 'running': False, 'detail': 'No Mod Kit test server is running.'}
    try:
        session = json.loads(ticket.read_text(encoding='utf-8'))
        pid, token = session.get('linuxPid'), session.get('userDir', '')
        if not isinstance(pid, int) or pid <= 1 or not token.startswith('/tmp/retake-modkit-'):
            raise ValueError('The saved test-server session is incomplete.')
        script = 'if [ -r "/proc/$1/cmdline" ]; then tr "\\000" " " < "/proc/$1/cmdline"; fi'
        command = query(wsl_prefix(session.get('distribution', ''), session.get('user', '')), 'sh', '-c', script, '_', str(pid))
        running = token in command
        return {'state': 'running' if running else 'stopped', 'running': running, 'session': session,
                'detail': ('Running at ' + session['address'] + ':' + str(session['port'])) if running else 'The last Mod Kit server has stopped.'}
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        return {'state': 'unknown', 'running': False, 'detail': 'Cannot verify test-server state: ' + str(error)}


def stop(session):
    """Only stop our recorded process group when its command line still contains our unique test profile."""
    if os.name != 'nt' or not session: return
    pid = session.get('linuxPid'); token = session.get('userDir', '')
    if not isinstance(pid, int) or pid <= 1 or not token.startswith('/tmp/retake-modkit-'): return
    script = ('if [ -r "/proc/$1/cmdline" ]; then cmd=$(tr "\\000" " " < "/proc/$1/cmdline"); '
              'case "$cmd" in *"$2"*) /bin/kill -TERM -- "-$1";; "") exit 0;; '
              '*) echo "The recorded PID no longer belongs to this test server; nothing was stopped." >&2; exit 2;; esac; fi')
    query(wsl_prefix(session.get('distribution', ''), session.get('user', '')), 'sh', '-c', script, '_', str(pid), token)
    # A terminated/zombie process can retain /proc/PID/cmdline with no command line.
    # Wait for this exact owner to disappear, not for an unrelated/reused PID; never escalate the signal.
    prefix = wsl_prefix(session.get('distribution', ''), session.get('user', ''))
    wait_script = ('n=0; while [ "$n" -lt 48 ]; do '
                   'cmd=""; if [ -r "/proc/$1/cmdline" ]; then cmd=$(tr "\\000" " " < "/proc/$1/cmdline"); fi; '
                   'case "$cmd" in *"$2"*) ;; *) exit 0;; esac; '
                   'n=$((n+1)); sleep 0.25; done; '
                   'echo "The test server is still shutting down; wait before updating its files." >&2; exit 1')
    query(prefix, 'sh', '-c', wait_script, '_', str(pid), token)


def start(kit, server, mods_dir, manifest, selected, mode, port=17777, distribution='', user='', manifest_hash='', binary_hash='', weapon_selection=None, rotation_file=None, preset_defaults=None):
    if os.name != 'nt': raise ValueError('This local dedicated-server launcher currently uses WSL on Windows')
    if not 1024 <= int(port) <= 65533: raise ValueError('Choose a test port from 1024 to 65533')
    # A private multi-map session can supply its operator-owned rotation. Read it before
    # stopping a previous session; native match management still validates every entry.
    rotation_text = (Path(rotation_file).read_text(encoding='utf-8-sig') if rotation_file else
                     '[Rotation]\n; An empty rotation repeats this explicitly selected test map/mode.\n')
    server = Path(server).resolve(strict=True)
    if server.is_dir(): server = server / 'RetakeServer.sh'
    if server.name != 'RetakeServer.sh' or not server.is_file():
        raise ValueError('Choose the existing Linux server package containing RetakeServer.sh')
    prefix = wsl_prefix(distribution, user)
    if query(prefix, 'id', '-u') == '0':
        if user: raise ValueError('Retake needs a non-root WSL user')
        # Existing studio setup uses this account. Never create accounts or install a server here.
        if query(wsl_prefix(distribution), 'id', '-u', 'retake') == '0': raise ValueError('Choose a non-root WSL user')
        user = 'retake'; prefix = wsl_prefix(distribution, user)
    distribution = query(prefix, 'printenv', 'WSL_DISTRO_NAME')
    user = query(prefix, 'id', '-un')
    prefix = wsl_prefix(distribution, user)
    # The packaged shell entry point provisions deployment/RCON state and supplies its own
    # UserDir. Local testing uses the already-verified native binary with our private profile.
    native = server.parent / 'Retake/Binaries/Linux/RetakeServer'
    if not native.is_file():
        raise ValueError('The selected package has no native Linux server executable')
    executable = query(prefix, 'wslpath', '-u', str(native))
    package_root = query(prefix, 'wslpath', '-u', str(server.parent))
    query(prefix, 'test', '-x', executable)
    linux_mods = query(prefix, 'wslpath', '-u', str(Path(mods_dir).resolve()))
    addresses = query(prefix, 'hostname', '-I').split()
    address = next((str(ipaddress.IPv4Address(value)) for value in addresses if value.count('.') == 3), None)
    if not address: raise RuntimeError('WSL did not report an IPv4 address for the local server')
    root = kit / 'Saved/ModKit/TestSessions'; root.mkdir(parents=True, exist_ok=True)
    ticket = root / 'server.json'
    previous = status(kit)
    if previous['state'] == 'unknown':
        raise RuntimeError(previous['detail'])
    if previous['running']:
        stop(previous['session'])
    token = '/tmp/retake-modkit-' + uuid.uuid4().hex
    query(prefix, 'mkdir', '-p', token)
    log = root / ('server-' + str(time.time_ns()) + '.log')
    command = [*prefix, 'setsid', '--wait', 'sh', '-c', 'cd "$1" || exit 1; shift; echo RETAKE_MODKIT_PID=$$; exec "$@"', '_', package_root, executable, 'Retake',
        selected['packageName'] + f'?Mode={mode}?Profile=Casual', '-port=' + str(port),
        '-UserDir=' + token + '/', '-RetakeModsDir=' + linux_mods,
        # Local WSL testing must not inherit packaged dedicated servers' Internet advertisement.
        '-ini:Game:[/Script/GameOnlineServices.GameOnlineSettings]:DedicatedService=LAN',
        '-log', '-stdout', '-FullStdOutLogOutput']
    if weapon_selection:
        if __package__:
            from .retake_test_launch import write_weapon_config
        else:
            from retake_test_launch import write_weapon_config
        config = write_weapon_config(root / (token.rsplit('/', 1)[-1] + '.ini'), weapon_selection, preset_defaults)
        command.append('-GameINI=' + query(prefix, 'wslpath', '-u', str(config.resolve())))
    rotation = root / (token.rsplit('/', 1)[-1] + '-rotation.ini')
    rotation.write_text(rotation_text, encoding='utf-8')
    command.append('-RetakeRotationFile=' + query(prefix, 'wslpath', '-u', str(rotation.resolve())))
    if manifest.get('qualificationCandidate'):
        command.extend(['-RetakeModQualification', '-ExecCmds=Retake.HitRig.ContentHash'])
    with log.open('w', encoding='utf-8') as output:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
    session = {'launcherPid': process.pid, 'distribution': distribution, 'user': user, 'userDir': token,
        'server': str(server), 'address': address, 'port': port, 'log': str(log), 'mod': manifest['modId'], 'revision': manifest['revision'],
        'map': selected['mapId'], 'mode': mode, 'manifestHash': manifest_hash, 'binaryHash': binary_hash,
        'weaponSelection': weapon_selection, 'presetDefaults': preset_defaults or [],
        'modsDir': str(Path(mods_dir).resolve()), 'rotationFile': str(rotation)}
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        text = log.read_text(encoding='utf-8', errors='replace')
        for line in text.splitlines():
            if line.startswith('RETAKE_MODKIT_PID='):
                session['linuxPid'] = int(line.split('=', 1)[1]); ticket.write_text(json.dumps(session, indent=2)+'\n')
        code = process.poll()
        if code is not None: raise RuntimeError(f'The local server stopped (exit {code}); see ' + str(log))
        if 'LogRetakeModes: Error:' in text or 'event=ModRejected' in text:
            stop(session); raise RuntimeError('The server rejected the map or game configuration; see ' + str(log))
        if ('listening on port' in text.lower() and f"event=ModMounted mod={manifest['modId']}" in text
                and 'event=RetakeServerReady ' in text):
            ticket.write_text(json.dumps(session, indent=2)+'\n')
            return f'{address}:{port}', session
        time.sleep(.25)
    stop(session)
    raise RuntimeError('The local server did not become ready within two minutes; see ' + str(log))
