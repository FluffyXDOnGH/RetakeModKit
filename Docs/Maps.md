# Make a map

Open **Retake → Mod Kit Workspace**, click **+ New mod**, select **Map**, and enter a name. If the workspace asks you to enable authoring tools, use its **Enable authoring tools** button.

**Empty** is the default and contains no demonstrations. Choose **Basic** for a simple starting area with geometry, lighting and map setup. An unfinished Empty map is editable but is not yet exportable.

Use **Example** in the map workspace to open or create the separate original SDK demonstration. Create your own mod before adapting its content.

The example generator refuses an existing or edited workspace. It demonstrates supported mode starts/objectives, doors, ladders, destruction, traversal, surfaces, ambience, acoustic regions and map presentation. Its geometry, generated audio and art are independent of the weapon example. It is not placed in the Empty or Basic template.

Minefields remain available in the palette but are not preplaced in this example: they require a creator-owned explosion effect, spatial cues and a generated mine layout. The validator reports missing resources before export.

Place SDK elements using the Retake palette, configure the supported modes and keep all imported assets under your mod's content root. Configure map-owned sounds through the supported ambience/interaction definitions. Reference the shared SDK material where appropriate; custom materials are supported only by qualified shader combinations.

Click **Save mod content**, then **Check map**. Review the listed issues and click **Prepare map data** when preparation is needed. When the checks pass, choose a revision in **Export your mod** and click **Export mod**.

The preparation workflow reports missing/outdated geometry and required acoustics. Configure and bake visibility and minimaps using their respective actions when used. Optional data must be explicitly absent or current; stale data cannot be silently accepted. Server visibility policy may impose additional requirements.

Preview in the editor is an authoring view, not Retake gameplay. Test actual interaction, movement, objectives, multiplayer and round reset using an installed compatible game after export.
