#!/usr/bin/env python3
"""Set up a released source kit without modifying its tracked project/configuration."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import venv
import xml.etree.ElementTree as ET


ENGINE_INPUTS = ('Engine/Source', 'Engine/Plugins', 'Engine/Shaders', 'Engine/Config',
                 'Engine/Build', 'Engine/Content')


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def git(engine, *args):
    result = subprocess.run(['git', '-C', str(engine), *args], capture_output=True,
                            text=True, encoding='utf-8', errors='replace')
    if result.returncode:
        raise ValueError('A complete Epic source checkout with Git is required: ' + result.stderr.strip())
    return result.stdout.strip()


def check_engine(kit, engine):
    """The release pin is a prerequisite, not a substitute for cooked-content qualification."""
    engine = Path(engine).resolve()
    policy_path = Path(kit) / 'ModKit/Engine.json'
    if not policy_path.is_file():
        raise ValueError('This project has no public engine pin (ModKit/Engine.json).')
    policy = read(policy_path)
    version = engine / 'Engine/Build/Build.version'
    if not version.is_file() or read(version).get('MajorVersion') != policy['majorVersion']:
        raise ValueError(f"Select the Epic UE{policy['majorVersion']} source engine documented in ModKit/Engine.json.")
    if (engine / 'Engine/Build/InstalledBuild.txt').exists():
        raise ValueError('This release requires the pinned Epic source engine; launcher/installed engines need a separately qualified release.')
    if Path(git(engine, 'rev-parse', '--show-toplevel')).resolve() != engine:
        raise ValueError('The selected directory is not the root of an Epic engine source checkout.')
    revision = git(engine, 'rev-parse', 'HEAD')
    if revision != policy['revision']:
        raise ValueError(f"Unsupported engine revision {revision}; this kit requires {policy['revision']}. Use a separate checkout; do not reset an engine with local work.")
    changed = git(engine, 'status', '--porcelain', '--untracked-files=all', '--', *ENGINE_INPUTS)
    relevant = [line for line in changed.splitlines()
                if '/RiderLink/' not in line and not any(x in line for x in ('/Intermediate/', '/Binaries/', '/Saved/'))]
    # Epic's dependency setup supplies some untracked source-adjacent files. Accept only exact
    # content hashes declared by the pinned, tracked gitdeps manifests; never blanket-ignore them.
    untracked = {line[3:] for line in relevant if line.startswith('?? ')}
    verified_dependencies = set()
    if untracked and not any(not line.startswith('?? ') for line in relevant):
        for relative in git(engine, 'ls-files', '--', '*.gitdeps.xml').splitlines():
            for entry in ET.parse(engine / relative).iter('File'):
                name = entry.get('Name', '')
                if name in untracked:
                    with (engine / name).open('rb') as stream:
                        actual = hashlib.file_digest(stream, 'sha1').hexdigest()
                    if actual == entry.get('Hash', '').lower():
                        verified_dependencies.add(name)
        relevant = [line for line in relevant if line[3:] not in verified_dependencies]
    if relevant:
        raise ValueError('The pinned engine has source changes; use a pristine checkout. First changes: ' + '; '.join(relevant[:5]))
    return {'revision': revision, 'majorVersion': policy['majorVersion']}


def find_engine(kit, explicit=None):
    saved = Path(kit) / 'Saved/ModKit/engine.json'
    if explicit:
        candidates = [Path(explicit)]
    elif os.environ.get('RETAKE_UE_ROOT'):
        candidates = [Path(os.environ['RETAKE_UE_ROOT'])]
    elif saved.is_file():
        candidates = [Path(read(saved)['path'])]
    else:
        candidates = []
        if os.name == 'nt':
            import winreg
            try:
                with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Epic Games\Unreal Engine\Builds') as key:
                    for index in range(winreg.QueryInfoKey(key)[1]):
                        candidates.append(Path(winreg.EnumValue(key, index)[1]))
            except FileNotFoundError:
                pass
    valid, failures = [], []
    for candidate in dict.fromkeys(p.resolve() for p in candidates):
        try:
            check_engine(kit, candidate)
            valid.append(candidate)
        except (ValueError, OSError) as error:
            failures.append(str(error))
    if len(valid) == 1:
        return valid[0]
    if len(valid) > 1:
        raise ValueError('Multiple matching engines found; pass --engine with the checkout to use.')
    raise ValueError('No supported engine found. Pass --engine <Epic source checkout>. ' + (failures[0] if failures else ''))


def prerequisites(kit, engine):
    if sys.version_info < (3, 11):
        raise ValueError('Install Python 3.11 or newer, then rerun Setup.ps1.')
    if os.name != 'nt':
        raise ValueError('This first creator release supports Windows authoring, with Windows and LinuxServer export.')
    required = ('Engine/Binaries/Win64/UnrealEditor-Cmd.exe',
                'Engine/Binaries/Win64/UnrealEditor.exe', 'Engine/Binaries/Win64/ShaderCompileWorker.exe',
                'Engine/Build/BatchFiles/Build.bat', 'Engine/Build/BatchFiles/RunUAT.bat')
    missing = [name for name in required if not (engine / name).is_file()]
    if missing:
        raise ValueError('Finish building the pinned Epic editor and ShaderCompileWorker first. Missing: ' + ', '.join(missing))
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    if not vswhere.is_file():
        raise ValueError('Install Visual Studio Build Tools with Desktop development with C++, MSVC and a Windows SDK.')
    result = subprocess.run([str(vswhere), '-latest', '-products', '*', '-requires',
                             'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                            capture_output=True, text=True)
    if result.returncode or not result.stdout.strip():
        raise ValueError('No MSVC C++ toolchain found. Install the C++ workload supported by the pinned engine.')
    inventory = Path(kit) / 'ModKit/Files.json'
    paths = [Path(kit) / name for name in read(inventory).get('files', {})] if inventory.is_file() else []
    for path in paths:
        if path.suffix.lower() in ('.uasset', '.umap', '.fbx'):
            with path.open('rb') as stream:
                if stream.read(80).startswith(b'version https://git-lfs.github.com/spec/v1'):
                    raise ValueError(f'Git LFS content is missing: {path.relative_to(kit)}. Run git lfs pull, or use the release ZIP.')
    return {'python': '.'.join(map(str, sys.version_info[:3])), 'compiler': 'MSVC',
            'engineRevision': read(Path(kit) / 'ModKit/Engine.json')['revision']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kit', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--engine', type=Path)
    parser.add_argument('--check-only', action='store_true', help='Inspect prerequisites without installing, building or writing settings')
    args = parser.parse_args()
    kit = args.kit.resolve()
    if not (kit / 'RetakeModKit.uproject').is_file():
        raise ValueError('Select a released RetakeModKit project.')
    engine = find_engine(kit, args.engine)
    report = prerequisites(kit, engine)
    if args.check_only:
        print(json.dumps(report, indent=2))
        return
    environment = kit / '.venv'
    python = environment / 'Scripts/python.exe'
    if not python.is_file():
        venv.EnvBuilder(with_pip=True).create(environment)
    subprocess.run([str(python), '-m', 'pip', 'install', '--disable-pip-version-check', '--only-binary=:all:',
                    '-r', str(kit / 'Scripts/ModKit/requirements.txt')], cwd=kit, check=True)
    subprocess.run([str(python), str(kit / 'Scripts/ModKit/retake_kit.py'), 'setup',
                    '--kit', str(kit), '--engine', str(engine)], cwd=kit, check=True)
    subprocess.run([str(python), str(kit / 'Scripts/ModKit/retake_kit.py'), 'record',
                    '--kit', str(kit), '--engine', str(engine)], cwd=kit, check=True)
    output = kit / 'Saved/ModKit/setup.json'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report | {'completed': True}, indent=2) + '\n', encoding='utf-8')
    print('Setup completed. Run Open-ModKit.ps1 to open the authoring project.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print('Setup failed: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
