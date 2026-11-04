"""Select an installed-game launch without starting processes or loading Unreal assets."""
from __future__ import annotations

import re
from pathlib import Path


def select(manifest, map_name=None, mode=None, base_map=None, weapon_profile=None):
    if manifest['format'] == 'RetakeWeapons':
        if map_name:
            raise ValueError('Weapon packs contain no maps; select an installed map with --base-map.')
        if not isinstance(base_map, str) or not re.fullmatch(r'/Game/(?:[A-Za-z0-9_]+/)*[A-Za-z0-9_]+', base_map):
            raise ValueError('Select an installed base-map package with --base-map /Game/... .')
        if not isinstance(mode, str) or not re.fullmatch(r'[A-Za-z0-9_]{1,32}', mode):
            raise ValueError('Select the installed map\'s game mode with --mode.')
        metadata = manifest.get('testLaunch', {})
        profiles = metadata.get('weaponProfiles') if isinstance(metadata, dict) else None
        digest = metadata.get('contentDigest') if isinstance(metadata, dict) else None
        if (not isinstance(profiles, list) or not profiles
                or any(not isinstance(p, str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', p)
                       or p.casefold() == 'none' for p in profiles)
                or len({p.casefold() for p in profiles}) != len(profiles)
                or not isinstance(digest, str) or not re.fullmatch(r'[0-9a-f]{12}', digest)):
            raise ValueError('Weapon export lacks native launch metadata; export a new revision with the current kit.')
        if weapon_profile not in profiles:
            raise ValueError('Choose --weapon-profile from this export: ' + ', '.join(profiles))
        # These are selection hints, not authority: the runtime recomputes the digest, loads the
        # hashed catalogue and validates the selected profile and installed map before admission.
        return {'map': {'mapId': base_map, 'packageName': base_map}, 'mode': mode,
                'weaponSelection': {'packId': manifest['modId'], 'revision': manifest['revision'],
                                    'contentDigest': digest, 'profileId': weapon_profile},
                'mapAndProfileAdmission': 'runtime'}
    if base_map or weapon_profile:
        raise ValueError('--base-map and --weapon-profile apply only to a weapon-pack export.')
    maps = [m for m in manifest['maps'] if not map_name or map_name in (m['mapId'], m['packageName'].rsplit('/', 1)[-1])]
    if len(maps) != 1:
        raise ValueError('Select one map from this export.')
    selected = maps[0]
    mode = mode or ('FFA' if 'FFA' in selected['supportedModes'] else selected['supportedModes'][0])
    if mode not in selected['supportedModes']:
        raise ValueError('This export does not support the selected game mode.')
    return {'map': selected, 'mode': mode, 'weaponSelection': None, 'mapAndProfileAdmission': 'runtime'}


def write_weapon_config(path, selection, preset_defaults=None):
    """Write a launcher-owned generated Game.ini; the installed/default configs stay untouched."""
    fields = (('PackId', 'packId', r'RetakeMod_[A-Za-z0-9_]{1,38}'),
              ('Revision', 'revision', r'[A-Za-z0-9]{1,32}'),
              ('ContentDigest', 'contentDigest', r'[0-9a-f]{12}'),
              ('ProfileId', 'profileId', r'[A-Za-z0-9_-]{1,64}'))
    values = []
    for native, key, pattern in fields:
        value = selection.get(key)
        if not isinstance(value, str) or not re.fullmatch(pattern, value) or value.casefold() == 'none':
            raise ValueError('Invalid exact weapon selection: ' + key)
        values.append(f'{native}="{value}"')
    defaults = []
    seen = set()
    for row in preset_defaults or []:
        if set(row) != {'gameplayPreset', 'packId', 'profileId', 'loadoutPolicy'}:
            raise ValueError('Unexpected weapon preset-default fields.')
        preset = row['gameplayPreset']
        if preset not in {'Standard', 'Realism', 'Classic', 'ClassicRealism', 'Competitive', 'ProMod'} or preset in seen:
            raise ValueError('Unknown or duplicate gameplay preset default.')
        if row['packId'] != selection['packId']:
            raise ValueError('A preset default cannot enable another weapon pack.')
        if not isinstance(row['profileId'], str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', row['profileId']) or row['profileId'].casefold() == 'none':
            raise ValueError('Invalid preset weapon profile.')
        if not isinstance(row['loadoutPolicy'], str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,128}', row['loadoutPolicy']) or row['loadoutPolicy'].casefold() == 'none':
            raise ValueError('Invalid preset loadout policy.')
        seen.add(preset)
        defaults.append('PresetDefaults=(GameplayPreset="' + preset + '",PackId="' + row['packId']
                        + '",ProfileId="' + row['profileId'] + '",LoadoutPolicy="' + row['loadoutPolicy'] + '")\n')
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    # GameINI selects the generated layer. Unreal replaces inherited values for this saved
    # key before applying it, so a prior/default pack selection cannot leak into this launch.
    path.write_text('[/Script/Retake.RetakeWeaponPackSettings]\nEnabledPacks=('
                    + ','.join(values) + ')\n'
                    + 'bUsePresetDefaults=' + ('True' if defaults else 'False') + '\n'
                    + ''.join(defaults), encoding='utf-8')
    return path
