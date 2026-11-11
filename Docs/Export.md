# Export and test

The kit includes the game cook registries and compatibility metadata used by its tools. Creators never run the studio's `stage-sdk`, `stage-game` or migration tools.

Use **Retake → Mod Kit Workspace** throughout:

1. Select your map or weapon pack and click **Save mod content**.
2. Run **Check map** or **Check weapon draft** and review the reported issues.
3. Run **Prepare map data** or **Prepare weapon profiles** as needed.
4. In **Export your mod** or **Export your weapon pack**, choose a revision and click **Export mod**.
5. Click **Open export folder** to find the finished bundle.

If tools are not ready, use **Enable authoring tools** in the workspace. Setup, preparation and export report progress in the editor. A compatibility error still needs a matching supported kit/game combination; the UI does not bypass that check.

The exporter validates owned dependencies, performs tagged Windows and LinuxServer cooks, verifies preparation and compatibility, and produces `Exports/<ModId>/<Revision>/`. A complete bundle contains `manifest.json`, the mod's `.uplugin` descriptor and both platforms' `.pak`, `.utoc` and `.ucas` containers. Keep the complete bundle together; a Linux server distributing mods also needs the Windows artifacts for clients.

Each revision is immutable. Recooking or editing content produces a new revision; never overwrite an already distributed revision. The installer verifies hashes before activation and preserves the previous revision on failure.

Export also writes `Recovery/<ModId>/<Revision>.source.zip`. This separate local archive contains editable assets, explicitly selected source art and reproducible build metadata. It is not a server download payload. Sharing it is your choice. Maintain `SourceFiles.json` for supported raw inputs under `SourceArt/`; unrelated files, credentials, caches and engine files do not belong in recovery.

For changes in an existing workspace, edit and export with a new revision. Recovery is for restoring a separate clean workspace and must never overwrite current work.

For maps, the **Test in Retake** card lets you browse for an existing compatible client/server, choose an export and game mode, check the installations, and launch. A local Linux server uses WSL and an existing server package. Weapon-pack gameplay testing uses the installed game's pack/profile configuration. The kit does not build game executables. Testing may require a controlled game restart to change an already mounted revision.

Use the normal export and test actions. They check preparation, dependencies and compatibility before a mod can be loaded by Retake.
