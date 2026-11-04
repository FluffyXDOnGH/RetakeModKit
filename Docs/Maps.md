# Make a map

Use the Retake creation action or the kit's Python interpreter from the project root:

```powershell
.\.venv\Scripts\python.exe Scripts\ModKit\retake_kit.py create --mod RetakeMod_MyMap --template Empty
```

`Empty` is the default and contains no demonstrations. Choose `--template Basic` for a simple starting area with geometry, lighting and map setup. An unfinished Empty map is editable but is not yet exportable.

To create the separate original SDK demonstration in a fresh namespace:

```powershell
.\.venv\Scripts\python.exe Scripts\ModKit\retake_kit.py example --mod RetakeMod_MyExample
```

The example generator refuses an existing or edited workspace. It demonstrates supported mode starts/objectives, doors, ladders, destruction, traversal, surfaces, ambience, acoustic regions and map presentation. Its geometry, generated audio and art are independent of the weapon example. It is not placed in the Empty or Basic template.

Minefields remain available in the palette but are not preplaced in this example: they require a creator-owned explosion effect, spatial cues and a generated mine layout. The validator reports missing resources before export.

Place SDK elements using the Retake palette, configure the supported modes and keep all imported assets under your mod's content root. Configure map-owned sounds through the supported ambience/interaction definitions. Reference the shared SDK material where appropriate; custom materials are supported only by qualified shader combinations.

Save the map before checking readiness:

```powershell
.\.venv\Scripts\python.exe Scripts\ModKit\retake_kit.py status --mod RetakeMod_MyMap
.\.venv\Scripts\python.exe Scripts\ModKit\retake_kit.py prepare --mod RetakeMod_MyMap
```

The preparation workflow reports missing/outdated geometry and required acoustics. Configure and bake visibility and minimaps using their respective actions when used. Optional data must be explicitly absent or current; stale data cannot be silently accepted. Server visibility policy may impose additional requirements.

Preview in the editor is an authoring view, not Retake gameplay. Test actual interaction, movement, objectives, multiplayer and round reset using an installed compatible game after export.
