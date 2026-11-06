"""Shared editor authoring basis for empty weapon workspaces and the original example.

Executed inside Unreal. Inputs are the published template-derived reference files;
no private Retake game assets, implementation or example weapon are required.
"""
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import sys
import unreal

sys.path.insert(0, str(Path(__file__).resolve().parent))
import retake_body_contract
import retake_example_pose
import retake_sources

REFERENCE_FILES = ('BodyContract.json', 'SharedPoseBasis.json', 'SKM_AuthoringBody.fbx', 'SKM_AuthoringArms.fbx')


def change(value, **properties):
    for name, field in properties.items():
        value.set_editor_property(name, field)
    return value


class WeaponBasisAuthor:
    def __init__(self, mod, raw, created):
        self.mod, self.raw, self.created = mod, Path(raw), created
        self.assets = unreal.AssetToolsHelpers.get_asset_tools()
        self.contract = json.loads((self.raw / 'BodyContract.json').read_text(encoding='utf-8'))
        self.standing_pose = retake_example_pose.pose(self.contract['body'], 0)
        self.skeleton = None

    def mesh(self, filename, name, skeleton=None):
        options = change(unreal.FbxImportUI(), import_as_skeletal=True, import_mesh=True, import_animations=False,
                         import_materials=False, import_textures=False, create_physics_asset=False)
        if skeleton:
            options.set_editor_property('skeleton', skeleton)
        task = change(unreal.AssetImportTask(), filename=str(self.raw / filename), destination_path=f'/{self.mod}/Meshes',
                      destination_name=name, automated=True, replace_existing=False, save=True, options=options)
        self.assets.import_asset_tasks([task])
        result = unreal.EditorAssetLibrary.load_asset(f'/{self.mod}/Meshes/{name}')
        if not result:
            raise RuntimeError('Reference mesh import failed: ' + name)
        self.created.append(result)
        return result

    def sequence(self, name, posture, frames=30, reloading=False, reference=None):
        factory = change(unreal.AnimSequenceFactory(), target_skeleton=self.skeleton)
        result = self.assets.create_asset(name, f'/{self.mod}/Animations', unreal.AnimSequence, factory)
        if not result:
            raise RuntimeError('Could not create ' + name)
        controller = result.get_editor_property('controller')
        controller.open_bracket('Create Retake authoring reference pose')
        controller.set_frame_rate(unreal.FrameRate(30, 1))
        controller.set_number_of_frames(unreal.FrameNumber(frames))
        poses = [reference or retake_example_pose.pose(self.contract['body'], posture,
                 math.sin(math.pi * frame / frames) ** 2 if reloading else 0.0,
                 self.standing_pose['rightHandRotation']) for frame in range(frames + 1)]
        for index, bone in enumerate(self.contract['body']['animationBones']):
            if not controller.add_bone_curve(bone['name']) or not controller.set_bone_track_keys(bone['name'],
                    [unreal.Vector(*p['positions'][index]) for p in poses],
                    [unreal.Quat(*p['rotations'][index]) for p in poses],
                    [unreal.Vector(*p['scales'][index]) for p in poses]):
                raise RuntimeError('Could not author reference bone ' + bone['name'])
        controller.close_bracket()
        self.created.append(result)
        return result

    def create(self):
        self.body = self.mesh('SKM_AuthoringBody.fbx', 'SKM_AuthoringBody')
        self.skeleton = self.body.get_editor_property('skeleton')
        if not self.skeleton:
            raise RuntimeError('The reference mesh has no skeleton')
        self.created.append(self.skeleton)
        self.arms = self.mesh('SKM_AuthoringArms.fbx', 'SKM_AuthoringArms', self.skeleton)
        material = self.assets.create_asset('M_AuthoringReference', f'/{self.mod}/Authoring', unreal.Material, unreal.MaterialFactoryNew())
        if not material:
            raise RuntimeError('Could not create the reference material')
        change(material, used_with_skeletal_mesh=True)
        unreal.MaterialEditingLibrary.recompile_material(material)
        self.created.append(material)
        for model in (self.body, self.arms):
            slots = model.get_editor_property('materials')
            for slot in slots:
                slot.set_editor_property('material_interface', material)
            model.set_editor_property('materials', slots)
        self.stand = self.sequence('AS_Authoring_Stand', 0)
        self.crouch = self.sequence('AS_Authoring_Crouch', 1)
        cls = unreal.load_class(None, '/Script/RetakeMapSDKEditor.RetakeWeaponPoseBasis')
        factory = change(unreal.DataAssetFactory(), data_asset_class=cls)
        self.basis = self.assets.create_asset('DA_WeaponAuthoringBasis', f'/{self.mod}/Authoring', cls, factory)
        if not self.basis:
            raise RuntimeError('Could not create the owned weapon authoring basis')
        self.created.append(self.basis)
        value = self.basis.get_editor_property('body').copy()
        value.import_text(retake_body_contract.import_text(self.contract['body']))
        change(self.basis, Body=value, ReferenceMesh=self.body, StandLower=self.stand, CrouchLower=self.crouch, SupportSocket='FABRIK_Socket')
        if unreal.RetakeMapSDKEditorLibrary.get_weapon_authoring_body_identity(self.basis) != self.contract['bodyIdentity']:
            raise RuntimeError('The imported body contract lost precision; preparation is refused')
        error = unreal.RetakeMapSDKEditorLibrary.normalize_weapon_authoring_reference(self.basis, self.mod)
        if error:
            raise RuntimeError(error)
        shared = json.loads((self.raw / 'SharedPoseBasis.json').read_text(encoding='utf-8'))
        bones = [b['name'] for b in self.contract['body']['animationBones']]
        if shared['sourceRigIdentity'] != self.contract['sourceRigIdentity'] or shared['bones'] != bones:
            raise RuntimeError('Shared procedural poses do not match the authoring body')
        neutral = self.sequence('AS_Authoring_NeutralReference', 0, frames=1,
                                reference=retake_example_pose.reference_pose(self.contract['body']))
        fields = {'stand_lean':'StandLean', 'crouch_lean':'CrouchLean', 'stand_moving_lean':'StandMovingLean',
                  'crouch_moving_lean':'CrouchMovingLean', 'clearance_stand':'ClearanceStand', 'clearance_crouch':'ClearanceCrouch'}
        for field, rows in shared['spaces'].items():
            factory = change(unreal.AimOffsetBlendSpaceFactory1D(), target_skeleton=self.skeleton)
            space = self.assets.create_asset('AO_Authoring_' + field, f'/{self.mod}/Authoring/Poses', unreal.AimOffsetBlendSpace1D, factory)
            if not space:
                raise RuntimeError('Could not create ' + field)
            parameters = list(space.get_editor_property('blend_parameters'))
            parameters[0] = change(parameters[0], display_name='Posture', min=min(r['axis'] for r in rows),
                                   max=max(r['axis'] for r in rows), grid_num=10)
            space.set_editor_property('blend_parameters', parameters)
            samples = []
            for index, row in enumerate(rows):
                if 'AAT_ROTATION_OFFSET_MESH_SPACE' not in row['additiveType']:
                    raise RuntimeError('Unsupported shared procedural pose type')
                clip = self.sequence('AS_Authoring_' + field + '_' + str(index), 0, frames=1,
                                     reference=retake_example_pose.additive_reference_pose(self.contract['body'], row['transforms']))
                change(clip, additive_anim_type=unreal.AdditiveAnimationType.AAT_ROTATION_OFFSET_MESH_SPACE,
                       ref_pose_type=unreal.AdditiveBasePoseType.ABPT_ANIM_FRAME, ref_pose_seq=neutral, ref_frame_index=0)
                samples.append(change(unreal.BlendSample(), animation=clip, sample_value=unreal.Vector(row['axis'], 0, 0), rate_scale=1.0))
            space.set_editor_property('sample_data', samples)
            self.created.append(space)
            self.basis.set_editor_property(fields[field], space)
        self.basis.set_editor_property('LeanStrength', shared['leanStrength'])
        return self

    def save(self):
        for asset in self.created:
            if not unreal.EditorAssetLibrary.save_loaded_asset(asset, False):
                raise RuntimeError('Could not save ' + asset.get_path_name())


