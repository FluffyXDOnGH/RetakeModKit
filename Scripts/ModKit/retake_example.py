"""Author one original SDK demonstration level through normal Unreal editor APIs.

Invoked by the creator backend immediately after creating a new Basic workspace. It contains no
private gameplay code, converted-map content or migration steps. Existing authored
levels are refused rather than overwritten.
"""

import unreal
# Commandlets have no editor tick to finish asynchronous engine/plugin asset discovery.
unreal.AssetRegistryHelpers.get_asset_registry().search_all_assets(True)

def ledge(owner, name, start, end, up):
    """A straight ledge from start to end, in unscaled cm from the owner's origin, whose up vector points out of the face."""
    spline = unreal.RetakeMapSDKEditorLibrary.add_instance_component(owner, unreal.SplineComponent.static_class(), name, None)
    spline.set_absolute(False, False, True)  # Ledge widths are spline lengths: keep the owner's scale out of them.
    spline.clear_spline_points(False)
    for point in (start, end):
        spline.add_spline_point(unreal.Vector(*point), unreal.SplineCoordinateSpace.LOCAL, False)
    for index in (0, 1):
        spline.set_spline_point_type(index, unreal.SplinePointType.LINEAR, False)
    spline.set_default_up_vector(unreal.Vector(*up), unreal.SplineCoordinateSpace.LOCAL)
    spline.update_spline()
    return spline

def traversal(owner, **props):
    component = unreal.RetakeMapSDKEditorLibrary.add_instance_component(owner, unreal.RetakeTraversalObstacleComponent.static_class(), "Traversal", None)
    for name, value in props.items():
        component.set_editor_property(name, value)
    return component

def ledge_access(access, profile_id):
    settings = unreal.RetakeLedgeAccessSettings()
    settings.set_editor_property("access", access)
    settings.set_editor_property("boost_profile_id", profile_id)
    return settings

def author_acoustics(world):
    """Example authoring only: Empty and Basic creator templates remain uncluttered."""
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    existing = subsystem.get_all_level_actors()
    acoustic = next((a for a in existing if isinstance(a, unreal.RetakeAcousticMap)), None)
    if acoustic:
        acoustic.call_method("Bake")
        library.save_loaded_asset(acoustic.get_editor_property("data"))
        return
    acoustic = subsystem.spawn_actor_from_class(unreal.RetakeAcousticMap, unreal.Vector(0, 0, 0))
    door = next((a for a in existing if isinstance(a, unreal.RetakeDoor)), None)
    split = door.get_actor_location().x if door else 0
    regions = []
    for name, offset in (("Exterior", -10000), ("Interior", 10000)):
        region = subsystem.spawn_actor_from_class(unreal.RetakeAcousticRegion, unreal.Vector(split + offset, 0, 500))
        region.set_editor_property("region_id", name)
        region.get_editor_property("bounds").set_box_extent(unreal.Vector(10000, 10000, 5000), False)
        regions.append(region)
    connection = subsystem.spawn_actor_from_class(unreal.RetakeAcousticConnection,
        door.get_actor_location() + unreal.Vector(0, 0, 100) if door else unreal.Vector(0, 0, 100))
    connection.set_editor_property("connection_id", "Gate")
    connection.set_editor_property("region_a", regions[0])
    connection.set_editor_property("region_b", regions[1])
    if door:
        connection.set_editor_property("door", door)
        connection.set_editor_property("dynamic", True)
    blocker = subsystem.spawn_actor_from_class(unreal.RetakeAcousticOccluder, unreal.Vector(1500, 1500, 200))
    blocker.get_editor_property("bounds").set_box_extent(unreal.Vector(100, 400, 200), False)
    data = library.load_asset(f"/{mod}/Audio/DA_Acoustics_Main")
    if not data:
        raise RuntimeError("The Basic template's acoustic asset is missing")
    acoustic.set_editor_property("data", data)
    acoustic.call_method("Bake")
    library.save_loaded_asset(data)
    print("event=ModKitAcousticsAuthored")


import math
import struct
import wave
import zlib
from pathlib import Path
import unreal
import re, json, hashlib
match = re.search(r"(?:^|\s)-RetakeExampleMod=(RetakeMod_[A-Za-z0-9_]+)(?:\s|$)", unreal.SystemLibrary.get_command_line())
if not match:
    raise RuntimeError("Select the new example workspace with -RetakeExampleMod")
