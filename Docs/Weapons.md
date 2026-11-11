# Make a weapon pack

Weapon mods contain definitions and presentation/preparation data. Retake implements their authoritative behavior; creator gameplay DLLs and replacement runtime classes are not supported.

Open **Retake → Mod Kit Workspace**, click **+ New mod**, select **Weapons**, and enter a name. Enable authoring tools in the workspace if prompted.

Click **Open / create weapon example** to explore the separate original blockout carbine. For your own pack, use **Create authoring references** to prepare the shared body/arms and pose basis, then the asset buttons to add weapon specifications, ammunition data, animation profiles and tracer profiles. **Open weapon catalogue** opens the pack's IDs, profile bindings and defaults.

It imports the provided original carbine and template-derived body/arms, creates owned definitions, procedural example sounds and editable reference/action clips, then runs native pose preparation and validation. It never overwrites an existing workspace. The example is a teaching blockout, with simple pose/motion and a timed reload sound; its appearance is not a production animation pack.

The example authors both hands in its clips. Retake's optional automatic support-hand IK uses the weapon's `FABRIK_Socket`; add that socket deliberately when adopting that workflow. The body basis also includes the shared lean and clearance references, so new poses can be prepared against the game's common procedural posture rules.

The body/arms use the shared Manny hierarchy. The reference normalizer restores exact published reference values after import, within a strict rounding envelope. A structurally different rig is refused; changing thresholds or the body contract to force it through is not an authoring solution. Skeleton reference and mesh reference are deliberately checked separately.

Author a pack catalogue, stable weapon/ammunition/profile IDs, mesh/material/audio references and the supported animation profiles under your mod root. Retain the shared skeleton, socket conventions and body/pose contract. Changing a mesh's appearance does not permit changing authoritative hit geometry or the shared skeleton contract.

Use animation events for supported presentation and action timing; do not implement damage or inventory mutation in arbitrary animation callbacks. The server's native weapon simulation remains authoritative.

Preparation uses a shared SDK authoring basis or an explicitly mod-owned basis. Keep the preparation recipe and all editable animation inputs. Click **Save mod content**, **Check weapon draft**, then **Prepare weapon profiles**. Preparation uses the catalogue's bindings and retains its recipes automatically. Review the issues shown in the workspace, then choose a revision and click **Export mod** when the checks pass. Use **Open export folder** to find the finished bundle.

Profiles select behavior data without duplicating identical assets. Pack IDs separate weapons that share display names. Map/loadout references must declare exact weapon-pack dependencies, and the server selects which packs/profiles are enabled.

Keep weapon IDs stable: a weapon is addressed as `<ModId>.<WeaponId>`, not by its display name. These IDs let server owners refer to pack weapons alongside base-game weapons in loadout configuration. Author sensible defaults in the pack; server configuration determines the effective choices and rules. Configuration features are supplied by the installed Retake client/server, while the kit supplies authoring and export tools.

Follow [Export.md](Export.md) for export and testing. A valid weapon pack must install without rebuilding Retake or rebaking private game assets.
