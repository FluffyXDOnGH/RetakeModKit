"""Create the original blockout weapon example in a NEW creator-owned workspace.

Executed by the kit's Unreal Python commandlet. No game or migrated weapon assets are inputs.
"""
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import struct
import sys
import wave
import unreal

sys.path.insert(0, str(Path(__file__).resolve().parent))
import retake_weapon_basis
import retake_sources

match = re.search(r'(?:^|\s)-RetakeExampleMod=(RetakeMod_[A-Za-z0-9_]+)(?:\s|$)', unreal.SystemLibrary.get_command_line())
if not match:
    raise RuntimeError('Select a fresh weapon example workspace with -RetakeExampleMod')
mod = match.group(1)
kit = Path(unreal.Paths.project_dir()).resolve()
workspace = kit / 'Mods' / mod
receipt_path = kit / 'Saved/ModKit' / f'example-{mod}.json'
receipt = json.loads(receipt_path.read_text(encoding='utf-8'))
if receipt.get('modId') != mod or not receipt.get('initialFiles') or receipt.get('kind') != 'weapons':
    raise RuntimeError('A fresh weapon workspace receipt is required')
if set(retake_sources.inventory(workspace, mod)) != set(receipt['initialFiles']):
    raise RuntimeError('The starter contains new creator files; use a fresh example workspace')
for relative, expected in receipt['initialFiles'].items():
    path = (workspace / relative).resolve()
    if not path.is_relative_to(workspace) or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        raise RuntimeError('The starter was edited; refusing to overwrite creator work')
seeds = kit / 'Examples/Weapons/SourceArt'
seed_manifest = json.loads((seeds / 'Files.json').read_text(encoding='utf-8'))
raw = workspace / 'SourceArt/Example'
raw.mkdir(parents=True)
for relative, expected in seed_manifest['files'].items():
    source, target = (seeds / relative).resolve(), (raw / relative).resolve()
    if not source.is_relative_to(seeds.resolve()) or not target.is_relative_to(raw):
        raise RuntimeError('Invalid example source path')
    if hashlib.sha256(source.read_bytes()).hexdigest() != expected:
        raise RuntimeError('Example source changed: ' + relative)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)

assets = unreal.AssetToolsHelpers.get_asset_tools()
library = unreal.EditorAssetLibrary
unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous([f'/{mod}'], True)
created = []


def asset(class_name, folder, name):
    module = 'RetakeMapSDKEditor' if class_name == 'RetakeWeaponPoseBasis' else 'RetakeMapSDK'
    cls = unreal.load_class(None, '/Script/' + module + '.' + class_name)
    if not cls:
        raise RuntimeError('Missing SDK authoring class: ' + class_name)
    factory = unreal.DataAssetFactory()
    factory.set_editor_property('data_asset_class', cls)
    result = assets.create_asset(name, f'/{mod}/{folder}', cls, factory)
    if not result:
        raise RuntimeError('Could not create ' + name)
    created.append(result)
    return result


def change(value, **properties):
    for name, field in properties.items():
        value.set_editor_property(name, field)
    return value


def tag(name):
    value = unreal.GameplayTag()
    value.import_text(f'(TagName="{name}")')
    return value


def import_file(source, folder, name, options=None):
    task = unreal.AssetImportTask()
    change(task, filename=str(source), destination_path=f'/{mod}/{folder}', destination_name=name,
           automated=True, replace_existing=False, save=True)
    if options:
        task.set_editor_property('options', options)
    assets.import_asset_tasks([task])
    result = library.load_asset(f'/{mod}/{folder}/{name}')
    if not result:
        raise RuntimeError('Import failed: ' + name)
    created.append(result)
    return result


def mesh(filename, name, skeleton=None):
    options = unreal.FbxImportUI()
    change(options, import_as_skeletal=True, import_mesh=True, import_animations=False,
           import_materials=False, import_textures=False, create_physics_asset=False)
    if skeleton:
        options.set_editor_property('skeleton', skeleton)
    return import_file(raw / filename, 'Meshes', name, options)


authoring = retake_weapon_basis.WeaponBasisAuthor(mod, raw, created).create()
body, arms, skeleton = authoring.body, authoring.arms, authoring.skeleton
contract, standing_pose = authoring.contract, authoring.standing_pose
stand, crouch, basis = authoring.stand, authoring.crouch, authoring.basis
sequence = authoring.sequence
weapon = mesh('SKM_Example_Carbine.fbx', 'SKM_Example_Carbine')
weapon_skeleton = weapon.get_editor_property('skeleton')
if not weapon_skeleton:
    raise RuntimeError('The example mesh requires its own weapon skeleton')