mod = match.group(1)
kit = Path(unreal.Paths.project_dir()).resolve()
mod_directory = kit / "Mods" / mod
if not (kit / "RetakeModKit.uproject").is_file():
    raise RuntimeError("Use the standalone RetakeModKit project")
request_file = kit / "Saved" / "ModKit" / f"example-{mod}.json"
request = json.loads(request_file.read_text(encoding="utf-8"))
if request.get("modId") != mod or not request.get("initialFiles"):
    raise RuntimeError("The example needs a fresh creator-owned workspace receipt")
for relative, expected in request["initialFiles"].items():
    source = (mod_directory / relative).resolve()
    if not source.is_relative_to(mod_directory.resolve()) or hashlib.sha256(source.read_bytes()).hexdigest() != expected:
        raise RuntimeError("The starter changed after creation; do not overwrite creator edits")
assets = unreal.AssetToolsHelpers.get_asset_tools()
library = unreal.EditorAssetLibrary
map_path = f"/{mod}/Maps/Main"
world = unreal.EditorLoadingAndSavingUtils.load_map(map_path)
if not world:
    raise RuntimeError("Create a Basic mod workspace before authoring its example")
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
# The backend just created and hash-verified this starter. Rebuild its layout as the one example map,
# retaining its original block, physical material and acoustic-data assets. An authored workspace is never accepted.
starter_actors = list(actors.get_all_level_actors())
if len(starter_actors) != 17:
    raise RuntimeError("Unexpected starter contents; refusing to replace them")
for actor in starter_actors:
    if not actors.destroy_actor(actor):
        raise RuntimeError("Could not replace a generated starter actor")
material_path = f"/{mod}/Materials/M_ExampleSignal"
material = assets.create_asset("M_ExampleSignal", f"/{mod}/Materials", unreal.Material, unreal.MaterialFactoryNew())
color = unreal.MaterialEditingLibrary.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -400, 0)
color.set_editor_property("constant", unreal.LinearColor(1.0, 0.35, 0.05, 1.0))
unreal.MaterialEditingLibrary.connect_material_property(color, "", unreal.MaterialProperty.MP_BASE_COLOR)
# Engine basic shapes are Nanite meshes in UE6, ladder rungs are instanced and destructibles are geometry collections;
# a cooked game cannot add usage flags at runtime.
material.set_editor_property("used_with_nanite", True)
material.set_editor_property("used_with_instanced_static_meshes", True)
material.set_editor_property("used_with_geometry_collections", True)
unreal.MaterialEditingLibrary.recompile_material(material)
library.save_asset(material_path)


# One example level, with authored requirements for FFA, TDM, OBJ, CTF, DOM and FT.
unreal.GameplayStatics.get_all_actors_of_class(world, unreal.WorldSettings)[0].set_editor_property("kill_z", -2000.0)
cube = library.load_asset(f"/{mod}/Meshes/SM_BasicBlock")
if not cube:
    raise RuntimeError("Could not create the example block")
nanite = cube.get_editor_property("nanite_settings")
nanite.set_editor_property("enabled", False)
cube.set_editor_property("nanite_settings", nanite)
cube.set_editor_property("allow_cpu_access", True)
cube.set_material(0, material)
library.save_loaded_asset(cube)
placed = []

def place(cls, x, y, z=0, yaw=0, **props):
    actor = actors.spawn_actor_from_class(cls, unreal.Vector(x, y, z), unreal.Rotator(roll=0, pitch=0, yaw=yaw))
    for name, value in props.items():
        actor.set_editor_property(name, value)
    placed.append(actor)
    return actor

def block(x, y, z, sx, sy, sz):
    """Engine cube sized in metres, centred on (x, y, z) cm."""
    actor = actors.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(x, y, z))
    actor.static_mesh_component.set_static_mesh(cube)
    actor.set_actor_scale3d(unreal.Vector(sx, sy, sz))
    return actor

