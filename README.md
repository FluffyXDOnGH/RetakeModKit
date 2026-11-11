![Retake Mod Kit](Docs/Images/retake-modkit-banner.png)

# Retake Mod Kit

Create maps and weapons for **Retake** in a standalone Unreal project.

Create, check, prepare and export your mod from the **Retake Mod Kit Workspace** inside Unreal Editor.

## 1. Get the kit

On this GitHub page, choose **Code → Download ZIP** and extract the folder. You can also clone the repository using your Git application. The kit includes its prebuilt Windows modules and source.

## 2. Open the project

Double-click **RetakeModKit.uproject**, then open **Retake → Mod Kit Workspace**.

You need Unreal Engine 6 installed on Windows; the engine itself is not included. If Unreal asks which engine to use, select your UE6 installation. If it reports missing or incompatible modules, follow [troubleshooting](Docs/Compatibility.md#opening-with-another-ue6-build).

On first use, click **Enable authoring tools** in the workspace. It uses the editor's bundled Python to prepare the required tools and compatibility records. Progress appears in the workspace; no terminal or separate Python installation is needed for this editor workflow.

## 3. Create your mod

| Start with | Guide |
|---|---|
| An empty map, a basic starting area, or a separate map example | [Create a map](Docs/Maps.md) |
| An empty weapon pack or the original example carbine | [Create weapons](Docs/Weapons.md) |

Click **+ New mod**, choose **Map** or **Weapons**, and give it a name. Keep your assets inside that mod's Content folder. The workspace also provides original examples to explore.

## 4. Prepare, export and test

**Create → Author → Validate → Prepare/Bake → Export → Test**

Use the workspace's **Save**, **Check**, **Prepare** and **Export** controls. **Open export folder** takes you to the finished files. Exports include Windows and Linux server content plus a separate editable-source recovery archive. See the [export guide](Docs/Export.md) for details.

Keep the Mod Kit and Retake up to date. Export checks SDK and game compatibility before cooking. Use your installed Retake game for gameplay testing; uploading and workshop integration are separate from the authoring workflow.

[Compatibility & troubleshooting](Docs/Compatibility.md) · [License](LICENSE.txt) · [Third-party notices](Notices/ThirdParty.md)
