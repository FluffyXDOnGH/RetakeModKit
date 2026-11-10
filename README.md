![Retake Mod Kit](Docs/Images/retake-modkit-banner.png)

# Retake Mod Kit

Create maps and weapons for **Retake** in a standalone Unreal project.

Keep the Mod Kit and Retake up to date. Setup and export check the required engine, SDK and game compatibility automatically.

## 1. Get the kit

Clone the kit from GitHub. The repository includes prebuilt Windows editor modules alongside their source:

```powershell
git clone https://github.com/humarkx/RetakeModKit.git
cd RetakeModKit
```

## 2. Set up Unreal

Use an **Unreal Engine 6** installation for Windows, either an installed engine or a source build. No specific Git revision or pristine checkout is required. For a source build, build **UnrealEditor** and **ShaderCompileWorker** first.

Install Python **3.11+**. Setup uses the bundled editor DLLs when they match your engine build. If they do not match, install the Visual Studio C++ tools required by your engine; Setup builds the kit locally. Git is needed only for cloning or maintaining a Git checkout.

From the kit folder, run these commands with your engine's location:

```powershell
.\Setup.ps1 -EngineRoot 'D:\YourEngine'
.\Open-ModKit.ps1
```

Setup installs the authoring tools and either uses the matching prebuilt modules or compiles them for your engine. Open the kit with `Open-ModKit.ps1` after setup succeeds; double-clicking the `.uproject` can select another engine. Rerun Setup after changing engines or updating the kit.

Accepting a UE6 installation does not guarantee that every revision can compile the current SDK or load its assets. Setup still checks the SDK contract, and export still checks cooked-content compatibility. You use existing Retake client/server packages for gameplay; you do not build Retake.

## 3. Create your mod

| Start with | Guide |
|---|---|
| An empty map, a basic starting area, or a separate map example | [Create a map](Docs/Maps.md) |
| An empty weapon pack or the original example carbine | [Create weapons](Docs/Weapons.md) |

Create a new mod namespace before editing examples. Keep your assets inside that mod and track it in its own repository; the kit ignores `Mods/*`.

## 4. Prepare, export and test

**Create → Author → Validate → Prepare/Bake → Export → Test**

Follow the [export and testing guide](Docs/Export.md). Exports contain Windows and Linux server content; editable-source recovery is a separate archive. Uploading and workshop integration are outside this kit.

[Compatibility & troubleshooting](Docs/Compatibility.md) · [License](LICENSE.txt) · [Third-party notices](Notices/ThirdParty.md)