block(0, 0, -50, 80, 50, 1)
sun = actors.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 1000), unreal.Rotator(roll=0, pitch=-40, yaw=30))
sun.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
sun.light_component.set_editor_property("atmosphere_sun_light", True)
actors.spawn_actor_from_class(unreal.SkyAtmosphere, unreal.Vector(0, 0, 0))
sky = actors.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0, 0, 500))
sky.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
sky.light_component.set_editor_property("real_time_capture", True)
Role = unreal.RetakeModeStartRole
# Starts: 8 Shared (FFA, TDM, CTF, DOM, Freeze Tag shared thaw), 5 Team per end (FT), 5 Attacker and 5 Defender (OBJ).
for i in range(8):
    angle = math.radians(45 * i)
    place(unreal.RetakeModePlayerStart, 1200 * math.cos(angle), 1200 * math.sin(angle), 100, yaw=45 * i + 180, start_role=Role.SHARED)
for y in range(-800, 801, 400):
    place(unreal.RetakeModePlayerStart, -3400, y, 100, yaw=0, start_role=Role.TEAM, team_id=0)
    place(unreal.RetakeModePlayerStart, 3400, y, 100, yaw=180, start_role=Role.TEAM, team_id=1)
    place(unreal.RetakeModePlayerStart, -2800, y, 100, yaw=0, start_role=Role.ATTACKER)
    place(unreal.RetakeModePlayerStart, 2800, y, 100, yaw=180, start_role=Role.DEFENDER)
place(unreal.RetakeMapSettings, 0, 0, 500, defending_faction=unreal.RetakeMapDefendingFaction.SECURITY)
# OBJ: site A destroys its Field Uplink; site B triggers a detonation target.
target = place(unreal.RetakeDetonationTarget, 2200, 2000)
target.get_editor_property("intact_mesh").set_static_mesh(cube)
target.get_editor_property("intact_mesh").set_material(0, material)
wreck = target.get_editor_property("destroyed_mesh")
wreck.set_static_mesh(cube)
wreck.set_relative_scale3d(unreal.Vector(2, 2, 0.3))
wreck.set_relative_location(unreal.Vector(0, 0, 15), False, False)
wreck.set_material(0, material)
place(unreal.RetakeFieldUplinkBombSite, 1800, -1200, objective_id="A")
place(unreal.RetakeFieldUplinkBombSite, 1800, 1200, objective_id="B",
      detonation_mode=unreal.RetakeUplinkDetonationMode.TRIGGER_TARGET, detonation_target=target)
Kind = unreal.RetakeModeMarkerKind
# CTF: a flag home and a capture zone per team. DOM: one point on the ground, one on the platform.
place(unreal.RetakeModeObjectiveMarker, -3000, -1600, kind=Kind.TEAM_FLAG_HOME, objective_id="FlagA", team_index=0)
place(unreal.RetakeModeObjectiveMarker, 3000, -1600, kind=Kind.TEAM_FLAG_HOME, objective_id="FlagB", team_index=1)
place(unreal.RetakeModeObjectiveMarker, -3000, 1600, kind=Kind.CAPTURE_ZONE, objective_id="CaptureA", team_index=0)
place(unreal.RetakeModeObjectiveMarker, 3000, 1600, kind=Kind.CAPTURE_ZONE, objective_id="CaptureB", team_index=1)
place(unreal.RetakeModeObjectiveMarker, 0, -1800, kind=Kind.CONTROL_POINT, objective_id="Alpha")
place(unreal.RetakeModeObjectiveMarker, -1400, 1800, 300, kind=Kind.CONTROL_POINT, objective_id="Bravo")
place(unreal.RetakeFreezeTagRelay, 0, 0, relay_id="Central", rotation_order=0)
# A danger-zone example in a corner away from every start and objective zone.
# A door in a 20 cm wall at x=-1500 with a 104 cm gap and a lintel above 210 cm.
block(-1500, -526, 150, 0.2, 9.48, 3)
block(-1500, 526, 150, 0.2, 9.48, 3)
block(-1500, 0, 255, 0.2, 1.04, 0.9)
door = place(unreal.RetakeDoor, -1500, -50, yaw=90)
leaf = door.get_editor_property("door_mesh")
leaf.set_static_mesh(cube)
leaf.set_material(0, material)
door.set_editor_property("door_size", unreal.Vector(100, 8, 210))
door.set_editor_property("hinge_edge", unreal.RetakeDoorHinge.NEGATIVE_X)
# A 300 cm platform on four pillars, climbed by a ladder whose +X faces the climber (yaw -90: the climber stands at -Y).
block(-1400, 1800, 290, 4, 4, 0.2)
for px, py in ((-1580, 1620), (-1220, 1620), (-1580, 1980), (-1220, 1980)):
    block(px, py, 140, 0.3, 0.3, 2.8)
