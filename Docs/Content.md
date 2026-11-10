# Content and distribution

The public snapshot is built from an explicit file allowlist. `ModKit/Files.json` records each delivered file's SHA-256 and size. `ModKit/Content.json` records editable binary assets, provenance and distribution status. Neither a renamed asset nor successful cooking proves permission to redistribute editable source.

The GitHub kit also includes the five public Mod Kit modules and their `.modules`/`.uclasses` files. `ModKit/Prebuilt.json` binds these bytes to their engine build and the included native source. The source remains available for rebuilding with another UE6 installation. Engine binaries, game executables, build caches and studio-only plugins are excluded.

Only approved original or appropriately licensed resources belong in the release. MOHAA, Spearhead, CoD4, studio migration packs and their indirect asset dependencies are excluded. Original examples use fresh creator-owned namespaces.

Epic engine resources referenced by path are obtained from the creator's own engine installation and are not copied into this kit. Shared gameplay authoring resources need both technical compatibility and individual content approval. Preserve third-party notices and restrictions.

When sharing a mod or its editable files, include only content you have permission to redistribute and retain the required notices.
