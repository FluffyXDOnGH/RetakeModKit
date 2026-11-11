# Compatibility and troubleshooting

Keep the Mod Kit and Retake up to date. Setup accepts UE6 source builds and installed engines without requiring a particular revision or a clean Git checkout. `ModKit/Engine.json` records the required major version. SDK compilation, asset loading and cooked-content compatibility are still checked separately.

Open `RetakeModKit.uproject` directly, then use **Retake → Mod Kit Workspace**. **Enable authoring tools** prepares dependencies and engine records using the engine's bundled Python. The editor workflow does not need a terminal or a separate Python installation.

The GitHub kit includes five prebuilt editor/runtime DLLs and their original Unreal module manifests. They load when the engine build identity matches. The kit includes no Unreal Engine installation or Retake game executable.

## Opening with another UE6 build

If opening the project reports missing or incompatible modules, your engine needs its own compiled copy of the kit modules. With the Visual Studio C++ tools supported by that engine installed, use Unreal's rebuild prompt. If compilation fails, report the error and build log. The bundled DLLs cannot be made compatible by editing their `.modules` BuildId.

`Setup.ps1` and `Open-ModKit.ps1` remain advanced repair tools. They are not prerequisites for opening a matching prebuilt project. For that optional command-line route, install Python 3.11+ and the engine's C++ build tools, run `Setup.ps1 -EngineRoot '<engine path>'`, then `Open-ModKit.ps1`. Finish building or repairing the engine's editor, ShaderCompileWorker and UnrealPak if they are missing.

Compatibility checks cover cooked serialization, collision, meshes, destruction, Niagara and shaders. SDK materials reduce shader dependencies but do not exempt a map from the other checks. Keep these checks enabled when creating and sharing mods.

Compatible game updates should continue accepting existing exports. Changes to native game logic do not inherently require mod updates. Breaking SDK/default/resource/body/cooked-data changes need explicit compatibility decisions. Clients and server still require identical content revisions, and replays retain their exact dependencies.

| Problem | Action |
|---|---|
| Unsupported engine major version | Select a UE6 installation when opening the project. |
| Prebuilt DLLs target another engine | Rerun Setup with your engine. It will build the included source; install the C++ toolchain required by that engine. |
| Missing modules / game module could not be found | Finish Setup successfully, then use `Open-ModKit.ps1`. If setup fails, include its console error and `Saved/ModKit/Logs/build.log` when reporting it. |
| SDK compile or contract failure on a UE6 build | Report the build log or `Saved/ModKit/contract-report.json`; accepting the engine does not establish SDK or content compatibility. |
| Authoring tools unavailable | Click **Enable authoring tools** or **Tool setup** in the workspace. Check **Activity & details** if setup fails. |
| Missing editor/compiler | Install or repair your UE6 engine; another engine build may need its C++ toolchain to rebuild the kit. |
| Missing binary assets / LFS pointers | Make a fresh clone or extract the complete kit ZIP. Assets are stored directly in Git; Git LFS is not required. |
| Missing or modified SDK contract | Restore this kit release. Do not edit snapshots to silence the check. |
| No matching platform compatibility | Update the kit and game, then rerun Setup. If the mismatch remains, report the diagnostic; do not bypass it. |
| Stale preparation | Save your inputs, run the relevant preparation action, then export a new revision. |
| Missing base runtime resource | Use approved shared SDK resources or mod-owned assets; do not copy private game content. |
| Editor moved | Rerun Setup with the new engine path. |

Logs are under `Saved/ModKit/Logs`. When reporting a problem, include the relevant diagnostic and setup information; omit credentials and personal paths where possible.