ladder = place(unreal.LadderActor, -1400, 1575, yaw=-90)
geometry = ladder.get_editor_property("geometry")
geometry.set_editor_property("type", unreal.LadderGeometry.GENERATED)
geometry.set_editor_property("height", 300.0)
geometry.set_editor_property("rung_mesh", cube)
geometry.set_editor_property("left_rail_mesh", cube)
geometry.set_editor_property("rung_mesh_offset", unreal.Transform(scale=unreal.Vector(0.04, 1, 0.04)))
geometry.set_editor_property("left_rail_offset", unreal.Transform(scale=unreal.Vector(0.06, 0.06, 1)))
geometry.set_editor_property("right_rail_offset", unreal.Transform(scale=unreal.Vector(0.06, 0.06, 1)))
ladder.set_editor_property("geometry", geometry)
for component in ("rungs", "left_rail", "right_rail"):
    ladder.get_editor_property(component).set_material(0, material)
# A destructible (contract 5): a 3 m cube wall fractured by the editor converter into the mod's own Destruction folder, with a
# mod Niagara break effect. The converter writes under the level's content root, so the level is saved first.
wall = block(600, 2200, 150, 3, 0.2, 3)
wall.static_mesh_component.set_material(0, material)
author_acoustics(world)
if not unreal.EditorLoadingAndSavingUtils.save_map(world, map_path):
    raise RuntimeError("could not save " + map_path)
options = unreal.RetakeDestructionConversionOptions()
options.set_editor_property("asset_name", "ExampleWall")
options.set_editor_property("interior_material", material)
options.set_editor_property("open_fracture_editor", False)
recipe = options.get_editor_property("fracture")
recipe.set_editor_property("fragment_count", 8)
options.set_editor_property("fracture", recipe)
destructible, message = unreal.RetakeDestructionConversion.convert_actor(wall, options)
if not destructible:
    raise RuntimeError("example wall not converted: " + message)
placed.append(destructible)
definition = destructible.get_editor_property("definition")
effect_path = f"/{mod}/Destruction/NS_ExampleBreak"
effect = library.duplicate_asset("/Niagara/DefaultAssets/Templates/Systems/RadialBurst", effect_path)
library.save_asset(effect_path)
definition.set_editor_property("break_effect", effect)
library.save_loaded_asset(definition)
summary = definition.get_preparation_summary()
if not definition.get_path_name().startswith(f"/{mod}/Destruction/") or "current=1" not in summary:
    raise RuntimeError(f"example destructible not prepared in the mod: {definition.get_path_name()} {summary}")
print(f"event=ModKitExampleDestructible class={destructible.get_class().get_path_name()} definition={definition.get_path_name()} "
      f"effect={effect.get_path_name()} {summary}")
# Exercise the optional linked leaf in the internal fixture; its acoustic connection already references this door.
options.set_editor_property("asset_name", "ExampleDoorLeaf")
door_leaf, door_message = unreal.RetakeDestructionConversion.convert_door_leaf(door, options)
if not door_leaf or "Warning:" in door_message or door_leaf.get_editor_property("door") != door:
    raise RuntimeError("example door leaf conversion failed: " + door_message)
door_definition = door_leaf.get_editor_property("definition")
door_definition.set_editor_property("break_effect", effect)
library.save_loaded_asset(door_definition)
if "current=1" not in door_definition.get_preparation_summary() or not door_definition.get_editor_property("anchor_boxes"):
    raise RuntimeError("example door leaf lacks its current hinge preparation")
placed.append(door_leaf)
print(f"event=ModKitExampleDoorLeaf door={door.get_path_name()} leaf={door_leaf.get_path_name()} "
      f"collapse={door_leaf.get_editor_property('door_collapse_fraction')} edges={door_leaf.get_editor_property('door_edge_cm')}")
# Weapon pickups, traversal and the spectator overview (contract 5). Pickups name built-in weapons by ID; ledges are splines on
# placed engine cubes with the SDK traversal component, added the way a creator does with Add Component.
pickups = [place(unreal.RetakeWeaponPickup, 300, -600, 20, weapon_id="m4a1", magazine_rounds=30, spare_rounds=90, always_available=True),
           place(unreal.RetakeWeaponPickup, 450, -600, 20, weapon_id="flash_grenade", quantity=2, always_available=True)]
