"""Read-only checks against the selected installed binaries and their shipped mod policy."""
from __future__ import annotations

import json
import re
from pathlib import Path


def _read(path):
    if not 0 < path.stat().st_size <= 16 * 1024 * 1024:
        raise ValueError(f'Compatibility metadata has an invalid size: {path}')
    value = json.loads(path.read_text(encoding='utf-8-sig'))
    if not isinstance(value, dict):
        raise ValueError(f'Compatibility metadata must be an object: {path}')
    return value


def installation(path, platform, manifest, digest, engine_record_id):
    label = 'client' if platform == 'Windows' else 'server'
    result = {'platform': platform, 'path': str(path or ''), 'state': 'blocked', 'allowed': False}
    try:
        if not path:
            raise ValueError(f'Choose the installed {label}.')
        path = Path(path).resolve(strict=True)
        project = None
        for parent in [path.parent, *list(path.parents)[:5]]:
            for candidate in (parent / 'Retake', parent):
                if (candidate / 'ModKit/compatibility.json').is_file():
                    project = candidate
                    break
            if project:
                break
        if project is None:
            raise ValueError(f'This {label} has no Mod Kit compatibility metadata. Choose a mod-capable game package.')
        policy = _read(project / 'ModKit/compatibility.json')
        if policy.get('format') != 1 or policy.get('platform') != platform:
            raise ValueError(f'The selected {label} has incompatible qualification metadata.')
        binaries = project / 'Binaries' / ('Win64' if platform == 'Windows' else 'Linux')
        names = ('Retake.exe', 'Retake-Win64-Shipping.exe') if platform == 'Windows' else ('RetakeServer', 'RetakeServer-Linux-Shipping')
        candidates = [path] if path.parent == binaries and path.name in names else [binaries / name for name in names]
        native = next((p for p in candidates if p.is_file() and digest(p) == policy.get('executableHash')), None)
        if native is None:
            raise ValueError(f'The installed {label} binary does not match its qualification metadata. Choose a complete matching package.')
        result['nativeExecutable'] = str(native)
        result['identityVerified'] = True
        result['executableHash'] = policy['executableHash']
        cook = next((c for c in manifest['compatibility'] if c['platform'] == platform), None)
        if cook is None:
            raise ValueError(f'This export has no {platform} artifacts.')
        candidate_only = policy.get('qualificationOnly') is True
        if manifest.get('qualificationCandidate') and not candidate_only:
            raise ValueError(f'This candidate export requires a Development qualification {label}; public installations cannot load it.')
        pairs = [p for p in policy.get('pairs', []) if isinstance(p, dict)
                 and p.get('platform') == platform and p.get('producerRecord') == cook['engineRecordId']
                 and p.get('consumerRecord') == policy.get('consumerRecord')
                 and p.get('materialPath') == cook['materialPath'] and isinstance(p.get('evidence'), list) and p['evidence']
                 and (p.get('reviewed') is True or (candidate_only and p.get('producerRecord') == p.get('consumerRecord')))]
        if not pairs:
            raise ValueError(f'This {label} has no approved compatibility record for the selected export. Choose a matching qualified or Development pilot package.')
        consumer = _read(project / 'ModKit/engine-record.json')
        if consumer.get('recordId') != policy.get('consumerRecord') or engine_record_id(consumer) != consumer.get('recordId'):
            raise ValueError(f'This {label}\'s engine record is missing, stale or does not match its policy.')
        if not set(cook.get('structuralProfile', [])).issubset(set(consumer.get('structuralProfile', []))):
            raise ValueError(f'This {label} uses different collision or surface settings.')
        if not set(cook.get('foreignPackages', [])).issubset(set(policy.get('packages', []))):
            raise ValueError(f'This {label} is missing a required shared package.')
        modules = set(policy.get('scriptModules', []))
        if any(not c.startswith('/Script/') or c[8:].split('.', 1)[0] not in modules for c in cook.get('nativeClasses', [])):
            raise ValueError(f'This {label} is missing a required native module.')
        versions = cook.get('customVersions', {})
        if not any(all(guid in pair.get('customVersions', {})
                       and pair['customVersions'][guid]['min'] <= version <= pair['customVersions'][guid]['max']
                       for guid, version in versions.items()) for pair in pairs):
            raise ValueError(f'This {label} has no readable custom-version range for the export.')
        minimum, maximum = policy.get('oldestReadableContract'), policy.get('sdkContractVersion')
        # Older packages publish their current contract in the base manifest, but no readable floor.
        # Their own current contract is known readable; never guess compatibility with other versions.
        if type(minimum) is not int or type(maximum) is not int:
            suffix = 'Win64' if platform == 'Windows' else 'Linux'
            base = project.parent / f'Manifest_RetakeModBase_{suffix}.json'
            current = _read(base).get('sdkContractVersion') if base.is_file() else None
            minimum = maximum = current
        if type(minimum) is not int or type(maximum) is not int or not minimum <= manifest['sdkContractVersion'] <= maximum:
            raise ValueError(f'This {label} does not advertise support for SDK contract {manifest["sdkContractVersion"]}. Choose a package with a matching readable contract.')
        replay = project.parent / 'ReplayPlayer.json'
        if replay.is_file():
            data = _read(replay)
            result['release'] = data.get('release', '')
            result['liveProtocolVersion'] = data.get('liveProtocolVersion')
            result['hitRigContentHash'] = data.get('hitRigContentHash')
        for key in ('liveProtocolVersion', 'hitRigContentHash', 'testMaps', 'testLaunchVersion'):
            if key in policy:
                result[key] = policy[key]
        result.update({'allowed': True, 'state': 'local_test' if candidate_only else 'compatible',
                       'detail': 'Installed binary and export compatibility records match.' +
                                 (' Development qualification mode is required.' if candidate_only else ''),
                       'qualificationOnly': candidate_only})
    except (OSError, ValueError, KeyError, TypeError) as error:
        result['detail'] = str(error)
    return result


def check(export_dir, game, server, verify_export, digest, engine_record_id, server_only=False):
    manifest = verify_export(Path(export_dir))
    client = None if server_only else installation(game, 'Windows', manifest, digest, engine_record_id)
    server_result = installation(server, 'LinuxServer', manifest, digest, engine_record_id) if server else None
    reasons = [r['detail'] for r in (client, server_result) if r and not r['allowed']]
    if server_result and client and server_result['allowed'] and client['allowed']:
        for item in (client, server_result):
            if (type(item.get('liveProtocolVersion')) is not int or item['liveProtocolVersion'] < 1
                    or not isinstance(item.get('hitRigContentHash'), str)
                    or not re.fullmatch(r'[0-9a-f]{32}', item['hitRigContentHash'])):
                reasons.append('Selected client/server lacks join-compatibility metadata. Choose current matching packages.')
                break
        else:
            if client['liveProtocolVersion'] != server_result['liveProtocolVersion']:
                reasons.append('Selected client and server use different live network protocols. Choose a matching pair.')
            if client['hitRigContentHash'] != server_result['hitRigContentHash']:
                reasons.append('Selected client and server use different hit-rig content. Choose a matching pair.')
    return {'format': 1, 'complete': True, 'modId': manifest['modId'], 'revision': manifest['revision'],
            'export': str(Path(export_dir).resolve()), 'client': client, 'server': server_result,
            'ready': not reasons, 'reasons': reasons, 'runtimeAdmissionStillRequired': True}
