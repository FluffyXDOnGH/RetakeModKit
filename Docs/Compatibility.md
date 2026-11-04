# Compatibility and troubleshooting

`ModKit/Engine.json` pins the authoring engine. `SDK.lock.json` identifies the SDK contract and supplied base cook metadata. `ModKit/qualification.json` identifies reviewed producer/consumer combinations; `ModKit/Release.json` states which release acceptance has actually completed.

An SDK version alone does not establish compatibility. Cooked serialization, collision, meshes, destruction, Niagara and shader dependencies must also match a reviewed combination. SDK materials reduce shader dependencies but do not exempt a map from other checks. Source-engine revision and derived-data keys are evidence inputs, not proof by themselves.

Compatible game updates should continue accepting existing exports. Changes to native game logic do not inherently require mod updates. Breaking SDK/default/resource/body/cooked-data changes need explicit compatibility decisions. Clients and server still require identical content revisions, and replays retain their exact dependencies.

| Problem | Action |
|---|---|
| Unsupported engine revision | Use the exact clean source checkout specified by this release. Do not reset a checkout containing your work. |
| Missing editor/compiler/Python | Complete the prerequisites in README, then rerun Setup. |
| Missing binary assets / LFS pointers | Make a fresh clone or extract the complete kit ZIP. This preview stores assets directly in Git and does not require Git LFS. |
| Missing or modified SDK contract | Restore this kit release. Do not edit snapshots to silence the check. |
| No reviewed platform compatibility | Install a kit release qualified for that game. Do not force candidate export. |
| Stale preparation | Save your inputs, run the relevant preparation action, then export a new revision. |
| Missing base runtime resource | Use approved shared SDK resources or mod-owned assets; do not copy private game content. |
| Editor moved | Rerun Setup with the new engine path. |

Logs are under `Saved/ModKit/Logs`. When reporting a problem, include kit version, engine revision, game release and the relevant error; omit credentials and personal paths where possible.

UE5.8 support is a separate development branch and will not be advertised until both platform exports and packaged runtime acceptance pass. This kit does not require broad future engine compatibility to be useful.