# A 1 m crate to vault over from either side: two opposite ledges and the game's Traversable collision preset.
crate = block(-600, -1200, 50, 1, 1, 1)
crate.static_mesh_component.set_material(0, material)
crate.static_mesh_component.set_collision_profile_name("TraversalObjectPreset")
ledge(crate, "Front", (-50, -50, 50), (-50, 50, 50), (-1, 0, 0))
ledge(crate, "Back", (50, -50, 50), (50, 50, 50), (1, 0, 0))
crate_traversal = traversal(crate, opposite_ledges={"Front": "Back", "Back": "Front"})
# A 3 m boost wall, 2 m deep so the climber lands on top: two players boost up its 3 m front edge.
boost_wall = block(-2200, -1700, 150, 3, 2, 3)
boost_wall.static_mesh_component.set_material(0, material)
ledge(boost_wall, "BoostEdge", (-150, -100, 150), (150, -100, 150), (0, -1, 0))
wall_traversal = traversal(boost_wall, minimum_width=150.0,
                           ledge_access={"BoostEdge": ledge_access(unreal.RetakeLedgeAccess.BOOST_ONLY, "Standard")})
overview = place(unreal.RetakeSpectatorOverview, -3600, -2300, 1800)
overview.set_actor_rotation(unreal.Rotator(roll=0, pitch=-23, yaw=33), False)
for component in (crate_traversal, wall_traversal):
    if not component.get_class().get_path_name().startswith("/Script/RetakeMapSDK."):
        raise RuntimeError(f"non-SDK traversal component: {component.get_class().get_path_name()}")
for element in pickups + [crate_traversal, wall_traversal]:
    if str(element.get_configuration_error()):
        raise RuntimeError(f"{element.get_path_name()} is misconfigured: {element.get_configuration_error()}")
print(f"event=ModKitExamplePlacements pickups={len(pickups)} traversal={crate.get_actor_label()},{boost_wall.get_actor_label()} "
      f"overview={overview.get_class().get_path_name()}")
# Every element must be an SDK or destruction class, and SDK code must have run in the kit (ladder build, door sizing).
foreign = [a.get_class().get_path_name() for a in placed if not a.get_class().get_path_name().startswith(("/Script/RetakeMapSDK.", "/Script/RetakeDestruction."))]
if foreign:
    raise RuntimeError(f"non-SDK elements in the validation map: {foreign}")
error = str(ladder.get_configuration_error())
rungs = ladder.get_editor_property("rungs").get_instance_count()
if error or rungs != len(ladder.get_rung_heights()) or rungs < 2:
    raise RuntimeError(f"validation ladder not built by the SDK: error={error!r} rungs={rungs}")
if not door.get_editor_property("door_mesh").get_editor_property("relative_scale3d").is_near_equal(unreal.Vector(1, 0.08, 2.1), 0.001):
    raise RuntimeError("validation door leaf not sized by the SDK")

# Presentation (manifest schema 2): generated audio, ambience profiles and an interior zone, a placed emitter, mod surfaces,
# loading and preview art, the map settings block and a minimap capture (modkit.py minimap bakes it after this script).
generated = mod_directory / "SourceArt" / "Example"
generated.mkdir(parents=True, exist_ok=True)

def import_file(source, folder, name):
    task = unreal.AssetImportTask()
    task.filename = str(source)
    task.destination_path = folder
    task.destination_name = name
    task.automated = True
    task.replace_existing = True
    task.save = False
    assets.import_asset_tasks([task])
    if not task.imported_object_paths:
        raise RuntimeError(f"could not import {source}")
    return unreal.load_asset(f"{folder}/{name}")

def wav(name, seconds, sample, looping):
    """Mono 16-bit 22.05 kHz. Looping sounds hold whole cycles, so the loop point is seamless."""
    rate, path = 22050, generated / f"{name}.wav"
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(b"".join(struct.pack("<h", int(12000 * sample(i / rate))) for i in range(int(rate * seconds))))
    sound = import_file(path, f"/{mod}/Audio", name)
    sound.set_editor_property("looping", looping)
    # Validation refuses Play When Silent: silent voices would hold mixer voices.
    sound.set_editor_property("virtualization_mode", unreal.VirtualizationMode.RESTART)
    library.save_asset(sound.get_path_name())
    return sound

