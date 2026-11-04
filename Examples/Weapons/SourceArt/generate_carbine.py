"""Original blockout carbine. Run with Blender --background --factory-startup --python this_file.

Copyright 2026 Codnyx. Retake Mod Kit License. No external models, textures or animations.
The generated FBX is delivered so modders do not need Blender to open the example.
"""
from pathlib import Path
import math
import bpy

output = Path(__file__).resolve().parent
if (output / 'SKM_Example_Carbine.fbx').exists():
    raise RuntimeError('Refusing to overwrite the editable reference FBX; choose a new output before regeneration.')
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
bpy.context.scene.unit_settings.system = 'METRIC'
bpy.context.scene.unit_settings.scale_length = 1.0
parts = []


def box(name, location, size, group='weapon_root'):
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=location)
    obj = bpy.context.object
    obj.name = name
    obj.dimensions = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    weights = obj.vertex_groups.new(name=group)
    weights.add(list(range(len(obj.data.vertices))), 1.0, 'REPLACE')
    parts.append(obj)
    return obj


box('Receiver', (0.15, 0, 0.015), (0.30, 0.055, 0.07))
box('Foregrip', (0.34, 0, 0.01), (0.16, 0.047, 0.053))
box('Stock', (-0.13, 0, 0), (0.25, 0.05, 0.08))
box('StockPad', (-0.263, 0, 0), (0.016, 0.06, 0.105))
box('Grip', (0.035, 0, -0.065), (0.045, 0.04, 0.10))
box('Magazine', (0.155, 0, -0.075), (0.065, 0.037, 0.12), 'magazine')
box('Barrel', (0.455, 0, 0.022), (0.15, 0.018, 0.018))
box('FrontSight', (0.40, 0, 0.055), (0.02, 0.012, 0.035))
box('RearSight', (0.04, 0, 0.06), (0.018, 0.03, 0.025))
box('Bolt', (0.15, -0.034, 0.02), (0.07, 0.018, 0.014), 'bolt')

bpy.ops.object.select_all(action='DESELECT')
for obj in parts:
    obj.select_set(True)
bpy.context.view_layer.objects.active = parts[0]
bpy.ops.object.join()
mesh = bpy.context.object
mesh.name = 'SKM_Example_Carbine'
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
material = bpy.data.materials.new('M_Example_Carbine')
material.diffuse_color = (0.06, 0.32, 0.38, 1.0)
mesh.data.materials.append(material)

armature = bpy.data.armatures.new('SK_Example_Carbine')
rig = bpy.data.objects.new('ExampleCarbineRig', armature)
bpy.context.collection.objects.link(rig)
bpy.context.view_layer.objects.active = rig
rig.select_set(True)
mesh.select_set(False)
bpy.ops.object.mode_set(mode='EDIT')
root = armature.edit_bones.new('weapon_root')
root.head = (0, 0, 0)
root.tail = (0.1, 0, 0)
for name, head in [('magazine', (0.155, 0, -0.075)), ('bolt', (0.15, -0.034, 0.02)),
                   ('muzzle', (0.535, 0, 0.022)), ('shell', (0.18, -0.035, 0.035)),
                   ('support', (0.32, 0, -0.02))]:
    bone = armature.edit_bones.new(name)
    bone.head = head
    bone.tail = (head[0] + 0.025, head[1], head[2])
    bone.parent = root
bpy.ops.object.mode_set(mode='OBJECT')
modifier = mesh.modifiers.new('Armature', 'ARMATURE')
modifier.object = rig
mesh.parent = rig
bpy.ops.object.select_all(action='DESELECT')
mesh.select_set(True)
rig.select_set(True)
bpy.context.view_layer.objects.active = rig
bpy.ops.export_scene.fbx(filepath=str(output / 'SKM_Example_Carbine.fbx'), use_selection=True,
                         object_types={'ARMATURE', 'MESH'}, add_leaf_bones=False, bake_anim=False,
                         axis_forward='-Y', axis_up='Z', apply_unit_scale=True, mesh_smooth_type='FACE')
bpy.ops.wm.save_as_mainfile(filepath=str(output / 'ExampleCarbine.blend'))
print('Original carbine source written to ' + str(output))
