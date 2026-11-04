"""Bounded, read-only discovery of existing Retake packages. Never installs game binaries."""
from __future__ import annotations
import json
import os
from pathlib import Path
import re
import subprocess


def discover(kit: Path) -> dict:
    roots: dict[Path, str] = {}
    results = {'clients': [], 'servers': []}
    seen = set()

    def add(value, source):
        if value:
            path = Path(os.path.expandvars(str(value))).expanduser()
            if path.exists(): roots.setdefault(path.resolve(), source)

    preferences = kit / 'Saved/ModKit/creator-preferences.json'
    if preferences.is_file():
        saved = json.loads(preferences.read_text(encoding='utf-8-sig'))
        for key in ('game', 'server'): add(saved.get(key), 'Your saved choice')
    add(os.environ.get('RETAKE_CLIENT'), 'RETAKE_CLIENT')
    add(os.environ.get('RETAKE_SERVER'), 'RETAKE_SERVER')

    if os.name == 'nt':
        import winreg
        steam = []
        for hive in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
            for key_name in (r'Software\Valve\Steam', r'Software\WOW6432Node\Valve\Steam'):
                try:
                    with winreg.OpenKey(hive, key_name) as key:
                        for value in ('SteamPath', 'InstallPath'):
                            try: steam.append(Path(winreg.QueryValueEx(key, value)[0]))
                            except OSError: pass
                except OSError: pass
            for branch in (r'Software\Microsoft\Windows\CurrentVersion\Uninstall', r'Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall'):
                try:
                    with winreg.OpenKey(hive, branch) as parent:
                        for index in range(winreg.QueryInfoKey(parent)[0]):
                            try:
                                with winreg.OpenKey(parent, winreg.EnumKey(parent, index)) as item:
                                    name = str(winreg.QueryValueEx(item, 'DisplayName')[0]).lower()
                                    if name.startswith('retake') and 'modkit' not in name:
                                        add(winreg.QueryValueEx(item, 'InstallLocation')[0], 'Windows installation')
                            except OSError: pass
                except OSError: pass
        libraries = set(steam)
        for location in steam:
            manifest = location / 'steamapps/libraryfolders.vdf'
            if manifest.is_file():
                libraries.update(Path(p.replace('\\\\', '\\')) for p in re.findall(r'"path"\s*"([^"\r\n]+)"', manifest.read_text(errors='replace')))
        for library in libraries:
            for manifest in (library / 'steamapps').glob('appmanifest_*.acf'):
                if manifest.stat().st_size > 2 * 1024 * 1024: continue
                text = manifest.read_text(errors='replace')
                name = re.search(r'"name"\s*"([^"\r\n]+)"', text)
                directory = re.search(r'"installdir"\s*"([^"\r\n]+)"', text)
                if name and directory and name[1].lower().startswith('retake'):
                    add(library / 'steamapps/common' / directory[1], 'Steam library')
        epic = Path(os.environ.get('PROGRAMDATA', 'C:/ProgramData')) / 'Epic/UnrealEngineLauncher/LauncherInstalled.dat'
        if epic.is_file():
            try:
                for entry in json.loads(epic.read_text()).get('InstallationList', []):
                    if 'retake' in (str(entry.get('AppName', '')) + str(entry.get('ArtifactId', ''))).lower():
                        add(entry.get('InstallLocation'), 'Epic installation')
            except (ValueError, OSError): pass
        # Read only Retake process paths, without inspecting other applications' command lines.
        try:
            running = subprocess.run(['powershell.exe', '-NoProfile', '-Command',
                "Get-Process -Name Retake -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Path"],
                capture_output=True, text=True, timeout=8, creationflags=subprocess.CREATE_NO_WINDOW)
            for line in running.stdout.splitlines(): add(line.strip(), 'Running Retake client')
        except (OSError, subprocess.TimeoutExpired): pass

    # Common installations and nearby packaged-build directories; never traverse the game source project.
    for parent in (kit.parent, kit.parent.parent):
        add(parent / 'Builds/Retake', 'Nearby packaged builds')
        add(parent / 'RetakeBuilds', 'Nearby packaged builds')
    for variable in ('PROGRAMFILES', 'PROGRAMFILES(X86)', 'LOCALAPPDATA'):
        if os.environ.get(variable):
            for child in ('Retake', 'Codnyx/Retake', 'Programs/Retake'): add(Path(os.environ[variable]) / child, 'Installed Retake')

    def record(kind, executable, root, origin):
        if not executable.is_file(): return
        if kind == 'servers' and executable.name == 'RetakeServer':
            for parent in list(executable.parents)[:4]:
                launcher = parent / 'RetakeServer.sh'
                if launcher.is_file(): executable, root = launcher, parent; break
        key = str(executable.resolve()).casefold()
        if key in seen: return
        seen.add(key)
        results[kind].append({'path': str(executable.resolve()), 'root': str(root.resolve()), 'source': origin})

    pending = [(p, source, 0) for p, source in roots.items()]
    visited = set()
    ignored = {'content', 'engine', 'plugins', 'source', 'saved', 'intermediate', 'deriveddatacache', '.git', '.venv', 'recovery', 'replays', 'players', 'backups', 'backup'}
    while pending and len(visited) < 320:
        path, origin, depth = pending.pop(0)
        if path in visited: continue
        visited.add(path)
        if path.is_file(): path = path.parent
        if not path.is_dir() or any(path.glob('*.uproject')): continue
        for relative in ('Retake/Binaries/Win64/Retake.exe', 'Binaries/Win64/Retake.exe', 'Retake.exe',
                         'Retake/Binaries/Win64/Retake-Win64-Shipping.exe'):
            executable = path / relative
            if relative == 'Retake.exe' and any((path / 'Retake/Binaries/Win64' / name).is_file() for name in ('Retake.exe','Retake-Win64-Shipping.exe')): continue
            record('clients', executable, path, origin)
        for relative in ('RetakeServer.sh', 'Retake/Binaries/Linux/RetakeServer', 'Binaries/Linux/RetakeServer'):
            executable = path / relative
            if relative != 'RetakeServer.sh' and (path / 'RetakeServer.sh').is_file(): continue
            record('servers', executable, path, origin)
        if depth < 4:
            try:
                for child in path.iterdir():
                    if child.is_dir() and not child.is_symlink() and child.name.lower() not in ignored:
                        pending.append((child, origin, depth + 1))
            except OSError: pass
    for items in results.values():
        items.sort(key=lambda item: (item['source'] != 'Your saved choice', item['source'] != 'Running Retake client', -Path(item['path']).stat().st_mtime))
    return results