created.append(weapon_skeleton)

material = assets.create_asset('M_Example_Carbine', f'/{mod}/Materials', unreal.Material, unreal.MaterialFactoryNew())
color = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -200, 0)
color.set_editor_property('constant', unreal.LinearColor(.06, .32, .38, 1.0))
unreal.MaterialEditingLibrary.connect_material_property(color, '', unreal.MaterialProperty.MP_BASE_COLOR)
material.set_editor_property('used_with_skeletal_mesh', True)
unreal.MaterialEditingLibrary.recompile_material(material)
created.append(material)
slots = weapon.get_editor_property('materials')
for slot in slots:
    slot.set_editor_property('material_interface', material)
weapon.set_editor_property('materials', slots)

profile = asset('RetakeWeaponAnimationProfile', 'Definitions', 'DA_Example_Profile')
change(profile, profile_id=mod + '.example_carbine', no_handling_animations=True)
for field in ('first_person_anims', 'third_person_anims'):
    settings = change(profile.get_editor_property(field), idle_anim=stand, crouch_idle_anim=crouch, aim_idle_anim=stand)
    profile.set_editor_property(field, settings)

# A deliberately simple editable action clip. Action timing and ammunition remain native gameplay data.
reload_clip = sequence('AS_Example_Reload', 0, 45, reloading=True)
montage_factory = unreal.AnimMontageFactory()
change(montage_factory, target_skeleton=skeleton, source_animation=reload_clip)
reload_montage = assets.create_asset('AM_Example_Reload', f'/{mod}/Animations', unreal.AnimMontage, montage_factory)
if not reload_montage:
    raise RuntimeError('Could not create the example reload montage')
created.append(reload_montage)
for field in ('first_person_montages', 'third_person_montages'):
    profile.set_editor_property(field, change(profile.get_editor_property(field), reload_montage=reload_montage))


def cue(name, number, frequency, duration):
    filename = raw / f'{name}.wav'
    rate = 22050
    with wave.open(str(filename), 'wb') as stream:
        stream.setnchannels(1)
        stream.setsampwidth(2)
        stream.setframerate(rate)
        stream.writeframes(b''.join(struct.pack('<h', int(14000 * math.sin(2 * math.pi * frequency * i / rate)
                                                        * math.exp(-14 * i / rate))) for i in range(int(duration * rate))))
    recording = import_file(filename, 'Audio', 'SW_' + name)
    change(recording, looping=False, virtualization_mode=unreal.VirtualizationMode.RESTART)
    entry = unreal.RetakeAudioRecording()
    change(entry, wave=recording, source_id='OriginalExample.' + name)
    definition = asset('RetakeAudioCueDefinition', 'Audio', 'DA_' + name)
    change(definition, cue_id=number, semantic_name='Example.' + name, recordings=[entry], spatial=True, looping=False)
    return definition


fire = cue('ExampleShot', 1, 90, .25)
reload_sound = cue('ExampleReload', 2, 520, .12)
dry = cue('ExampleDryFire', 3, 900, .06)
ammo = asset('RetakeAmmoDefinition', 'Definitions', 'DA_Example_Ammo')
change(ammo, ammo_id=mod + '.example_rifle', display_name='Example rifle ammunition',
       ammo_type=tag('Ammo.Type.rifle'), reserve_capacity=120)
definition = asset('RetakeWeaponSpecification', 'Definitions', 'DA_Example_Carbine')
change(definition, weapon_id=mod + '.example_carbine', display_name='Example Carbine', category=tag('Weapon.Type.Rifle'),
       ammo=ammo, starting_ammo=90, magazine_capacity=30, source_definition='Original Retake blockout example',
       source_revision='1', provisional=True, provisional_notes='Original teaching asset; presentation acceptance is tracked with the kit release.')
action = definition.get_editor_property('primary_action')
change(action, type=unreal.RetakeWeaponAction.HITSCAN, ammo_cost=1, interval_seconds=.12)
action.set_editor_property('damage', change(action.get_editor_property('damage'), base_damage=25.0))
definition.set_editor_property('primary_action', action)
reload = change(definition.get_editor_property('reload'), policy=unreal.RetakeReloadPolicy.MAGAZINE,
                duration_seconds=1.5, commit_seconds=1.0)