tau = 2 * math.pi
bed = wav("SW_ExampleBed", 2.0, lambda t: .5 * math.sin(tau * 110 * t) + .3 * math.sin(tau * 165 * t), True)
room = wav("SW_ExampleRoom", 2.0, lambda t: .6 * math.sin(tau * 220 * t), True)
knock = wav("SW_ExampleKnock", .3, lambda t: math.sin(tau * 600 * t) * math.exp(-12 * t), False)
hum = wav("SW_ExampleHum", 2.0, lambda t: .7 * math.sin(tau * 440 * t) * (.6 + .4 * math.sin(tau * 2 * t)), True)
attenuation = assets.create_asset("ATT_ExampleAmbience", f"/{mod}/Audio", unreal.SoundAttenuation, unreal.SoundAttenuationFactory())
falloff = attenuation.get_editor_property("attenuation")
falloff.set_editor_property("falloff_distance", 3000.0)
attenuation.set_editor_property("attenuation", falloff)
library.save_asset(attenuation.get_path_name())

def ambience_profile(name, beds, elements):
    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", unreal.RetakeAmbienceProfile)
    profile = assets.create_asset(name, f"/{mod}/Audio", unreal.RetakeAmbienceProfile, factory)
    profile.set_editor_property("beds", [unreal.RetakeAmbienceBed(sound=sound, volume=.8) for sound in beds])
    profile.set_editor_property("elements", elements)
    profile.set_editor_property("fade_seconds", 1.0)
    library.save_asset(profile.get_path_name())
    return profile

element = unreal.RetakeAmbienceElement()
element.set_editor_property("sounds", [knock])
element.set_editor_property("interval", unreal.FloatInterval(min=4.0, max=8.0))
element.set_editor_property("distance", unreal.FloatInterval(min=800.0, max=2000.0))
element.set_editor_property("height", unreal.FloatInterval(min=0.0, max=300.0))
element.set_editor_property("attenuation", attenuation)
exterior = ambience_profile("DA_ExampleAmbience_Exterior", [bed], [element])
interior = ambience_profile("DA_ExampleAmbience_Interior", [room], [])

# An interior: an Audio Gameplay Volume with the SDK ambience zone and the stock filter, added as Details > Add Component does.
interior_volume = actors.spawn_actor_from_class(unreal.AudioGameplayVolume, unreal.Vector(2600, -1800, 200))
builder_class = unreal.load_class(None, "/Script/UnrealEd.CubeBuilder")
if builder_class is None:
    raise RuntimeError("The supported engine has no native CubeBuilder for volume authoring")
builder = unreal.new_object(builder_class, outer=interior_volume)
for axis in ("x", "y", "z"):
    builder.set_editor_property(axis, 600.0)
if not actors.rebuild_brush(interior_volume, builder):
    raise RuntimeError("could not shape the example Audio Gameplay Volume")
subobjects = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
subobject_data = unreal.SubobjectDataBlueprintFunctionLibrary

def add_component(actor, component_class):
    root = subobjects.k2_gather_subobject_data_for_instance(actor)[0]
    handle, reason = subobjects.add_new_subobject(unreal.AddNewSubobjectParams(parent_handle=root, new_class=component_class))
    if not subobject_data.is_handle_valid(handle):
        raise RuntimeError(f"could not add {component_class.get_name()} to {actor.get_actor_label()}: {reason}")
    return subobject_data.get_object(subobject_data.get_data(handle))

zone = add_component(interior_volume, unreal.RetakeAmbienceZoneComponent.static_class())
zone.set_editor_property("profile", interior)
zone.set_editor_property("priority", 1)
add_component(interior_volume, unreal.FilterVolumeComponent.static_class()).set_editor_property("exterior_lpf", 4000.0)
emitter = actors.spawn_actor_from_class(unreal.AmbientSound, unreal.Vector(-600, -1600, 150))
emitter_audio = emitter.get_editor_property("audio_component")
emitter_audio.set_editor_property("sound", hum)
emitter_audio.set_editor_property("attenuation_settings", attenuation)

