# Compatibility and troubleshooting

Keep the Mod Kit and Retake up to date. Use the engine specified in `ModKit/Engine.json`; Setup and export read the supplied SDK and game compatibility records automatically.

Compatibility checks cover cooked serialization, collision, meshes, destruction, Niagara and shaders. SDK materials reduce shader dependencies but do not exempt a map from the other checks. Keep these checks enabled when creating and sharing mods.

Compatible game updates should continue accepting existing exports. Changes to native game logic do not inherently require mod updates. Breaking SDK/default/resource/body/cooked-data changes need explicit compatibility decisions. Clients and server still require identical content revisions, and replays retain their exact dependencies.

| Problem | Action |
|---|---|
| Unsupported engine revision | Use the exact clean source checkout specified by this release. Do not reset a checkout containing your work. |
| Missing editor/compiler/Python | Complete the prerequisites in README, then rerun Setup. |
| Missing binary assets / LFS pointers | Make a fresh clone or extract the complete kit ZIP. Assets are stored directly in Git; Git LFS is not required. |
| Missing or modified SDK contract | Restore this kit release. Do not edit snapshots to silence the check. |
| No matching platform compatibility | Update the kit and game, then rerun Setup. If the mismatch remains, report the diagnostic; do not bypass it. |
| Stale preparation | Save your inputs, run the relevant preparation action, then export a new revision. |
| Missing base runtime resource | Use approved shared SDK resources or mod-owned assets; do not copy private game content. |
| Editor moved | Rerun Setup with the new engine path. |

Logs are under `Saved/ModKit/Logs`. When reporting a problem, include the relevant diagnostic and setup information; omit credentials and personal paths where possible.
