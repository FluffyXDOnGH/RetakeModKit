#!/usr/bin/env python3
"""Standalone Retake Map Kit tooling. No imports, paths or credentials from the private game repository.

Run with Python 3.11+ and requirements.txt installed. --kit defaults to this script's project root.
setup/build compile the source SDK; doctor checks prerequisites; export always uses tagged cooking.
Qualification receipts are supplied by the studio after user-owned acceptance, never invented by this tool.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import uuid
import zipfile
from pathlib import Path

if __package__:
    from . import retake_sources, retake_prebuilt
    from .retake_contract import retained_surface_changes
else:
    import retake_sources
    import retake_prebuilt
    from retake_contract import retained_surface_changes

FORMAT = 'RetakeMap'
SCHEMA = 3
MINIMUM_SDK_CONTRACT = 39  # Paired with the fresh native SDK readable/weapon floor.
PLATFORMS = {'Windows': ('Win64', False), 'LinuxServer': ('Linux', True)}
PLUGINS = ('RetakeMapSDK', 'RetakeDestruction', 'RetakeMapContent')
MOD = re.compile(r'RetakeMod_[A-Za-z0-9_]{1,38}')
REVISION = re.compile(r'[A-Za-z0-9]{1,32}')


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def write(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, sort_keys=True, indent=2) + '\n', encoding='utf-8')
    os.replace(temporary, path)


def hasher():
    try:
        from blake3 import blake3
    except ImportError as error:
        raise RuntimeError('Authoring dependencies could not load. Use Retake > Set Up Authoring Tools to repair them, then retry. CLI: python -m pip install --force-reinstall --only-binary=:all: -r requirements.txt') from error
    return blake3()


def digest(path):
    h = hasher()
    with Path(path).open('rb') as stream:
        for data in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            h.update(data)
    return h.hexdigest()


def identity(value):
    h = hasher()
    h.update(json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode())
    return h.hexdigest()


def child(root, relative):
    root = Path(root).resolve()
    return child_of_resolved_root(root, relative)


def child_of_resolved_root(root, relative):
    """Same containment checks when an internal caller has already resolved its root once."""
    if not isinstance(relative, str) or not relative or '\\' in relative or ':' in relative or relative.startswith('/'):
        raise ValueError(f'Invalid relative path: {relative!r}')
    if any(p in ('', '.', '..') for p in relative.split('/')):
        raise ValueError(f'Invalid relative path: {relative!r}')
    path = (root / relative).resolve()
    if not path.is_relative_to(root) or path == root:
        raise ValueError(f'Path leaves its workspace: {relative!r}')
    return path


def remove_generated(root, path):
    root, path = Path(root).resolve(), Path(path).resolve()
    if path == root or not path.is_relative_to(root):
        raise ValueError(f'Refusing removal outside generated workspace: {path}')
    if path.exists():
        shutil.rmtree(path)


def run(args, *, cwd, log=None):
    args = [str(a) for a in args]
    # Windows batch entry points are shell-parsed even with shell=False.
    if any(any(c in a for c in '\r\n"&|<>^%') for a in args):
        raise ValueError('Unsupported shell metacharacter in a build argument')
    print('event=CreatorTask ' + json.dumps({'program': Path(args[0]).name,
        'task': next((a[5:] for a in args if a.startswith('-run=')), Path(args[1]).name if len(args) > 1 else ''),
        'server': '-server' in args, 'phase': next((a[7:] for a in args if a.startswith('-Phase=')), ''),
        'log': str(log) if log else ''}), flush=True)
    if log:
        Path(log).parent.mkdir(parents=True, exist_ok=True)
        with Path(log).open('w', encoding='utf-8') as stream:
            result = subprocess.run(args, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT, check=False)
        if result.returncode:
            # Present the authoring failure itself in the panel, while retaining the complete diagnostic log.
            with Path(log).open('rb') as stream:
                stream.seek(max(0, Path(log).stat().st_size - 512 * 1024))
                tail = stream.read().decode('utf-8', errors='replace')
            reasons = re.findall(r'event=[A-Za-z0-9_]*Failed[^\r\n]*?reason="([^"]+)"', tail)
            python_errors = re.findall(r'LogPython: Error:[ \t]*(.+)', tail)
            detail = (reasons[-1] if reasons else ' '.join(python_errors[-1].split())[:1200]
                      if python_errors else f'{Path(args[0]).name} failed ({result.returncode})')
            raise RuntimeError(f'{detail}; see {log}')
    else:
        subprocess.run(args, cwd=cwd, check=True)


@contextlib.contextmanager
def lock(kit):
    path = Path(kit) / 'Saved/ModKit/authoring.lock'
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('a+b') as stream:
        stream.seek(0)
        if path.stat().st_size == 0:
            stream.write(b'0'); stream.flush(); stream.seek(0)
        try:
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise RuntimeError('Another build, export or SDK staging operation is using this kit') from error
        try:
            yield
        finally:
            stream.seek(0)
            if os.name == 'nt':
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream, fcntl.LOCK_UN)


def engine_root(kit, explicit=None):
    if (Path(kit) / 'ModKit/Engine.json').is_file():
        if __package__:
            from .retake_setup import find_engine
        else:
            from retake_setup import find_engine
        return find_engine(kit, explicit)
    if explicit:
        root = Path(explicit).resolve()
    elif (Path(kit) / 'Saved/ModKit/engine.json').exists():
        root = Path(read(Path(kit) / 'Saved/ModKit/engine.json')['path']).resolve()
    else:
        association = read(Path(kit) / 'RetakeModKit.uproject').get('EngineAssociation', '')
        if os.name != 'nt':
            raise ValueError('Run setup --engine <Epic engine checkout> first')
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Epic Games\Unreal Engine\Builds') as key:
            root = Path(winreg.QueryValueEx(key, association)[0]).resolve()
    if not (root / 'Engine/Build/Build.version').is_file():
        raise ValueError(f'Not an Unreal engine: {root}')
    if read(root / 'Engine/Build/Build.version').get('MajorVersion') != 6:
        raise ValueError('This source SDK baseline targets qualified Epic UE6 engines')
    return root


def commandlet(kit, engine, name, *args, rendering=False, uproject=None):
    # PythonScriptCommandlet unescapes quoted filenames. Windows paths containing
    # spaces are quoted, so a backslash before retake_*.py becomes a carriage return.
    # All -script values supplied by this backend are file paths, not inline code.
    args = tuple(arg[:8] + arg[8:].replace('\\', '/') if arg.lower().startswith('-script=') else arg for arg in args)
    executable = engine / ('Engine/Binaries/Win64/UnrealEditor-Cmd.exe' if os.name == 'nt' else 'Engine/Binaries/Linux/UnrealEditor-Cmd')
    options = ['-AllowCommandletRendering'] if rendering else ['-NullRHI']
    selected = [arg[5:] for arg in args if arg.startswith('-Mod=')]
    if selected:
        if len(selected) != 1 or not MOD.fullmatch(selected[0]):
            raise ValueError('Select one valid content mod for the creator commandlet')
        descriptor = Path(kit) / 'Mods' / selected[0] / f'{selected[0]}.uplugin'
        validate_descriptor(descriptor)
        options += [f'-PLUGIN={descriptor}', f'-EnablePlugins={selected[0]}']
    # Retake's studio baker has the same short class name. Select the SDK owner explicitly.
    commandlet_name = '/Script/RetakeMapSDKEditor.RetakeVisibilityBakeCommandlet' if name == 'RetakeVisibilityBake' else name
    log = Path(kit) / 'Saved/ModKit/Logs' / f'{name}-{time.time_ns()}.log'
    run([executable, uproject or Path(kit) / 'RetakeModKit.uproject', f'-run={commandlet_name}', *args,
         '-unattended', '-nop4', '-nosplash', '-stdout', '-FullStdOutLogOutput', *options], cwd=kit, log=log)
    return log


def snapshot_inputs(kit, mod):
    kit = Path(kit)
    paths = [kit / 'RetakeModKit.uproject', kit / 'SDK.lock.json',
             *retake_sources.inventory(kit / 'Mods' / mod, mod).values()]
    paths.extend(kit / 'Saved/ModKit/EngineRecords' / f'{platform}.json' for platform in PLATFORMS)
    for folder in [kit / 'Config', kit / 'ModKit', kit / 'Source', kit / 'Scripts/ModKit', kit / 'Scripts/Visibility', kit / 'Mods' / mod / 'Content',
                   *(kit / 'Plugins' / p for p in PLUGINS)]:
        paths.extend(p for p in folder.rglob('*') if p.is_file() and not any(x in ('Intermediate', 'Saved', '.git', '__pycache__') for x in p.relative_to(folder).parts))
    result = {}
    for path in sorted(paths):
        if not path.is_file() or path.is_symlink() or not path.resolve().is_relative_to(kit.resolve()):
            raise ValueError(f'Input is missing or leaves the kit: {path}')
        result[path.relative_to(kit).as_posix()] = digest(path)
    return result


def contract_check(kit, engine, uproject=None):
    baseline = Path(kit) / 'Plugins/RetakeMapSDK/Contract/RetakeMapSDK.contract.json'
    output = Path(kit) / 'Saved/ModKit/current-contract.json'
    output.parent.mkdir(parents=True, exist_ok=True)
    commandlet(kit, engine, 'RetakeMapSDKContract', f'-Out={output}', uproject=uproject)
    expected, actual = read(baseline), read(output)
    actual['sourceHash'] = sdk_source_hash(kit)
    old, new = expected['surface'], actual['surface']
    changed = sorted(k for k in old if k not in new or old[k] != new[k])
    added = sorted(new.keys() - old.keys())
    report = {'breaking': changed, 'additiveCandidates': added, 'contractVersion': actual['contractVersion'],
              'oldestReadableContract': actual['oldestReadableContract'], 'baselineHash': digest(baseline), 'currentHash': digest(output)}
    write(Path(kit) / 'Saved/ModKit/contract-report.json', report)
    # A maintained baseline is committed together with each reviewed bump, never silently rewritten by a creator build.
    if changed or added or actual['sourceHash'] != expected.get('sourceHash') or actual['contractVersion'] != expected['contractVersion'] or actual['oldestReadableContract'] != expected['oldestReadableContract']:
        raise ValueError('SDK surface differs from its maintained contract; see Saved/ModKit/contract-report.json. Studio review/versioning is required.')
    reviews_path = baseline.parent / 'behavior-reviews.json'
    reviews = read(reviews_path).get('reviews', []) if reviews_path.exists() else []
    for path in sorted((baseline.parent / 'releases').glob('*.json')):
        previous = read(path)
        version = previous['contractVersion']
        if not actual['oldestReadableContract'] <= version <= actual['contractVersion']:
            continue
        if version == actual['contractVersion'] and previous['surface'] != new:
            raise ValueError(f'Published contract {version} changed without a version bump; keep its immutable snapshot')
        breaks, extensions = retained_surface_changes(previous['surface'], new)
        if extensions:
            report.setdefault('readableDefaultExtensions', []).append({'contractVersion': version, 'extensions': extensions})
        if breaks:
            report['retainedContractBreaks'] = {'contractVersion': version, 'paths': breaks}
            write(Path(kit) / 'Saved/ModKit/contract-report.json', report)
            raise ValueError(f'Readable contract {version} is broken by {breaks[:5]}; raise the readable floor, never overwrite its snapshot')
        if previous.get('sourceHash') != actual['sourceHash'] and not any(
            r.get('from') == previous.get('sourceHash') and r.get('to') == actual['sourceHash'] and r.get('compatible') is True and r.get('evidence') for r in reviews):
            raise ValueError(f'Native behavior changes since contract {version} require an explicit compatibility review')
    write(Path(kit) / 'Saved/ModKit/contract-report.json', report)
    return report


def sdk_source_hash(kit):
    entries = {}
    for plugin in ('RetakeMapSDK', 'RetakeDestruction'):
        directory = Path(kit) / 'Plugins' / plugin / 'Source' / plugin
        for path in sorted(directory.rglob('*')):
            if path.is_file() and path.suffix in ('.h', '.cpp', '.cs'):
                entries[path.relative_to(Path(kit)).as_posix()] = digest(path)
    for path in sorted((Path(kit) / 'Plugins/RetakeMapContent/Content').rglob('*.uasset')):
        entries[path.relative_to(Path(kit)).as_posix()] = digest(path)
    catalogue = Path(kit) / 'Plugins/RetakeMapSDK/Contract/RetakeWeaponCatalogue.json'
    if catalogue.is_file():
        entries[catalogue.relative_to(Path(kit)).as_posix()] = digest(catalogue)
    return identity(entries)


def hash_engine_inputs(engine, relatives, allow_missing=False):
    """Bounded parallel I/O; preserve sorted input order so records stay deterministic."""
    from concurrent.futures import ThreadPoolExecutor
    from threading import local
    engine = Path(engine).resolve()
    relatives = sorted(set(relatives))
    buffers = local()
    def hash_one(relative):
        path = child_of_resolved_root(engine, relative)
        if path.is_file():
            # Most engine inputs are small. read(4 MiB) allocates a large temporary even for
            # those files and again at EOF. Keep a private buffer for each worker instead;
            # every byte is still read on every pass, without trusting timestamps or a cache.
            if not hasattr(buffers, 'data'):
                buffers.data = bytearray(1024 * 1024)
                buffers.view = memoryview(buffers.data)
            hashed = hasher()
            with path.open('rb') as stream:
                while size := stream.readinto(buffers.data):
                    hashed.update(buffers.view[:size])
            return hashed.hexdigest()
        if allow_missing: return None
        raise ValueError(f'Engine input is incomplete: {relative}')
    with ThreadPoolExecutor(max_workers=8) as workers:
        for offset in range(0, len(relatives), 256):
            batch = relatives[offset:offset + 256]
            for relative, value in zip(batch, workers.map(hash_one, batch)):
                yield relative, value
            if offset and offset % 16384 == 0:
                print(f'event=EngineEvidence files={min(offset + 256, len(relatives))}/{len(relatives)}', flush=True)


def installed_engine_evidence(engine):
    """Identify an installed engine or a source archive by its supplied inputs and tools."""
    files = []
    roots = ('Engine/Source', 'Engine/Plugins', 'Engine/Shaders', 'Engine/Config', 'Engine/Build', 'Engine/Binaries', 'Engine/Content')
    for root in roots:
        for directory, subdirs, names in os.walk(engine / root):
            subdirs[:] = sorted(d for d in subdirs if d not in ('Intermediate', 'Saved', 'DerivedDataCache', '.git', 'RiderLink'))
            for filename in sorted(names):
                path = Path(directory) / filename
                if path.suffix.lower() in ('.pdb', '.log', '.tmp', '.pyc'):
                    continue
                files.append(path.relative_to(engine).as_posix())
    if not files:
        raise ValueError('Installed engine has no inspectable cook inputs')
    kind = 'installed' if (engine / 'Engine/Build/InstalledBuild.txt').is_file() else 'unversioned'
    return {'revision': kind + ':' + digest(engine / 'Engine/Build/Build.version'),
            'domains': {'installedSourceAndTools': identity(dict(hash_engine_inputs(engine, files)))}, 'trackedChanges': []}


def source_evidence(engine):
    """Conservative evidence, never an automatic compatibility verdict. Source and installed engines qualify explicitly."""
    if (engine / 'Engine/Build/InstalledBuild.txt').is_file():
        return installed_engine_evidence(engine)
    try:
        repository = subprocess.run(['git', '-C', str(engine), 'rev-parse', '--show-toplevel'],
                                    capture_output=True, text=True, check=False)
    except FileNotFoundError:
        return installed_engine_evidence(engine)
    if repository.returncode or Path(repository.stdout.strip()).resolve() != engine.resolve():
        return installed_engine_evidence(engine)
    roots = ['Engine/Source', 'Engine/Plugins', 'Engine/Shaders', 'Engine/Config', 'Engine/Build', 'Engine/Content']
    tracked = subprocess.check_output(['git', '-C', str(engine), 'ls-files', '-z', *roots], text=True).split('\0')
    included = lambda name: bool(name) and not any(part in name for part in ('/RiderLink/', '/Intermediate/', '/Binaries/'))
    domains = {k: hasher() for k in ('serialization', 'meshes', 'collision', 'destruction', 'niagara', 'shaders', 'other')}
    for relative, hashed in hash_engine_inputs(engine, [name for name in tracked if included(name)]):
        name = relative.lower()
        domain = ('shaders' if ('shader' in name or 'rendercore' in name or 'vertexfactory' in name) else
                  'niagara' if 'niagara' in name else 'destruction' if 'geometrycollection' in name else
                  'collision' if ('chaos' in name or 'physics' in name) else 'meshes' if ('mesh' in name or 'nanite' in name) else
                  'serialization' if ('coreuobject' in name or '/core/' in name) else 'other')
        domains[domain].update(relative.encode() + b'\0' + bytes.fromhex(hashed))
    revision = subprocess.check_output(['git', '-C', str(engine), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '-C', str(engine), 'status', '--porcelain', '--untracked-files=no', '--', *roots], text=True)
    untracked = subprocess.check_output(['git', '-C', str(engine), 'ls-files', '--others', '--exclude-standard', '-z', '--', *roots], text=True).split('\0')
    for relative, hashed in hash_engine_inputs(engine, [name for name in untracked if included(name)]):
        domains['other'].update(relative.encode() + b'\0' + bytes.fromhex(hashed))
    import xml.etree.ElementTree as ET
    dependency_files = set()
    for relative in sorted(filter(None, tracked)):
        if not relative.endswith('.gitdeps.xml'):
            continue
        for item in ET.parse(engine / relative).iter('File'):
            name = item.get('Name', '')
            if name.startswith(('Engine/Source/', 'Engine/Plugins/', 'Engine/Shaders/', 'Engine/Binaries/ThirdParty/', 'Engine/Content/')):
                dependency_files.add(name)
    dependencies = hasher()
    for relative, hashed in hash_engine_inputs(engine, dependency_files, allow_missing=True):
        dependencies.update(relative.encode() + b'\0' + (bytes.fromhex(hashed) if hashed is not None else b'MISSING'))
    domains['dependencies'] = dependencies
    return {'revision': revision, 'domains': {k: v.hexdigest() for k, v in domains.items()}, 'trackedChanges': dirty.splitlines()}


def engine_record_id(record):
    return identity({k: record[k] for k in ('platform', 'packageUE4', 'packageUE5', 'licenseeVersion', 'cookProfile', 'compilerIdentity')}
                    | {'engineDomains': record['sourceEvidence']['domains']})


def record_engine(kit, engine, uproject=None):
    evidence = source_evidence(engine)
    records = {}
    for platform in PLATFORMS:
        path = Path(kit) / 'Saved/ModKit/EngineRecords' / f'{platform}.json'
        path.parent.mkdir(parents=True, exist_ok=True)
        commandlet(kit, engine, 'RetakeMapSDKRecord', f'-Platform={platform}', f'-Out={path}', uproject=uproject)
        record = read(path)
        record['sourceEvidence'] = evidence
        # Loaded optional modules and project-specific custom versions are diagnostics, not the engine identity.
        record['recordId'] = engine_record_id(record)
        write(path, record)
        records[platform] = path
    return records


def refresh_build_inputs(kit):
    """Source archives can contain older timestamps than cached objects. Changed bytes must invalidate normal UBT actions."""
    kit = Path(kit)
    paths = [kit / 'RetakeModKit.uproject']
    paths += [kit / 'Plugins' / name / (name + '.uplugin') for name in PLUGINS]
    for folder in [kit / 'Source', *(kit / 'Plugins' / name / 'Source' for name in PLUGINS)]:
        paths.extend(p for p in folder.rglob('*') if p.is_file() and p.suffix in ('.h', '.cpp', '.cs'))
    inputs = {p.relative_to(kit).as_posix(): digest(p) for p in paths}
    receipt = kit / 'Saved/ModKit/source-build-inputs.json'
    previous = read(receipt).get('inputs', {}) if receipt.exists() else {}
    changed = [relative for relative, value in inputs.items() if previous.get(relative) != value]
    if set(previous) - set(inputs):
        # A removed translation unit must also invalidate the target makefile.
        changed.append('RetakeModKit.uproject')
    for relative in set(changed):
        os.utime(child(kit, relative), None)
    return inputs


def build(kit, engine):
    inputs = refresh_build_inputs(kit)
    batch = engine / ('Engine/Build/BatchFiles/Build.bat' if os.name == 'nt' else 'Engine/Build/BatchFiles/Linux/Build.sh')
    host = 'Win64' if os.name == 'nt' else 'Linux'
    if not (engine / 'Engine/Build/InstalledBuild.txt').is_file():
        for target, log_name in (('ShaderCompileWorker', 'shader-worker-build'), ('UnrealPak', 'packaging-tool-build')):
            run([batch, target, host, 'Development', '-WaitMutex'], cwd=kit,
                log=Path(kit) / 'Saved/ModKit/Logs' / (log_name + '.log'))
    run([batch, 'RetakeModKitEditor', host, 'Development',
         Path(kit) / 'RetakeModKit.uproject', '-NoHotReloadFromIDE', '-WaitMutex'], cwd=kit,
        log=Path(kit) / 'Saved/ModKit/Logs/build.log')
    if any(digest(child(kit, relative)) != value for relative, value in inputs.items()):
        raise ValueError('SDK source changed during compilation; repeat the build before capturing a contract')
    write(Path(kit) / 'Saved/ModKit/source-build-inputs.json', {'inputs': inputs})
    return contract_check(kit, engine)


def qualified(kit, platform, producer, material, custom_versions=None):
    path = Path(kit) / 'ModKit/qualification.json'
    if not path.is_file():
        return False
    targets = read(Path(kit) / 'SDK.lock.json').get('shipped', {}).get('targets', [])
    consumers = {t['consumerRecord'] for t in targets if t.get('name') == platform and t.get('consumerRecord') and not t.get('qualificationOnly')}
    return any(p.get('platform') == platform and p.get('producerRecord') == producer and p.get('materialPath') == material
               and p.get('consumerRecord') in consumers
               and p.get('reviewed') is True and p.get('evidence') and all(
                   guid in p.get('customVersions', {}) and p['customVersions'][guid]['min'] <= version <= p['customVersions'][guid]['max']
                   for guid, version in (custom_versions or {}).items()) for p in read(path).get('pairs', []))


def qualify(kit, receipt_path, policy_path=None):
    """Import a user-run qualification receipt. This command never runs gameplay or converts missing evidence into a pass."""
    receipt_path = Path(receipt_path).resolve()
    receipt = read(receipt_path)
    required = ('sourcePluginBuild', 'taggedPackageLoad', 'windowsLinuxIdentity', 'wrongModRevisionRefused',
                'sdkElements', 'roundReset', 'lateJoin', 'replayDependency', 'gameLogicOnlyUpdate', 'additiveSdkUpdate',
                'breakingDefaultsRefused', 'oldUnversionedRefused', 'unknownEngineRefused', 'sameSizeCorruptionRefused', 'sourceRecovery')
    if receipt.get('format') != 1 or receipt.get('reviewed') is not True or any(receipt.get('results', {}).get(k) != 'PASS' for k in required):
        raise ValueError('Qualification needs a reviewed PASS receipt for every required acceptance scenario')
    producer = read(child(receipt_path.parent, receipt['producerRecordFile']))
    consumer = read(child(receipt_path.parent, receipt['consumerRecordFile']))
    if any(record.get('recordId') != engine_record_id(record) for record in (producer, consumer)):
        raise ValueError('Qualification record content does not match its immutable identity')
    if producer['platform'] != consumer['platform'] or producer['platform'] not in PLATFORMS:
        raise ValueError('Qualification must pair records for the same platform')
    material = receipt.get('materialPath')
    if material not in ('SdkMaterials', 'CustomShaders'):
        raise ValueError('Qualification must specify its material path')
    material_case = 'sdkMaterialsSurviveShaderUpdate' if material == 'SdkMaterials' else 'customShaderCompatibility'
    if receipt.get('results', {}).get(material_case) != 'PASS':
        raise ValueError(f'Qualification needs PASS evidence for {material_case}')
    if producer['recordId'] != consumer['recordId'] and receipt.get('results', {}).get('differingEngines') != 'PASS':
        raise ValueError('Differing engine records need an explicit differingEngines PASS')
    evidence = []
    for relative, expected in receipt.get('evidenceFiles', {}).items():
        if digest(child(receipt_path.parent, relative)) != expected:
            raise ValueError(f'Qualification evidence changed: {relative}')
        evidence.append(expected)
    if not evidence:
        raise ValueError('Qualification needs retained evidence files')
    ranges = receipt.get('customVersions', {})
    for guid, interval in ranges.items():
        version = producer['customVersions'].get(guid, -1)
        if not 0 <= interval['min'] <= version <= interval['max'] <= consumer['customVersions'].get(guid, -1):
            raise ValueError(f'Custom version has no reviewed readable interval: {guid}')
    pair = {'platform': producer['platform'], 'producerRecord': producer['recordId'], 'consumerRecord': consumer['recordId'],
            'materialPath': material, 'reviewed': True, 'approvalScope': 'public-creator-release', 'customVersions': ranges, 'evidence': evidence,
            'packageUE4': producer['packageUE4'], 'packageUE5': producer['packageUE5'], 'licenseeVersion': producer['licenseeVersion'],
            'receiptHash': digest(receipt_path)}
    path = Path(policy_path) if policy_path is not None else Path(kit) / 'ModKit/qualification.json'
    policy = read(path) if path.exists() else {'format': 1, 'pairs': []}
    key = lambda p: (p['platform'], p['producerRecord'], p['consumerRecord'], p['materialPath'])
    policy['pairs'] = [p for p in policy['pairs'] if key(p) != key(pair)] + [pair]
    write(path, policy)
    return pair


def verify_export(folder):
    folder = Path(folder).resolve()
    if not 0 < (folder / 'manifest.json').stat().st_size <= 16 * 1024 * 1024:
        raise ValueError('Manifest is empty or exceeds 16 MiB')
    manifest = read(folder / 'manifest.json')
    if not isinstance(manifest, dict) or manifest.get('format') not in (FORMAT, 'RetakeWeapons') or type(manifest.get('schemaVersion')) is not int or manifest['schemaVersion'] != SCHEMA:
        raise ValueError('Experimental/unknown export format; re-export editable source with the fresh kit')
    if not isinstance(manifest.get('modId'), str) or not isinstance(manifest.get('revision'), str) or not MOD.fullmatch(manifest['modId']) or not REVISION.fullmatch(manifest['revision']):
        raise ValueError('Invalid mod identity')
    if type(manifest.get('sdkContractVersion')) is not int or manifest['sdkContractVersion'] < MINIMUM_SDK_CONTRACT:
        raise ValueError(f'Fresh exports require SDK contract {MINIMUM_SDK_CONTRACT} or newer; re-export editable source')
    weapons = manifest['format'] == 'RetakeWeapons'
    for field in ('maps', 'compatibility', 'artifacts'):
        if not isinstance(manifest.get(field), list) or (not manifest[field] and not (weapons and field == 'maps')) or not all(isinstance(v, dict) for v in manifest[field]):
            raise ValueError(f'Missing nonempty object array: {field}')
    for field in ('displayName', 'author'):
        value = manifest.get(field, '')
        if not isinstance(value, str) or len(value.encode('utf-16-le')) // 2 > 64 or any(ord(c) < 32 or ord(c) == 127 or c in '"\\?=' for c in value):
            raise ValueError(f'Invalid display metadata: {field}')
    root = '/' + manifest['modId'] + '/'
    def package_path(value):
        return isinstance(value, str) and value.startswith(root) and all(part and part not in ('.', '..') for part in value[1:].split('/')) and not any(ord(c) < 32 or c in '\\.:?*"<>| ' for c in value)
    def object_path(value):
        if not isinstance(value, str) or '.' not in value: return False
        package, object_name = value.rsplit('.', 1)
        return package_path(package) and object_name == package.rsplit('/', 1)[-1]
    catalogue = manifest.get('weaponPack', '')
    if not isinstance(catalogue, str) or (weapons and (manifest['maps'] or not object_path(catalogue))) or (not weapons and catalogue):
        raise ValueError('Weapon exports need an owned catalogue and no maps; map exports cannot declare a weapon catalogue')
    requirements = manifest.get('requiredWeaponPacks', [])
    if not isinstance(requirements, list) or len(requirements) > 64 or (weapons and requirements):
        raise ValueError('Only maps may declare up to 64 independent weapon-pack requirements')
    required_ids = set()
    for required in requirements:
        if not isinstance(required, dict) or not all(isinstance(required.get(k), str) for k in ('packId', 'revision', 'contentDigest', 'profileId')):
            raise ValueError('Weapon-pack requirements need exact string identity, revision, digest and profile fields')
        identity = required['packId'].casefold()
        if (not MOD.fullmatch(required['packId']) or not REVISION.fullmatch(required['revision'])
                or not re.fullmatch(r'[0-9a-f]{12}', required['contentDigest'])
                or not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', required['profileId']) or required['profileId'].casefold() == 'none'
                or identity == manifest['modId'].casefold() or identity in required_ids):
            raise ValueError('Invalid, duplicate or self-referencing weapon-pack requirement')
        required_ids.add(identity)
    ids = set()
    for entry in manifest['maps']:
        map_id = entry.get('mapId')
        if not isinstance(map_id, str) or not re.fullmatch(re.escape(manifest['modId']) + r'\.[A-Za-z0-9_.-]+', map_id) or len(map_id) > 64 or map_id.casefold() in ids:
            raise ValueError('Invalid or duplicate map ID')
        ids.add(map_id.casefold())
        display = entry.get('displayName')
        if not isinstance(display, str) or not display.strip() or len(display.encode('utf-16-le')) // 2 > 64 or any(ord(c) < 32 or ord(c) == 127 or c in '"\\?=' for c in display):
            raise ValueError('Map display name is missing or invalid')
        description = entry.get('description', '')
        if not isinstance(description, str) or len(description.encode('utf-16-le')) // 2 > 280 or any(ord(c) < 32 or ord(c) == 127 or c in '"\\' for c in description):
            raise ValueError('Map description is invalid')
        if not package_path(entry.get('packageName')) or not object_path(entry.get('acoustics')) or entry.get('visibility') not in ('Baked', 'Unbaked'):
            raise ValueError('Map package, acoustic resource or visibility declaration is invalid')
        for field in ('loadingArt', 'preview', 'minimap', 'ambience', 'visibilityBake', 'defaultLoadoutPolicy'):
            if entry.get(field, '') != '' and not object_path(entry[field]):
                raise ValueError(f'Map {field} must reference an object inside its mod')
        if (entry['visibility'] == 'Baked') != bool(entry.get('visibilityBake', '')):
            raise ValueError('Baked maps require their own visibility bake path; Unbaked maps must omit it')
        modes = entry.get('supportedModes')
        if not isinstance(modes, list) or not modes or not all(isinstance(v, str) and re.fullmatch(r'[A-Za-z0-9_]{1,32}', v) for v in modes):
            raise ValueError('Map supportedModes must be nonempty mode identifiers')
        policies = entry.get('modeLoadoutPolicies', {})
        if not isinstance(policies, dict) or len(policies) > 32 or any(k not in modes or not object_path(v) for k, v in policies.items()):
            raise ValueError('Mode loadout policies must be mod-owned assets for supported modes')
        if (policies or entry.get('defaultLoadoutPolicy')) and manifest['sdkContractVersion'] < 7:
            raise ValueError('Loadout policy exports require SDK contract 7 or later')
    platforms = set()
    for entry in manifest['compatibility']:
        platform = entry.get('platform')
        if platform not in PLATFORMS or platform in platforms or not isinstance(entry.get('engineRecordId'), str) or not re.fullmatch(r'[0-9a-f]{64}', entry['engineRecordId']) or entry.get('materialPath') not in ('SdkMaterials', 'CustomShaders'):
            raise ValueError('Invalid platform compatibility record')
        platforms.add(platform)
        for field in ('foreignPackages', 'nativeClasses', 'structuralProfile'):
            if not isinstance(entry.get(field), list) or not all(isinstance(v, str) for v in entry[field]) or (field != 'foreignPackages' and not entry[field]):
                raise ValueError(f'Invalid compatibility {field}')
        versions = entry.get('customVersions')
        if not isinstance(versions, dict) or not all(re.fullmatch(r'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', key) and type(value) is int and 0 <= value <= 2147483647 for key, value in versions.items()):
            raise ValueError('Invalid custom-version declaration')
    if platforms != set(PLATFORMS):
        raise ValueError('Both platform compatibility records are required')
    allowed = {'manifest.json', manifest['modId'] + '.uplugin', *PLATFORMS}
    if {p.name for p in folder.iterdir()} != allowed:
        raise ValueError('Undeclared loose mod files or directories; exports contain only manifest, descriptor and platform artifacts')
    descriptor_path = folder / (manifest['modId'] + '.uplugin')
    validate_descriptor(descriptor_path, expected_kind='weapons' if weapons else 'map')
    if read(descriptor_path).get('ExplicitlyLoaded') is not True:
        raise ValueError('An exported plugin must be explicitly loaded')
    listed = {}
    for artifact in manifest.get('artifacts', []):
        platform = artifact.get('platform')
        if type(artifact.get('size')) is not int or not 0 < artifact['size'] <= 9007199254740991 or not isinstance(artifact.get('hash'), str) or not re.fullmatch(r'[0-9a-f]{64}', artifact['hash']):
            raise ValueError('Invalid artifact size or hash')
        path = child(folder, artifact.get('relativePath'))
        if platform not in PLATFORMS or path.parent != folder / platform or path in listed:
            raise ValueError('Invalid or duplicate platform artifact')
        if not path.is_file() or path.stat().st_size != artifact['size'] or digest(path) != artifact['hash']:
            raise ValueError(f'Artifact is missing, truncated or corrupt: {artifact["relativePath"]}')
        listed[path] = platform
    if not listed or set(listed.values()) != set(PLATFORMS):
        raise ValueError('A complete export requires Windows and LinuxServer artifacts')
    for platform in PLATFORMS:
        present = set((folder / platform).iterdir())
        expected = {p for p, target in listed.items() if target == platform}
        if present != expected:
            raise ValueError(f'Undeclared files in {platform}')
        if sum(p.suffix == '.pak' for p in present) != 1 or sum(p.suffix == '.utoc' for p in present) != 1 or not any(p.suffix == '.ucas' for p in present):
            raise ValueError(f'Incomplete {platform} container')
    return manifest


def install(export_dir, mods_dir, *, qualification_test=False):
    export_dir, mods_dir = Path(export_dir).resolve(), Path(mods_dir).resolve()
    manifest = verify_export(export_dir)
    if manifest.get('qualificationCandidate') and not qualification_test:
        raise ValueError('Qualification candidates are studio evidence, not installable public exports')
    mod = manifest['modId']
    mods_dir.mkdir(parents=True, exist_ok=True)
    with lock(mods_dir / '.installer'):
        target = child(mods_dir, mod)
        incoming = child(mods_dir, f'.incoming-{mod}-{uuid.uuid4().hex}')
        previous = None
        try:
            shutil.copytree(export_dir, incoming)
            verify_export(incoming)  # Verify bytes AFTER copying, before touching the working revision.
            if target.exists():
                previous = child(mods_dir, f'.previous/{mod}-{time.time_ns()}')
                previous.parent.mkdir(parents=True, exist_ok=True)
                os.rename(target, previous)  # No copy fallback for mounted/open files.
            try:
                os.rename(incoming, target)
            except OSError:
                if previous is not None:
                    os.rename(previous, target)
                raise
        finally:
            remove_generated(mods_dir, incoming)
    return target


def recover_archive(kit, mod, revision, inputs, manifest, records):
    kit = Path(kit)
    folder = kit / 'Recovery' / mod
    folder.mkdir(parents=True, exist_ok=True)
    target = folder / f'{revision}.source.zip'
    if target.exists():
        raise ValueError('Recovery revision already exists; choose a new immutable revision')
    temporary = target.with_suffix('.partial')
    plugin = kit / 'Mods' / mod
    files = list(retake_sources.inventory(plugin, mod).values())
    metadata = {'format': 1, 'modId': mod, 'revision': revision, 'manifest': manifest,
                'sdkRelease': read(kit / 'SDK.lock.json'), 'engineRecords': {p: read(f) for p, f in records.items()},
                'sourceHashes': {p.relative_to(plugin).as_posix(): digest(p) for p in files}, 'exportInputs': inputs}
    # SDK lock is a distributable manifest: studio staging must never put absolute source/archive paths or credentials in it.
    try:
        with zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED) as archive:
            for path in files:
                if path.is_symlink() or not path.resolve().is_relative_to(plugin.resolve()):
                    raise ValueError(f'Recovery source leaves mod workspace: {path}')
                archive.write(path, 'mod/' + path.relative_to(plugin).as_posix())
            archive.writestr('recovery.json', json.dumps(metadata, sort_keys=True, indent=2))
            # Engine records retain cook settings; arbitrary creator config/credentials are never bundled.
        with zipfile.ZipFile(temporary) as archive:
            for relative, expected in metadata['sourceHashes'].items():
                h = hasher()
                with archive.open('mod/' + relative) as stream:
                    for data in iter(lambda: stream.read(4 * 1024 * 1024), b''):
                        h.update(data)
                if h.hexdigest() != expected:
                    raise ValueError(f'Editable source changed while archiving: {relative}')
        if inputs != snapshot_inputs(kit, mod):
            raise ValueError('Authoring inputs changed while archiving; export must be repeated')
        os.rename(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)
    return target


def export(kit, engine, mod, revision, modes='auto', candidate=False):
    kit = Path(kit).resolve()
    if not MOD.fullmatch(mod) or not REVISION.fullmatch(revision):
        raise ValueError('Invalid mod or revision')
    descriptor_path = kit / 'Mods' / mod / f'{mod}.uplugin'
    validate_descriptor(descriptor_path)
    kind = read(descriptor_path).get('RetakeModKind', 'map')
    if kind not in ('map', 'weapons'):
        raise ValueError('Only map and weapon-pack export is implemented')
    targets = {t['name']: t for t in read(kit / 'SDK.lock.json').get('shipped', {}).get('targets', [])}
    for platform in PLATFORMS:
        registry = kit / 'ModKit/BaseCook' / platform / 'AssetRegistry.bin'
        if platform not in targets or not isinstance(targets[platform].get('consumerRecord'), str) or not re.fullmatch(r'[0-9a-f]{64}', targets[platform]['consumerRecord']) or not registry.is_file() or digest(registry) != targets[platform].get('baseRegistryHash'):
            raise ValueError(f'Missing verified {platform} base cook registry; stage a matching game package first')
    contract_check(kit, engine)
    records = {p: kit / 'Saved/ModKit/EngineRecords' / f'{p}.json' for p in PLATFORMS}
    for path in records.values():
        if not path.is_file():
            raise ValueError('Build the source SDK and capture engine records before export')
    if not candidate:
        for platform, path in records.items():
            producer = read(path)['recordId']
            if not any(qualified(kit, platform, producer, material) for material in ('SdkMaterials', 'CustomShaders')):
                raise ValueError(f'No reviewed {platform} compatibility with the selected game release; qualification is required before cooking')
    engine_before = source_evidence(engine)['domains']
    for path in records.values():
        if read(path).get('sourceEvidence', {}).get('domains') != engine_before:
            raise ValueError('Engine inputs changed after recording; rebuild the source SDK and capture new records')
    before = snapshot_inputs(kit, mod)
    commandlet(kit, engine, 'RetakeMapValidate', f'-Mod={mod}', f'-Modes={modes}')
    destination = kit / ('Saved/ModKit/QualificationCandidates' if candidate else 'Exports') / mod
    final = destination / revision
    if final.exists() or (kit / 'Recovery' / mod / f'{revision}.source.zip').exists():
        raise ValueError('Revision already exists; exports are immutable')
    out = destination / f'.{revision}.partial'
    work = kit / 'Saved/ModKit/Cook' / mod
    remove_generated(kit, out); remove_generated(kit, work)
    out.mkdir(parents=True)
    uat = engine / ('Engine/Build/BatchFiles/RunUAT.bat' if os.name == 'nt' else 'Engine/Build/BatchFiles/RunUAT.sh')
    plugin = kit / 'Mods' / mod / f'{mod}.uplugin'
    registry_args = []
    for platform, (target, server) in PLATFORMS.items():
        stage = work / platform
        release_name = targets[platform]['consumerRecord']
        base = child(kit, f'Releases/{release_name}/{platform}')
        base.mkdir(parents=True, exist_ok=True)
        shutil.copy2(kit / 'ModKit/BaseCook' / platform / 'AssetRegistry.bin', base / 'AssetRegistry.bin')
        args = [uat, 'BuildCookRun', f'-project={kit / "RetakeModKit.uproject"}', '-nop4', '-unattended', '-utf8output',
                '-nocompileeditor', '-skipbuild', '-cook', '-stage', '-pak', '-iostore', '-VersionCookedContent',
                f'-DLCName={plugin}', f'-BasedOnReleaseVersion={release_name}', '-AdditionalCookerOptions=-DlcPluginOnly',
                f'-stagingdirectory={stage}']
        args += ['-noclient', '-server', f'-serverplatform={target}', '-serverconfig=Development'] if server else [f'-platform={target}', '-clientconfig=Development']
        run(args, cwd=kit, log=kit / 'Saved/ModKit/Logs' / f'cook-{mod}-{platform}.log')
        files = [p for p in stage.rglob('*') if p.suffix in ('.pak', '.utoc', '.ucas')]
        if not files or len({p.name for p in files}) != len(files):
            raise ValueError(f'Missing or ambiguous {platform} containers')
        (out / platform).mkdir()
        for path in files:
            shutil.copy2(path, out / platform / path.name)
        # DLC output is rooted at the plugin on UE6; avoid accidentally using a previous platform's registry.
        registries = [p for root in (kit / 'Saved/Cooked', plugin.parent / 'Saved/Cooked') for p in root.rglob('DevelopmentAssetRegistry.bin')
                      if platform in p.parts]
        if len(registries) != 1:
            raise ValueError(f'Expected one {platform} DevelopmentAssetRegistry.bin, found {len(registries)}')
        registry_args.extend([f'-CookedRegistry{platform}={registries[0]}', f'-EngineRecord{platform}={records[platform]}'])
    write(out / f'{mod}.uplugin', {'FileVersion': 3, 'Version': 1, 'VersionName': revision,
        'FriendlyName': mod, 'CanContainContent': True, 'ExplicitlyLoaded': True, 'Modules': [], 'RetakeModKind': kind})
    commandlet(kit, engine, 'RetakeModManifest', f'-Mod={mod}', f'-Revision={revision}', f'-Dir={out}', f'-Modes={modes}', *registry_args)
    manifest = verify_export(out)
    if not candidate:
        for entry in manifest['compatibility']:
            if not qualified(kit, entry['platform'], entry['engineRecordId'], entry['materialPath'], entry['customVersions']):
                raise ValueError(f'Unqualified {entry["platform"]}/{entry["materialPath"]}; use a qualified engine. Studio candidates stay under Saved.')
    else:
        manifest['qualificationCandidate'] = True
        write(out / 'manifest.json', manifest)
    if before != snapshot_inputs(kit, mod) or source_evidence(engine)['domains'] != engine_before:
        raise ValueError('Authoring or engine inputs changed during export; neither platform revision is published')
    archive = recover_archive(kit, mod, revision, before, manifest, records)
    try:
        os.rename(out, final)
    except OSError:
        archive.unlink()  # Only this operation's newly created recovery archive; existing revisions are never replaced.
        raise
    print(json.dumps({'export': str(final), 'recovery': str(archive), 'candidate': candidate}))
    return final


def recook(kit, engine, archive_path, revision, candidate=False):
    kit = Path(kit).resolve()
    with zipfile.ZipFile(archive_path) as archive:
        if archive.getinfo('recovery.json').file_size > 8 * 1024 * 1024:
            raise ValueError('Recovery metadata is too large')
        metadata = json.loads(archive.read('recovery.json'))
        mod = metadata['modId']
        if metadata.get('format') != 1 or not MOD.fullmatch(mod):
            raise ValueError('Unknown recovery archive')
        hashes = metadata.get('sourceHashes')
        if not isinstance(hashes, dict) or not hashes or len(hashes) > 200000:
            raise ValueError('Invalid recovery source inventory')
        if any(not isinstance(h, str) or not re.fullmatch(r'[0-9a-f]{64}', h) for h in hashes.values()):
            raise ValueError('Invalid recovery source hash')
        names = archive.namelist()
        expected_names = {'recovery.json', *('mod/' + relative for relative in hashes)}
        if set(names) != expected_names or len({n.casefold() for n in names}) != len(names):
            raise ValueError('Recovery archive contains undeclared or duplicate entries')
        source_art = set()
        if retake_sources.MANIFEST in hashes:
            entry = 'mod/' + retake_sources.MANIFEST
            if archive.getinfo(entry).file_size > 8 * 1024 * 1024:
                raise ValueError('SourceFiles.json exceeds 8 MiB')
            source_art = set(retake_sources.declared_art(json.loads(archive.read(entry))))
            if not source_art.issubset(hashes):
                raise ValueError('Recovery archive is missing declared source art')
        destination = kit / 'Mods' / mod
        if destination.exists():
            raise ValueError('Restore into a clean kit workspace; an existing creator mod is never overwritten')
        if sum(info.file_size for info in archive.infolist()) > 100 * 1024**3:
            raise ValueError('Recovery archive exceeds the 100 GiB extraction limit')
        temporary = kit / 'Saved/ModKit/Restore' / uuid.uuid4().hex
        temporary.mkdir(parents=True)
        try:
            for relative, expected in metadata['sourceHashes'].items():
                if not retake_sources.allowed_recovery_path(relative, mod, source_art):
                    raise ValueError(f'Recovery archive contains undeclared editable source: {relative}')
                path = child(temporary, relative)
                path.parent.mkdir(parents=True, exist_ok=True)
                with archive.open('mod/' + relative) as src, path.open('wb') as dst:
                    shutil.copyfileobj(src, dst)
                if digest(path) != expected:
                    raise ValueError(f'Recovery hash mismatch: {relative}')
            validate_descriptor(temporary / f'{mod}.uplugin')
            if set(retake_sources.inventory(temporary, mod)) != set(hashes):
                raise ValueError('Recovered editable inventory does not match the archive')
            destination.parent.mkdir(parents=True, exist_ok=True)
            os.rename(temporary, destination)
        finally:
            remove_generated(kit, temporary)
    # Use this kit's CURRENT qualified SDK/config. Historical metadata is evidence, not permission to overwrite the kit.
    kind = read(destination / f'{mod}.uplugin').get('RetakeModKind', 'map')
    try:
        if kind == 'weapons':
            commandlet(kit, engine, 'RetakeWeaponPoseBake', f'-Mod={mod}', '-AllProfiles')
        elif kind == 'map':
            # The shared readiness workflow preserves current custom artwork and
            # only regenerates required data. Stale custom alignment needs author review.
            prepare_maps(kit, engine, mod)
        else:
            raise ValueError('The recovered mod kind has no supported recook workflow')
        return export(kit, engine, mod, revision, candidate=candidate)
    except (ValueError, RuntimeError, OSError) as error:
        preparation = 'prepare-weapons --all-profiles' if kind == 'weapons' else 'prepare'
        raise RuntimeError(f'Editable source was restored to {destination} and has been kept. {error} '
                           f'Continue in that workspace with {preparation} --mod {mod}, then '
                           f'export --mod {mod} --revision {revision}. Do not extract the archive again.') from error


def validate_descriptor(path, expected_kind=None):
    descriptor = read(path)
    kind = descriptor.get('RetakeModKind', 'map')
    if kind not in ('map', 'weapons', 'game-mode'):
        raise ValueError('Unknown workspace mod kind; use the supported creator type selector')
    if expected_kind and kind != expected_kind:
        raise ValueError(f'This operation requires a {expected_kind} mod, but the workspace is {kind}')
    forbidden = ('Modules', 'Plugins', 'LocalizationTargets', 'PreBuildSteps', 'PostBuildSteps', 'AdditionalPluginDirectories')
    if any(descriptor.get(k) for k in forbidden) or descriptor.get('CanContainContent') is not True or descriptor.get('CanContainVerse') or descriptor.get('IsPluginExtension'):
        raise ValueError('A creator workspace must be content-only: no code, plugin dependencies, config or build steps')
    if (Path(path).parent / 'Config').exists() or (Path(path).parent / 'Source').exists() or (Path(path).parent / 'Binaries').exists():
        raise ValueError('Creator workspaces cannot contain Config, Source or Binaries folders')


def compatibility_status(kit):
    """Shared saved engine/game readiness for both editor workflows."""
    sdk = read(kit / 'SDK.lock.json')
    targets = {row['name']: row for row in sdk.get('shipped', {}).get('targets', [])}
    platforms = []
    for platform in PLATFORMS:
        record = kit / 'Saved/ModKit/EngineRecords' / f'{platform}.json'
        target = targets.get(platform, {})
        producer = read(record).get('recordId', '') if record.is_file() else ''
        configured = bool(producer and target.get('consumerRecord') and (kit / 'ModKit/BaseCook' / platform / 'AssetRegistry.bin').is_file())
        platforms.append({'platform': platform, 'configured': configured,
            'localTestOnly': target.get('qualificationOnly') is True,
            'qualified': configured and any(qualified(kit, platform, producer, material) for material in ('SdkMaterials', 'CustomShaders'))})
    candidate = bool(platforms) and all(row['localTestOnly'] for row in platforms)
    return {'sdkCurrent': sdk_source_hash(kit) == sdk.get('sourceHash'), 'platforms': platforms,
            'candidateOnly': candidate,
            'compatibilityConfigured': all(row['configured'] and (candidate or row['qualified']) for row in platforms)}



def draft_status(kit, engine, mod, report=None):
    if not MOD.fullmatch(mod): raise ValueError('Select a valid mod workspace')
    validate_descriptor(kit / 'Mods' / mod / f'{mod}.uplugin', expected_kind='weapons')
    report = Path(report) if report else kit / 'Saved/ModKit/Reports' / f'{mod}-{time.time_ns()}.draft.json'
    native = kit / 'Saved/ModKit/Reports' / f'{mod}-{time.time_ns()}.draft.native.json'
    try:
        commandlet(kit, engine, 'RetakeMapValidate', f'-Mod={mod}', '-AuthoringDraft', f'-Report={native}')
    except RuntimeError:
        if not native.is_file(): raise
        result = read(native)
        if result.get('complete') is not True or result.get('kind') != 'weapons' or result.get('modId') != mod or result.get('valid') is not False: raise
    result = read(native)
    if result.get('format') != 1 or result.get('complete') is not True or result.get('kind') != 'weapons' or result.get('modId') != mod:
        raise ValueError('Weapon authoring check returned an incompatible report')
    result.update(compatibility_status(kit))
    result['ready'] = bool(result['valid'] and result['sdkCurrent'] and result['compatibilityConfigured'])
    if not result['compatibilityConfigured']:
        result.setdefault('issues', []).append({'error': True, 'code': 'Compatibility', 'actor': '',
            'detail': 'Export compatibility is not ready. Enable authoring tools in the workspace, then check again. If this remains, update the kit and Retake; the engine/game combination needs a published compatibility record.'})
    write(report, result)
    print('event=CreatorDraftReadiness ' + json.dumps({'report': str(report), 'valid': result['valid'],
          'compatibilityReady': result['compatibilityConfigured'], 'exportChecksPreparation': True}), flush=True)
    return result


def authoring_status(kit, engine, mod, modes='auto', report=None):
    """Saved-content readiness for the editor and CLI. Export still performs full cooked verification."""
    if not MOD.fullmatch(mod):
        raise ValueError('Select a valid mod workspace')
    validate_descriptor(kit / 'Mods' / mod / f'{mod}.uplugin', expected_kind='map')
    report = Path(report) if report else kit / 'Saved/ModKit/Reports' / f'{mod}-{time.time_ns()}.json'
    native = kit / 'Saved/ModKit/Reports' / f'{mod}-{time.time_ns()}.native.json'
    operation_error = None
    log = kit / 'Saved/ModKit/Logs'
    try:
        log = commandlet(kit, engine, 'RetakeMapValidate', f'-Mod={mod}', f'-Modes={modes}', f'-Report={native}')
    except RuntimeError as error:
        # A completed invalid report is useful authoring feedback; a crash or missing report is an operation failure.
        if not native.is_file():
            raise
        operation_error = error
    if not native.is_file():
        raise RuntimeError('Map check could not complete: the native validator did not write a readiness report. '
                           'The creator scripts and compiled SDK editor tools may be out of sync. '
                           'Update and rebuild the Mod Kit, then restart its editor. '
                           f'Validator log: {log}')
    try:
        result = read(native)
    except (ValueError, OSError) as error:
        raise RuntimeError(f'Map check returned an unreadable readiness report. See {native} and {log}') from error
    if (not isinstance(result, dict) or result.get('format') != 1 or result.get('complete') is not True
            or result.get('modId') != mod or not isinstance(result.get('valid'), bool)
            or not isinstance(result.get('maps'), list) or not isinstance(result.get('issues'), list)):
        raise RuntimeError(f'Map check returned an incomplete or incompatible readiness report. Update the Mod Kit; see {native}')
    if operation_error and result['valid'] is not False:
        raise operation_error
    compatibility = compatibility_status(kit)
    compatible, current = compatibility['compatibilityConfigured'], compatibility['sdkCurrent']
    preparation = [step for m in result['maps'] for step in m.get('preparation', [])]
    if any({step.get('id') for step in m.get('preparation', [])} != {'acoustics', 'minimap', 'visibility'}
           for m in result['maps']):
        raise RuntimeError('The native map checker lacks preparation details. Update and rebuild the Mod Kit editor tools.')
    if any(step.get('state') not in ('current', 'unused', 'needed', 'blocked') for step in preparation):
        raise RuntimeError('The native map checker returned an unknown preparation state. Update the Mod Kit.')
    baked = bool(result['maps']) and all(step['state'] in ('current', 'unused') for step in preparation)
    result.update(compatibility)
    result.update({'sdkCurrent': current,
        'compatibilityConfigured': compatible, 'preparationComplete': baked,
        'preparationNeeded': any(s['state'] == 'needed' for s in preparation),
        'preparationBlocked': any(s['state'] == 'blocked' for s in preparation),
        'ready': bool(result.get('valid') and current and compatible and baked),
        'checkedAt': time.time(), 'exportChecksCookedContent': True})
    write(report, result)
    print('event=CreatorReadiness ' + json.dumps({'report': str(report), 'valid': result['valid'], 'ready': result['ready']}), flush=True)
    return result


def prepare_maps(kit, engine, mod, map_name=None, modes='auto', report=None):
    initial = authoring_status(kit, engine, mod, modes, report)
    maps = [m for m in initial['maps'] if not map_name or m['name'] == map_name]
    if not maps or any(not re.fullmatch(r'[A-Za-z0-9_]+', m['name']) for m in maps):
        raise ValueError('Select a saved map in this mod')
    blocked = [f"{m['name']}: {s['detail']}" for m in maps for s in m['preparation'] if s['state'] == 'blocked']
    if blocked:
        raise ValueError('Fix preparation setup first. ' + ' '.join(blocked))
    changed = False
    for m in maps:
        for step in m['preparation']:
            if step['state'] != 'needed':
                print('event=PreparationSkipped ' + json.dumps({'map': m['name'], **step}), flush=True)
                continue
            changed = True
            if step['id'] == 'visibility':
                bake_visibility(kit, engine, mod, m['name'])
            else:
                commandlet(kit, engine, {'acoustics': 'RetakeAcousticsBake', 'minimap': 'RetakeMinimapBake'}[step['id']],
                           f'-Mod={mod}', f"-Map={m['name']}")
    return authoring_status(kit, engine, mod, modes, report) if changed else initial


def bake_visibility(kit, engine, mod, map_name):
    commandlet(kit, engine, 'RetakeVisibilityBake', f'-Mod={mod}', f'-Map={map_name}', '-Phase=Prepare')
    folder = kit / 'Saved/ModKit/Visibility' / mod / map_name / str(time.time_ns())
    folder.mkdir(parents=True)
    scene, baked = folder / 'scene.json', folder / 'bake.json'
    commandlet(kit, engine, 'RetakeVisibilityBake', f'-Mod={mod}', f'-Map={map_name}', '-Phase=Export', f'-File={scene}')
    run([sys.executable, kit / 'Scripts/Visibility/bake_visibility.py', scene, baked], cwd=kit, log=folder / 'baker.log')
    commandlet(kit, engine, 'RetakeVisibilityBake', f'-Mod={mod}', f'-Map={map_name}', '-Phase=Import', f'-File={baked}')


def test_information(kit, export_dir, game=None, server=None, server_only=False,
                     map_name=None, mode=None, base_map=None, weapon_profile=None):
    if __package__:
        from .retake_test_preflight import check
        from .retake_test_server import status
        from .retake_test_launch import select
    else:
        from retake_test_preflight import check
        from retake_test_server import status
        from retake_test_launch import select
    result = check(export_dir, game, server, verify_export, digest, engine_record_id, server_only)
    try:
        result['launchSelection'] = select(verify_export(Path(export_dir)), map_name, mode, base_map, weapon_profile)
        if result['launchSelection']['weaponSelection']:
            for label in ('client', 'server'):
                item = result.get(label)
                if not item or not item['allowed']:
                    continue
                maps = item.get('testMaps')
                if not isinstance(maps, list) or any(not isinstance(m, str) for m in maps):
                    result['reasons'].append(f'The selected {label} does not advertise installed test maps. Choose a current package.')
                elif base_map not in maps:
                    result['reasons'].append(f'The selected {label} does not contain this playable base map: {base_map}')
            result['ready'] = not result['reasons']
    except ValueError as error:
        result['reasons'].append(str(error))
        result['ready'] = False
    selected_server = result.get('server')
    if selected_server and selected_server['allowed'] and selected_server.get('testLaunchVersion') != 2:
        result['reasons'].append('The server package lacks the current local-test startup contract; choose a current package.')
        result['ready'] = False
    result['serverStatus'] = status(kit)
    return result


def launch_test(kit, export_dir, game, mods_dir, map_name=None, mode=None, server=None, port=17777, distribution='', user='', server_only=False,
                base_map=None, weapon_profile=None):
    """User-triggered installed-game launch. Never runs smoke tests or stops an existing game."""
    if not export_dir or not mods_dir or (not game and not server_only) or (server_only and not server):
        raise ValueError('Select an exported revision and the installed client/server required for this launch')
    preflight = test_information(kit, export_dir, game, server, server_only, map_name, mode, base_map, weapon_profile)
    if not preflight['ready']:
        raise ValueError('Cannot launch this export: ' + ' '.join(preflight['reasons']))
    game = Path(preflight['client']['nativeExecutable']) if not server_only else None
    server = Path(server).resolve() if server else None
    if server and server.is_dir():
        server = server / 'RetakeServer.sh'
    manifest = verify_export(Path(export_dir))
    qualification_test = bool(manifest.get('qualificationCandidate'))
    if qualification_test:
        expected = (Path(kit) / 'Saved/ModKit/TestMods').resolve()
        if Path(mods_dir).resolve() != expected:
            raise ValueError('Qualification testing must use the isolated Kit Saved/ModKit/TestMods folder.')
        installations = [preflight.get('client'), preflight.get('server')]
        if any(not item.get('qualificationOnly') for item in installations if item):
            raise ValueError('Qualification testing requires matching Development pilot installations.')
    selection = preflight['launchSelection']
    selected, mode, weapon_selection = selection['map'], selection['mode'], selection['weaponSelection']
    profile = kit / 'Saved/ModKit/PlayProfiles' / manifest['modId']
    session = profile / 'session.json'
    for active_session in (kit / 'Saved/ModKit/PlayProfiles').glob('*/session.json') if os.name == 'nt' else []:
        # Only inspect the PID saved by this launcher. Never terminate or alter user-owned game processes.
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        recorded = read(active_session)
        handle = kernel.OpenProcess(0x1000, False, int(recorded.get('pid', 0)))
        if handle:
            code = wintypes.DWORD()
            alive = bool(kernel.GetExitCodeProcess(handle, ctypes.byref(code)) and code.value == 259)
            image = ctypes.create_unicode_buffer(32768)
            size = wintypes.DWORD(len(image))
            alive = alive and bool(kernel.QueryFullProcessImageNameW(handle, 0, image, ctypes.byref(size))) and Path(image.value).resolve() == Path(recorded.get('game', '')).resolve()
            kernel.CloseHandle(handle)
            if alive: raise ValueError('A Mod Kit test client is still running; close it before installing or launching another revision')
    reuse_server = None
    if server:
        if __package__:
            from .retake_test_server import stop
        else:
            from retake_test_server import stop
        status = preflight['serverStatus']
        if status['state'] == 'unknown':
            raise ValueError(status['detail'])
        if status['running']:
            existing = status['session']
            matches = (existing.get('manifestHash') == digest(Path(export_dir) / 'manifest.json')
                       and existing.get('map') == selected['mapId'] and existing.get('mode') == mode
                       and existing.get('weaponSelection') == weapon_selection
                       and existing.get('modsDir') is not None and Path(existing['modsDir']).resolve() == Path(mods_dir).resolve()
                       and existing.get('port') == port and Path(existing.get('server', '')).resolve() == Path(server).resolve()
                       and existing.get('binaryHash') == preflight['server'].get('executableHash')
                       and (not distribution or existing.get('distribution') == distribution)
                       and (not user or existing.get('user') == user))
            if matches:
                reuse_server = existing
            else:
                stop(existing)
    if reuse_server:
        installed = child(Path(mods_dir), manifest['modId'])
        try:
            verify_export(installed)
            if digest(installed / 'manifest.json') != digest(Path(export_dir) / 'manifest.json'):
                raise ValueError('Installed revision does not match the running server selection')
        except (OSError, ValueError) as error:
            raise ValueError("The running test server's installed content changed; stop it before reinstalling. " + str(error)) from error
    else:
        install(Path(export_dir), Path(mods_dir), qualification_test=qualification_test)
    profile.mkdir(parents=True, exist_ok=True)
    destination = selected['packageName'] + f'?Mode={mode}?Profile=Casual'
    if server:
        if __package__:
            from .retake_test_server import start
        else:
            from retake_test_server import start
        if reuse_server:
            server_session = reuse_server
            destination = f"{server_session['address']}:{server_session['port']}"
        else:
            destination, server_session = start(kit, server, mods_dir, manifest, selected, mode, port, distribution, user,
                digest(Path(export_dir) / 'manifest.json'), preflight['server']['executableHash'], weapon_selection=weapon_selection)
        print('event=TestServerReady ' + json.dumps(server_session), flush=True)
        if server_only:
            print(json.dumps({'serverStarted': True, 'address': destination, 'map': selected['mapId'], 'mode': mode}), flush=True)
            return
    log_root = Path(kit) / 'Saved/ModKit/TestSessions'
    log_root.mkdir(parents=True, exist_ok=True)
    client_log = log_root / ('client-' + manifest['modId'] + '-' + str(time.time_ns()) + '.log')
    command = [str(game), destination,
        '-RetakeModsDir=' + str(Path(mods_dir).resolve()), '-UserDir=' + profile.resolve().as_posix() + '/',
        '-abslog=' + client_log.resolve().as_posix(), '-windowed']
    if weapon_selection and not server:
        if __package__:
            from .retake_test_launch import write_weapon_config
        else:
            from retake_test_launch import write_weapon_config
        config = write_weapon_config(profile / 'WeaponGame.ini', weapon_selection)
        command.append('-GameINI=' + config.resolve().as_posix())
    if manifest.get('qualificationCandidate'): command.append('-RetakeModQualification')
    process = subprocess.Popen(command, cwd=game.parent, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == 'nt' else 0)
    write(session, {'pid': process.pid, 'game': str(game), 'map': selected['mapId'], 'revision': manifest['revision'],
                    'weaponSelection': weapon_selection, 'log': str(client_log), 'modsDir': str(Path(mods_dir).resolve())})
    print(json.dumps({'launched': True, 'pid': process.pid, 'map': selected['mapId'], 'mode': mode, 'log': str(client_log)}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['discover', 'setup', 'build', 'doctor', 'contract', 'record', 'create', 'example', 'weapon-basis', 'status', 'draft-status', 'prepare', 'prepare-weapons', 'validate', 'minimap', 'acoustics', 'geometry', 'visibility', 'export', 'install', 'test', 'start-test-server', 'test-info', 'server-status', 'stop-test-server', 'recook', 'qualify'])
    parser.add_argument('--kit', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--engine', type=Path)
    parser.add_argument('--mod')
    parser.add_argument('--map', help='Playable map name for visibility preparation')
    parser.add_argument('--template', choices=['Empty', 'Basic'], default='Empty')
    parser.add_argument('--kind', choices=['map', 'weapons', 'game-mode'], default='map', help='creator workspace type; game-mode creation is not enabled')
    parser.add_argument('--revision')
    parser.add_argument('--modes', default='auto')
    parser.add_argument('--candidate', action='store_true', help='studio-only unqualified output under Saved; never a public export')
    parser.add_argument('--export-dir', type=Path)
    parser.add_argument('--mods-dir', type=Path)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--receipt', type=Path, help='reviewed user-run qualification receipt (qualify only)')
    parser.add_argument('--request', type=Path, help='owned weapon pose preparation JSON request (prepare-weapons only)')
    parser.add_argument('--contribution', help='owned pose contribution to rebuild from its saved recipe (prepare-weapons only)')
    parser.add_argument('--all-profiles', action='store_true', help='rebuild every catalogue contribution from its saved recipe (prepare-weapons only)')
    parser.add_argument('--report', type=Path, help='structured saved-content readiness report')
    parser.add_argument('--game', type=Path, help='installed Retake executable (test only)')
    parser.add_argument('--mode', help='one supported game-mode ID for the user-triggered test launch')
    parser.add_argument('--base-map', help='installed /Game/... map package for a weapon-only test launch')
    parser.add_argument('--weapon-profile', help='exact exported weapon profile for a weapon-only test launch')
    parser.add_argument('--custom-minimap-texture', help='mod-owned 1024px texture object path; minimap --map only')
    parser.add_argument('--replace-custom-minimap', action='store_true', help='explicitly switch the active custom minimap to a generated image')
    parser.add_argument('--server', type=Path, help='existing Linux server package or RetakeServer.sh for local WSL testing')
    parser.add_argument('--port', type=int, default=17777)
    parser.add_argument('--wsl-distribution', default='')
    parser.add_argument('--wsl-user', default='')
    args = parser.parse_args()
    kit = args.kit.resolve()
    if args.command == 'discover':
        if __package__:
            from .retake_installations import discover
        else:
            from retake_installations import discover
        result = discover(kit)
        if args.report: write(args.report, result)
        print(json.dumps(result)); return
    if args.command in ('test-info', 'server-status'):
        if args.command == 'server-status':
            if __package__:
                from .retake_test_server import status
            else:
                from retake_test_server import status
            result = {'serverStatus': status(kit)}
        else:
            if not args.export_dir:
                raise ValueError('Select an exported revision to check against the installed game')
            result = test_information(kit, args.export_dir, args.game, args.server,
                                      map_name=args.map, mode=args.mode, base_map=args.base_map, weapon_profile=args.weapon_profile)
        if args.report: write(args.report, result)
        print(json.dumps(result)); return
    if args.command == 'stop-test-server':
        if __package__:
            from .retake_test_server import stop
        else:
            from retake_test_server import stop
        ticket = kit / 'Saved/ModKit/TestSessions/server.json'
        if ticket.is_file():
            session = read(ticket); stop(session); session['stoppedAt'] = time.time(); write(ticket, session)
        print(json.dumps({'stopped': True})); return
    if args.command == 'install':
        print(install(args.export_dir, args.mods_dir)); return
    if not (kit / 'RetakeModKit.uproject').is_file():
        raise ValueError('Select a standalone RetakeModKit project with --kit')
    if args.command in ('test', 'start-test-server'):
        with lock(kit):
            launch_test(kit, args.export_dir, args.game, args.mods_dir, args.map, args.mode, args.server, args.port, args.wsl_distribution, args.wsl_user, args.command == 'start-test-server', args.base_map, args.weapon_profile)
        return
    engine = engine_root(kit, args.engine)
    with lock(kit):
        if args.command == 'status':
            authoring_status(kit, engine, args.mod or '', args.modes, args.report)
        elif args.command == 'draft-status':
            draft_status(kit, engine, args.mod or '', args.report)
        elif args.command == 'prepare':
            prepare_maps(kit, engine, args.mod or '', args.map, args.modes, args.report)
        elif args.command in ('setup', 'build'):
            if args.command == 'setup':
                write(kit / 'Saved/ModKit/engine.json', {'path': str(engine)})
                if os.name == 'nt' and not (kit / 'ModKit/Engine.json').is_file():
                    import winreg
                    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, r'Software\Epic Games\Unreal Engine\Builds') as key:
                        association = None
                        for index in range(winreg.QueryInfoKey(key)[1]):
                            name, value, _ = winreg.EnumValue(key, index)
                            if Path(value).resolve() == engine: association = name; break
                        if not association:
                            association = '{' + str(uuid.uuid5(uuid.NAMESPACE_URL, str(engine).lower())).upper() + '}'
                            winreg.SetValueEx(key, association, 0, winreg.REG_SZ, str(engine))
                    project = read(kit / 'RetakeModKit.uproject')
                    project['EngineAssociation'] = association
                    write(kit / 'RetakeModKit.uproject', project)
            prebuilt = retake_prebuilt.status(kit, engine) if args.command == 'setup' else {'usable': False, 'reason': 'Source build requested.'}
            print(('Using prebuilt editor modules. ' if prebuilt['usable'] else 'Building editor modules. ') + prebuilt['reason'], flush=True)
            if prebuilt['usable']:
                contract_check(kit, engine)
            else:
                build(kit, engine)
        elif args.command == 'doctor':
            report = contract_check(kit, engine)
            print(json.dumps({'contract': report, 'engine': str(engine), 'qualificationFile': (kit / 'ModKit/qualification.json').exists()}, indent=2))
        elif args.command == 'contract':
            print(json.dumps(contract_check(kit, engine), indent=2))
        elif args.command == 'record':
            record_engine(kit, engine)
        elif args.command == 'qualify':
            if not args.receipt: raise ValueError('--receipt is required')
            print(json.dumps(qualify(kit, args.receipt), indent=2))
        elif args.command == 'create':
            if not args.mod or not MOD.fullmatch(args.mod): raise ValueError('--mod must be RetakeMod_<Name>')
            if args.kind != 'map' and args.template != 'Empty': raise ValueError('Level templates apply only to map mods')
            options = ['-Basic'] if args.template == 'Basic' else []
            commandlet(kit, engine, 'RetakeModCreate', f'-Name={args.mod}', f'-Kind={args.kind}', *options)
        elif args.command == 'weapon-basis':
            if not args.mod or not MOD.fullmatch(args.mod): raise ValueError('--mod must name a weapon workspace')
            validate_descriptor(kit / 'Mods' / args.mod / f'{args.mod}.uplugin', expected_kind='weapons')
            commandlet(kit, engine, 'pythonscript', f'-Mod={args.mod}',
                       f'-script={kit / "Scripts/ModKit/retake_weapon_basis.py"}', f'-RetakeBasisMod={args.mod}', rendering=True)
        elif args.command == 'example':
            if not args.mod or not MOD.fullmatch(args.mod): raise ValueError('--mod must name a new example workspace')
            if args.kind not in ('map', 'weapons'):
                raise ValueError('Only map and weapon examples are supported')
            script = 'retake_example.py' if args.kind == 'map' else 'retake_weapon_example.py'
            if args.kind == 'weapons' and not (kit / 'Examples/Weapons/SourceArt/Files.json').is_file():
                raise ValueError('This kit has no weapon example source pack; use a complete released kit')
            options = ['-Basic'] if args.kind == 'map' else ['-Kind=weapons']
            commandlet(kit, engine, 'RetakeModCreate', f'-Name={args.mod}', *options)
            workspace = kit / 'Mods' / args.mod
            initial = retake_sources.inventory(workspace, args.mod)
            write(kit / 'Saved/ModKit' / f'example-{args.mod}.json', {
                'modId': args.mod,
                'kind': args.kind,
                'initialFiles': {p.relative_to(workspace).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in initial.values()},
            })
            commandlet(kit, engine, 'pythonscript', f'-Mod={args.mod}',
                       f'-script={kit / "Scripts/ModKit" / script}', f'-RetakeExampleMod={args.mod}', rendering=True)
            if args.kind == 'weapons':
                commandlet(kit, engine, 'RetakeWeaponPoseBake', f'-Mod={args.mod}',
                           f'-Request={workspace / "SourceArt/Example/Preparation.json"}')
                commandlet(kit, engine, 'RetakeMapValidate', f'-Mod={args.mod}')
        elif args.command == 'prepare-weapons':
            if not args.mod or not MOD.fullmatch(args.mod) or sum((bool(args.request), bool(args.contribution), args.all_profiles)) != 1:
                raise ValueError('Weapon preparation requires --mod and exactly one of --request, --contribution or --all-profiles')
            if args.all_profiles:
                commandlet(kit, engine, 'RetakeWeaponPoseBake', f'-Mod={args.mod}', '-AllProfiles')
            elif args.contribution:
                if not args.contribution.startswith(f'/{args.mod}/') or any(c in args.contribution for c in ('\\', ':', '\n', '\r', '"')):
                    raise ValueError('Choose an owned contribution object path')
                commandlet(kit, engine, 'RetakeWeaponPoseBake', f'-Mod={args.mod}', f'-Contribution={args.contribution}')
            else:
                request = args.request.resolve()
                if not request.is_file() or not 0 < request.stat().st_size <= 1024 * 1024:
                    raise ValueError('Choose a weapon preparation JSON request smaller than 1 MiB')
                job = read(request)
                if not isinstance(job, dict) or job.get('owner') != args.mod:
                    raise ValueError('The preparation request owner differs from --mod')
                commandlet(kit, engine, 'RetakeWeaponPoseBake', f'-Mod={args.mod}', f'-Request={request}')
        elif args.command in ('validate', 'minimap', 'acoustics'):
            if not args.mod or not MOD.fullmatch(args.mod): raise ValueError('--mod is required')
            options = [f'-Report={args.report}'] if args.report and args.command == 'validate' else []
            if args.map and args.command in ('minimap', 'acoustics'):
                if not re.fullmatch(r'[A-Za-z0-9_]+', args.map): raise ValueError('Select a valid saved map')
                options.append(f'-Map={args.map}')
            if args.command == 'minimap':
                if args.custom_minimap_texture:
                    options.append(f'-CustomTexture={args.custom_minimap_texture}')
                if args.replace_custom_minimap:
                    options.append('-ReplaceCustom')
            commandlet(kit, engine, {'validate':'RetakeMapValidate', 'minimap':'RetakeMinimapBake', 'acoustics':'RetakeAcousticsBake'}[args.command], f'-Mod={args.mod}', f'-Modes={args.modes}', *options)
        elif args.command in ('geometry', 'visibility'):
            if not args.mod or not MOD.fullmatch(args.mod) or not args.map or not re.fullmatch(r'[A-Za-z0-9_]+', args.map):
                raise ValueError('--mod and --map are required for visibility preparation')
            if args.command == 'geometry':
                commandlet(kit, engine, 'RetakeVisibilityBake', f'-Mod={args.mod}', f'-Map={args.map}', '-Phase=Prepare')
            else:
                bake_visibility(kit, engine, args.mod, args.map)
        elif args.command == 'export':
            export(kit, engine, args.mod or '', args.revision or '', args.modes, args.candidate)
        elif args.command == 'recook':
            if not args.source or not args.revision: raise ValueError('--source and --revision are required')
            recook(kit, engine, args.source, args.revision, args.candidate)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print(f'event=ModKitFailed reason={error}', file=sys.stderr)
        raise SystemExit(1)