# Mod-local physical materials with the canonical surfaces: the signal material is metal, everything else gets a concrete floor.
def physical_material(name, surface):
    physical = assets.create_asset(name, f"/{mod}/Physics", unreal.PhysicalMaterial, unreal.PhysicalMaterialFactoryNew())
    physical.set_editor_property("surface_type", surface)
    library.save_asset(physical.get_path_name())
    return physical

concrete = library.load_asset(f"/{mod}/Physics/PM_Concrete")
if not concrete:
    raise RuntimeError("The Basic template's concrete material is missing")
material.set_editor_property("phys_material", physical_material("PM_ExampleMetal", unreal.PhysicalSurface.SURFACE_TYPE4))
unreal.MaterialEditingLibrary.recompile_material(material)
library.save_asset(material_path)
floor_material = assets.create_asset("M_ExampleFloor", f"/{mod}/Materials", unreal.Material, unreal.MaterialFactoryNew())
gray = unreal.MaterialEditingLibrary.create_material_expression(floor_material, unreal.MaterialExpressionConstant3Vector, -400, 0)
gray.set_editor_property("constant", unreal.LinearColor(0.35, 0.35, 0.33, 1.0))
unreal.MaterialEditingLibrary.connect_material_property(gray, "", unreal.MaterialProperty.MP_BASE_COLOR)
floor_material.set_editor_property("used_with_nanite", True)
floor_material.set_editor_property("phys_material", concrete)
unreal.MaterialEditingLibrary.recompile_material(floor_material)
library.save_asset(floor_material.get_path_name())
for actor in actors.get_all_level_actors():
    if isinstance(actor, unreal.StaticMeshActor):
        overrides = actor.static_mesh_component.get_editor_property("override_materials")
        if not any(overrides):
            actor.static_mesh_component.set_material(0, floor_material)
    elif isinstance(actor, unreal.RetakeFieldUplinkBombSite):
        for component in actor.get_components_by_class(unreal.PrimitiveComponent):
            component.set_phys_material_override(concrete)

def poster(name, width, height):
    """A 16:9 vertical gradient with the UI texture settings presentation art needs."""
    top, bottom = (24, 30, 36), (196, 120, 40)
    rows = b"".join(b"\x00" + bytes(round(a + (b - a) * y / (height - 1)) for a, b in zip(top, bottom)) * width for y in range(height))
    chunk = lambda tag, data: struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    path = generated / f"{name}.png"
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))
    texture = import_file(path, f"/{mod}/UI", name)
    for prop, value in (("compression_settings", unreal.TextureCompressionSettings.TC_EDITOR_ICON),
                        ("lod_group", unreal.TextureGroup.TEXTUREGROUP_UI),
                        ("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS), ("never_stream", True), ("srgb", True)):
        texture.set_editor_property(prop, value)
    library.save_asset(texture.get_path_name())
    return texture

map_settings = next(actor for actor in placed if isinstance(actor, unreal.RetakeMapSettings))
for prop, value in (("display_name", "Retake SDK Example"), ("description", "Playable SDK examples: modes, interaction, traversal, destruction and ambience."),
                    ("loading_art", poster("T_ExampleLoading", 1920, 1080)), ("preview", poster("T_ExamplePreview", 1024, 576)),
                    ("ambience", exterior)):
    map_settings.set_editor_property(prop, value)
capture = actors.spawn_actor_from_class(unreal.RetakeMinimapCapture, unreal.Vector(0, 0, 0))
capture.set_editor_property("capture_size", 8000.0)
capture.set_editor_property("capture_height", 1000.0)
print(f"event=ModKitExamplePresentation zone={zone.get_path_name()} emitter={emitter.get_actor_label()} capture={capture.get_actor_label()} "
      f"ambience={exterior.get_path_name()}")
author_acoustics(world)
if not unreal.EditorLoadingAndSavingUtils.save_map(world, map_path):
    raise RuntimeError("could not save " + map_path)
print(f"event=ModKitValidationCreated map={map_path} elements={len(placed)} rungs={rungs}")

# Preserve generated import inputs through the same recovery selection every creator uses.
selected = sorted(p.relative_to(mod_directory).as_posix() for p in generated.rglob("*") if p.is_file())
(mod_directory / "SourceFiles.json").write_text(json.dumps({"format": 1, "files": selected}, indent=2) + "\n", encoding="utf-8")
request_file.replace(request_file.with_suffix(".completed.json"))
