# Export and test

Setup includes the verified game cook registries and compatibility metadata required by this kit release. Creators never run the studio's `stage-sdk`, `stage-game` or migration tools.

After saving and preparing your content:

```powershell
.\.venv\Scripts\python.exe Scripts\ModKit\retake_kit.py export --mod RetakeMod_MyMap --revision r1
```

The exporter validates owned dependencies, performs tagged Windows and LinuxServer cooks, verifies preparation and compatibility, and produces `Exports/<ModId>/<Revision>/`. A complete bundle contains `manifest.json`, the mod's `.uplugin` descriptor and both platforms' `.pak`, `.utoc` and `.ucas` containers. Keep the complete bundle together; a Linux server distributing mods also needs the Windows artifacts for clients.

Each revision is immutable. Recooking or editing content produces a new revision; never overwrite an already distributed revision. The installer verifies hashes before activation and preserves the previous revision on failure.

Export also writes `Recovery/<ModId>/<Revision>.source.zip`. This separate local archive contains editable assets, explicitly selected source art and reproducible build metadata. It is not a server download payload. Sharing it is your choice. Maintain `SourceFiles.json` for supported raw inputs under `SourceArt/`; unrelated files, credentials, caches and engine files do not belong in recovery.

```powershell
.\.venv\Scripts\python.exe Scripts\ModKit\retake_kit.py recook --source Recovery\RetakeMod_MyMap\r1.source.zip --revision r2
```

Run recovery from a separate clean copy of the kit, with no existing workspace of the same mod ID. For changes in an existing editable workspace, use `export` with a new revision instead; recovery never overwrites your work.

Use the Retake test action or `test-info` to select an existing compatible client and server. CLI discovery and launch options are documented by `retake_kit.py --help`; a local Linux server test uses WSL and an existing server package. The kit does not build game executables. Testing may require a controlled game restart to change an already mounted revision.

Do not use `--candidate` or `-RetakeModQualification` to distribute community mods. Those options are studio diagnostics for unqualified engine pairs. Normal public exports must pass the reviewed compatibility records.
