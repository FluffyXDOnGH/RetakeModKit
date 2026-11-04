# Third-party prerequisites and notices

Unreal Engine is obtained separately from Epic Games under its own agreement:
https://www.unrealengine.com/eula/unreal

No engine installation is included. A reference to an engine asset is not a copy of that asset. Any Epic sample content added to the kit must be individually listed in `ModKit/Content.json` with the applicable distribution terms.

The weapon authoring body and arms derive from the Manny model in Epic's installed Third Person template (UE5.8.4). Copyright Epic Games, Inc. Their source files and changes are identified in `EpicExamples.json`. Epic's Unreal Engine EULA section 5(b) permits distribution of Examples, including modified Examples, in source or object form. These files are excluded from the Codnyx license grant; Epic's applicable terms continue to apply. No template textures, Animation Blueprints, retargeters or physics assets are bundled.

Python packages are installed from their official package distributions by Setup and are not vendored in the release ZIP:

- `blake3` 1.0.11: CC0-1.0 / Apache-2.0; retain the installed distribution's license files.
- `numpy` 2.3.5: BSD-3-Clause, with bundled-component notices in its distribution.

Git, Python, Visual Studio build tools and Epic engine dependencies retain their own terms. The Retake Mod Kit license applies only to covered Codnyx work. This preview stores its assets directly in Git; Git LFS is not required.