def main():
    match = re.search(r'(?:^|\s)-RetakeBasisMod=(RetakeMod_[A-Za-z0-9_]{1,38})(?:\s|$)', unreal.SystemLibrary.get_command_line())
    if not match:
        raise RuntimeError('Select a weapon workspace with -RetakeBasisMod')
    mod = match.group(1)
    kit = Path(unreal.Paths.project_dir()).resolve()
    workspace = kit / 'Mods' / mod
    descriptor = json.loads((workspace / (mod + '.uplugin')).read_text(encoding='utf-8'))
    if descriptor.get('RetakeModKind') != 'weapons':
        raise RuntimeError('The animation basis belongs in a weapon workspace')
    raw = workspace / 'SourceArt/WeaponAuthoring'
    if raw.exists() or unreal.EditorAssetLibrary.does_asset_exist(f'/{mod}/Authoring/DA_WeaponAuthoringBasis'):
        raise RuntimeError('This workspace already has authoring inputs; reuse them instead of overwriting')
    seeds = kit / 'Examples/Weapons/SourceArt'
    manifest = json.loads((seeds / 'Files.json').read_text(encoding='utf-8'))['files']
    for name in REFERENCE_FILES:
        if hashlib.sha256((seeds / name).read_bytes()).hexdigest() != manifest.get(name):
            raise RuntimeError('Published reference input changed: ' + name)
    raw.mkdir(parents=True)
    for name in REFERENCE_FILES:
        shutil.copyfile(seeds / name, raw / name)
    author = WeaponBasisAuthor(mod, raw, []).create()
    author.save()
    (raw / 'Basis.json').write_text(json.dumps({'owner': mod, 'basis': author.basis.get_path_name(),
                                               'bodyIdentity': author.contract['bodyIdentity']}, indent=2) + '\n')
    source_list = workspace / retake_sources.MANIFEST
    previous = retake_sources.declared_art(json.loads(source_list.read_text())) if source_list.exists() else []
    selected = sorted(set(previous) | {p.relative_to(workspace).as_posix() for p in raw.iterdir() if p.is_file()})
    source_list.write_text(json.dumps({'format': 1, 'files': selected}, indent=2) + '\n')
    print('event=WeaponBasisCreated ' + json.dumps({'modId': mod, 'basis': author.basis.get_path_name()}))


if __name__ == '__main__':
    main()
