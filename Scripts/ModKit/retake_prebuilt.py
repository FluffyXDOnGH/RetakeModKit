"""Portable identity and payload checks for optional Win64 Mod Kit editor binaries."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path


PLUGINS = ('RetakeMapSDK', 'RetakeDestruction', 'RetakeMapContent')
MODULES = {
    'Binaries/Win64': ('RetakeModKit',),
    'Plugins/RetakeMapSDK/Binaries/Win64': ('RetakeMapSDK', 'RetakeMapSDKEditor'),
    'Plugins/RetakeDestruction/Binaries/Win64': ('RetakeDestruction', 'RetakeDestructionEditor'),
}
MANIFEST = 'ModKit/Prebuilt.json'


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def file_record(path):
    return {'size': Path(path).stat().st_size, 'sha256': sha(path)}


def native_inputs(kit):
    kit = Path(kit)
    paths = [kit / 'RetakeModKit.uproject']
    paths += [kit / 'Plugins' / name / (name + '.uplugin') for name in PLUGINS]
    for folder in [kit / 'Source', *(kit / 'Plugins' / name / 'Source' for name in PLUGINS)]:
        paths.extend(p for p in folder.rglob('*') if p.is_file())
    return {p.relative_to(kit).as_posix(): sha(p) for p in sorted(paths)}


def engine_identity(engine):
    engine = Path(engine)
    version = read(engine / 'Engine/Build/Build.version')
    if version.get('MajorVersion') != 6:
        raise ValueError('Select an Unreal Engine 6 installation.')
    build_id = read(engine / 'Engine/Binaries/Win64/UnrealEditor.modules').get('BuildId')
    if not isinstance(build_id, str) or not build_id:
        raise ValueError('The engine has no valid editor BuildId; repair or finish building the engine.')
    return {'buildId': build_id, 'version': version}


def allowed_files():
    required, optional = set(), set()
    for directory, names in MODULES.items():
        required.add(directory + '/UnrealEditor.modules')
        required.update(directory + '/UnrealEditor-' + name + '.dll' for name in names)
        optional.add(directory + '/UnrealEditor.uclasses')
    return required, optional


def module_files(kit, build_id):
    """Only the five public modules, with UBT's original manifests; never rewrite BuildIds."""
    kit = Path(kit)
    required, optional = allowed_files()
    for directory, names in MODULES.items():
        path = kit / directory / 'UnrealEditor.modules'
        if not path.is_file():
            raise ValueError('Mod Kit module manifest is missing. Rerun Setup.ps1: ' + directory)
        manifest = read(path)
        expected = {name: 'UnrealEditor-' + name + '.dll' for name in names}
        if manifest.get('BuildId') != build_id:
            raise ValueError('Mod Kit binaries do not match the selected engine build. Rerun Setup.ps1.')
        if manifest.get('Modules') != expected:
            raise ValueError('Unexpected modules or hot-reload binaries in ' + directory)
    missing = sorted(relative for relative in required if not (kit / relative).is_file())
    if missing:
        raise ValueError('Mod Kit binaries are missing. Rerun Setup.ps1. Missing: ' + ', '.join(missing))
    return sorted(required | {relative for relative in optional if (kit / relative).is_file()})


def capture(kit, engine, source_content_hash):
    identity = engine_identity(engine)
    paths = module_files(kit, identity['buildId'])
    return {'format': 1, 'platform': 'Win64', 'architecture': 'x64', 'configuration': 'Development',
            'engine': identity, 'sourceContentHash': source_content_hash,
            'inputs': native_inputs(kit), 'files': {relative: file_record(Path(kit) / relative) for relative in paths}}


def validate(kit):
    """Check a distributed binary payload and its native sources without requiring an engine."""
    kit = Path(kit)
    manifest = read(kit / MANIFEST)
    if (manifest.get('format') != 1 or manifest.get('platform') != 'Win64'
            or manifest.get('architecture') != 'x64' or manifest.get('configuration') != 'Development'):
        raise ValueError('Unsupported prebuilt Mod Kit format.')
    required, optional = allowed_files()
    files = manifest.get('files', {})
    if not isinstance(files, dict) or not required <= set(files) <= required | optional:
        raise ValueError('Prebuilt payload must contain only the public editor modules and their manifests.')
    identity = manifest.get('engine', {})
    if not identity.get('buildId') or identity.get('version', {}).get('MajorVersion') != 6:
        raise ValueError('Prebuilt payload has no valid UE6 engine identity.')
    paths = module_files(kit, identity['buildId'])
    if set(paths) != set(files) or any(file_record(kit / relative) != entry for relative, entry in files.items()):
        raise ValueError('Prebuilt binaries are missing or changed; restore the ZIP or rebuild from source.')
    if manifest.get('inputs') != native_inputs(kit):
        raise ValueError('Mod Kit source changed since its prebuilt binaries were compiled.')
    return manifest


def status(kit, engine):
    if not (Path(kit) / MANIFEST).is_file():
        return {'usable': False, 'reason': 'This copy contains source only.'}
    try:
        manifest = validate(kit)
        if manifest['engine'] != engine_identity(engine):
            return {'usable': False, 'reason': 'The bundled DLLs target a different engine build.'}
        return {'usable': True, 'reason': 'Bundled DLLs match the selected engine and kit source.'}
    except (ValueError, OSError, KeyError, TypeError) as error:
        return {'usable': False, 'reason': str(error)}
