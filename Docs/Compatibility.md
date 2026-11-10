# Compatibility and troubleshooting

Keep the Mod Kit and Retake up to date. Setup accepts UE6 source builds and installed engines without requiring a particular revision or a clean Git checkout. `ModKit/Engine.json` records the required major version. SDK compilation, asset loading and cooked-content compatibility are still checked separately.

The GitHub kit includes five prebuilt editor/runtime DLLs and their original Unreal module manifests. Setup uses them only when the engine build identity and kit source match. A different UE6 engine uses the included source fallback and needs its supported Visual Studio C++ tools. Never edit a `.modules` BuildId to force a match. The kit includes no Unreal Engine installation or Retake game executable.

Compatibility checks cover cooked serialization, collision, meshes, destruction, Niagara and shaders. SDK materials reduce shader dependencies but do not exempt a map from the other checks. Keep these checks enabled when creating and sharing mods.

Compatible game updates should continue accepting existing exports. Changes to native game logic do not inherently require mod updates. Breaking SDK/default/resource/body/cooked-data changes need explicit compatibility decisions. Clients and server still require identical content revisions, and replays retain their exact dependencies.

| Problem | Action |
|---|---|
| Unsupported engine major version | Select a UE6 installation with `Setup.ps1 -EngineRoot '<engine path>'`. |
| Prebuilt DLLs target another engine | Rerun Setup with your engine. It will build the included source; install the C++ toolchain required by that engine. |
| Missing modules / game module could not be found | Finish Setup successfully, then use `Open-ModKit.ps1`. If setup fails, include its console error and `Saved/ModKit/Logs/build.log` when reporting it. |
| SDK compile or contract failure on a UE6 build | Report the build log or `Saved/ModKit/contract-report.json`; accepting the engine does not establish SDK or content compatibility. |
| Missing editor/compiler/Python | Complete the prerequisites in README, then rerun Setup. |
| Missing binary assets / LFS pointers | Make a fresh clone or extract the complete kit ZIP. Assets are stored directly in Git; Git LFS is not required. |
| Missing or modified SDK contract | Restore this kit release. Do not edit snapshots to silence the check. |
| No matching platform compatibility | Update the kit and game, then rerun Setup. If the mismatch remains, report the diagnostic; do not bypass it. |
| Stale preparation | Save your inputs, run the relevant preparation action, then export a new revision. |
| Missing base runtime resource | Use approved shared SDK resources or mod-owned assets; do not copy private game content. |
| Editor moved | Rerun Setup with the new engine path. |

Logs are under `Saved/ModKit/Logs`. When reporting a problem, include the relevant diagnostic and setup information; omit credentials and personal paths where possible.