definition.set_editor_property('reload', reload)
timed_audio = unreal.RetakeWeaponTimedAudioCue(seconds=.75, cue=reload_sound)
definition.set_editor_property('reload_audio', change(definition.get_editor_property('reload_audio'), authored=True, start=[timed_audio]))
presentation = change(definition.get_editor_property('presentation'), character_profile=profile,
                      world_mesh=weapon, first_person_mesh=weapon, grip_socket='hand_r', third_person_grip_socket='hand_r',
                      muzzle_socket='muzzle', primary_fire_cue=fire, dry_fire_cue=dry,
                      smoke_loop_attenuation=None)
presentation.set_editor_property('clearance', change(presentation.get_editor_property('clearance'), enabled=False))
placement = unreal.Transform()
placement.translation = unreal.Vector(*standing_pose['weaponPosition'])
placement.rotation = unreal.Quat(*standing_pose['weaponRotation'])
change(presentation, world_offset=placement, first_person_offset=placement)
definition.set_editor_property('presentation', presentation)

icon = import_file(raw / 'T_Example_Carbine.png', 'UI', 'T_Example_Carbine')
change(icon, lod_group=unreal.TextureGroup.TEXTUREGROUP_UI, never_stream=True,
       mip_gen_settings=unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
icon_material = assets.create_asset('M_Example_WeaponIcon', f'/{mod}/UI', unreal.Material, unreal.MaterialFactoryNew())
change(icon_material, material_domain=unreal.MaterialDomain.MD_UI, blend_mode=unreal.BlendMode.BLEND_TRANSLUCENT)
sample = unreal.MaterialEditingLibrary.create_material_expression(icon_material, unreal.MaterialExpressionTextureSample, -200, 0)
sample.set_editor_property('texture', icon)
unreal.MaterialEditingLibrary.connect_material_property(sample, 'RGB', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
unreal.MaterialEditingLibrary.connect_material_property(sample, 'A', unreal.MaterialProperty.MP_OPACITY)
unreal.MaterialEditingLibrary.recompile_material(icon_material)
created.append(icon_material)
definition.set_editor_property('presentation', change(definition.get_editor_property('presentation'), icon=icon, weapon_icon_material=icon_material))

# Export one profile. Pose preparation below is performed by the same native commandlet as any creator pack.
catalogue = library.load_asset(f'/{mod}/Data/DA_WeaponPack')
if not catalogue:
    raise RuntimeError('The weapon starter catalogue is missing')
entry = unreal.RetakeWeaponPackEntry(local_id='example_carbine', display_name='Example Carbine')
selection = unreal.RetakeWeaponPackProfile(profile_id='Example', display_name='Original example',
    pose_contribution=unreal.SoftObjectPath(f'/{mod}/Preparation/DA_Example_Poses.DA_Example_Poses'),
    weapons={'example_carbine': unreal.SoftObjectPath(definition.get_path_name())})
change(catalogue, display_name='Original Retake weapon example', weapons=[entry], profiles=[selection])
created.append(catalogue)
for item in created:
    if not library.save_loaded_asset(item, False):
        raise RuntimeError('Could not save ' + item.get_path_name())
for path in library.list_assets(f'/{mod}/Meshes', recursive=True, include_folder=False):
    if not library.save_asset(path):
        raise RuntimeError('Could not save imported resource ' + path)
request = {'owner': mod, 'basis': basis.get_path_name(),
           'output': f'/{mod}/Preparation/DA_Example_Poses.DA_Example_Poses', 'specifications': [definition.get_path_name()],
           'expectedBodyIdentity': contract['bodyIdentity']}
(raw / 'Preparation.json').write_text(json.dumps(request, indent=2) + '\n', encoding='utf-8')
selected = sorted(p.relative_to(workspace).as_posix() for p in raw.rglob('*') if p.is_file() and p.suffix.lower() != '.py')
(workspace / 'SourceFiles.json').write_text(json.dumps({'format': 1, 'files': selected}, indent=2) + '\n', encoding='utf-8')
receipt_path.replace(receipt_path.with_suffix('.completed.json'))
print('event=WeaponExampleAuthored owner=' + mod)
