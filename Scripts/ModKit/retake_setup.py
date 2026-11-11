#!/usr/bin/env python3
"""Set up a source or prebuilt kit without modifying its tracked project/configuration."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import venv

if __package__:
    from . import retake_prebuilt
else:
    import retake_prebuilt


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def check_engine(kit, engine):
    """Accept UE6 installations; compilation and content compatibility are separate checks."""
    engine = Path(engine).resolve()
    policy_path = Path(kit) / 'ModKit/Engine.json'
    if not policy_path.is_file():
        raise ValueError('This project has no engine requirements (ModKit/Engine.json).')
    policy = read(policy_path)
    version = engine / 'Engine/Build/Build.version'
    if not version.is_file() or read(version).get('MajorVersion') != policy['majorVersion']:
        raise ValueError(f"Select an Unreal Engine {policy['majorVersion']} installation.")
    return {'majorVersion': policy['majorVersion'], 'version': read(version)}


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
        raise ValueError('Multiple UE6 engines found; pass --engine with the installation to use.')
    raise ValueError('No supported engine found. Pass --engine <UE6 installation>. ' + (failures[0] if failures else ''))


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
        raise ValueError('Repair or finish building the UE6 editor and ShaderCompileWorker first. Missing: ' + ', '.join(missing))
    prebuilt = retake_prebuilt.status(kit, engine)
    if prebuilt['usable'] and not (engine / 'Engine/Binaries/Win64/UnrealPak.exe').is_file():
        raise ValueError('The selected engine is missing UnrealPak.exe. Repair or finish building its packaging tools.')
    if not prebuilt['usable']:
        vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
        if not vswhere.is_file():
            raise ValueError(prebuilt['reason'] + ' Building the kit requires Visual Studio Build Tools with Desktop development with C++, MSVC and a Windows SDK.')
        result = subprocess.run([str(vswhere), '-latest', '-products', '*', '-requires',
                                 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                                capture_output=True, text=True)
        if result.returncode or not result.stdout.strip():
            raise ValueError(prebuilt['reason'] + ' Install the MSVC C++ workload supported by your engine to build the kit.')
    inventory = Path(kit) / 'ModKit/Files.json'
    paths = [Path(kit) / name for name in read(inventory).get('files', {})] if inventory.is_file() else []
    for path in paths:
        if path.suffix.lower() in ('.uasset', '.umap', '.fbx'):
            with path.open('rb') as stream:
                if stream.read(80).startswith(b'version https://git-lfs.github.com/spec/v1'):
                    raise ValueError(f'Git LFS content is missing: {path.relative_to(kit)}. Run git lfs pull, or use the release ZIP.')
    return {'python': '.'.join(map(str, sys.version_info[:3])),
            'buildMode': 'prebuilt' if prebuilt['usable'] else 'source', 'prebuilt': prebuilt,
            'engine': retake_prebuilt.engine_identity(engine)}


def check_launch(kit, engine):
    receipt = Path(kit) / 'Saved/ModKit/setup.json'
    if not receipt.is_file():
        raise ValueError('Run Setup.ps1 successfully before opening the kit.')
    setup = read(receipt)
    if setup.get('completed') is not True or setup.get('engine') != retake_prebuilt.engine_identity(engine):
        raise ValueError('The engine changed or setup is incomplete. Rerun Setup.ps1 before opening the kit.')
    if setup.get('inputs') != retake_prebuilt.native_inputs(kit):
        raise ValueError('The kit source changed. Rerun Setup.ps1 before opening the kit.')
    paths = retake_prebuilt.module_files(kit, setup['engine']['buildId'])
    files = {relative: retake_prebuilt.file_record(Path(kit) / relative) for relative in paths}
    if setup.get('files') != files:
        raise ValueError('The editor modules changed after setup. Rerun Setup.ps1 before opening the kit.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kit', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--engine', type=Path)
    parser.add_argument('--check-only', action='store_true', help='Inspect prerequisites without installing, building or writing settings')
    parser.add_argument('--check-launch', action='store_true', help='Verify the configured editor modules before opening the kit')
    parser.add_argument('--editor', action='store_true', help='Prepare authoring tools for an already running editor; never rebuild its loaded modules')
    args = parser.parse_args()
    kit = args.kit.resolve()
    if not (kit / 'RetakeModKit.uproject').is_file():
        raise ValueError('Select a released RetakeModKit project.')
    engine = find_engine(kit, args.engine)
    if args.check_launch:
        check_launch(kit, engine)
        return
    if args.editor:
        report = {'python': '.'.join(map(str, sys.version_info[:3])), 'buildMode': 'editor',
                  'engine': retake_prebuilt.engine_identity(engine)}
        retake_prebuilt.module_files(kit, report['engine']['buildId'])
    else:
        report = prerequisites(kit, engine)
    if args.check_only:
        print(json.dumps(report, indent=2))
        return
    # A failed rerun must not leave an earlier success marker usable by the launcher.
    output = kit / 'Saved/ModKit/setup.json'
    output.unlink(missing_ok=True)
    inputs = retake_prebuilt.native_inputs(kit)
    environment = kit / '.venv'
    python = environment / 'Scripts/python.exe'
    if not python.is_file():
        venv.EnvBuilder(with_pip=True).create(environment)
    subprocess.run([str(python), '-m', 'pip', 'install', '--disable-pip-version-check', '--only-binary=:all:',
                    '-r', str(kit / 'Scripts/ModKit/requirements.txt')], cwd=kit, check=True)
    if args.editor:
        settings = kit / 'Saved/ModKit/engine.json'
        settings.parent.mkdir(parents=True, exist_ok=True)
        settings.write_text(json.dumps({'path': str(engine)}, indent=2) + '\n', encoding='utf-8')
    subprocess.run([str(python), str(kit / 'Scripts/ModKit/retake_kit.py'), 'doctor' if args.editor else 'setup',
                    '--kit', str(kit), '--engine', str(engine)], cwd=kit, check=True)
    subprocess.run([str(python), str(kit / 'Scripts/ModKit/retake_kit.py'), 'record',
                    '--kit', str(kit), '--engine', str(engine)], cwd=kit, check=True)
    if inputs != retake_prebuilt.native_inputs(kit):
        raise ValueError('Kit source changed during setup. Rerun Setup.ps1.')
    identity = retake_prebuilt.engine_identity(engine)
    files = {relative: retake_prebuilt.file_record(kit / relative)
             for relative in retake_prebuilt.module_files(kit, identity['buildId'])}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report | {'completed': True, 'engine': identity, 'inputs': inputs,
                                          'files': files}, indent=2) + '\n', encoding='utf-8')
    print('Setup completed. Run Open-ModKit.ps1 to open the authoring project.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print('Setup failed: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
