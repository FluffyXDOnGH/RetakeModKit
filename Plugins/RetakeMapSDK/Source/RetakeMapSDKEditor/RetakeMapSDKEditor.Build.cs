using UnrealBuildTool;

public class RetakeMapSDKEditor : ModuleRules
{
    public RetakeMapSDKEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false; // Keep each SDK source file independently compilable.
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "RetakeMapSDK" });
        PrivateDependencyModuleNames.AddRange(new[] { "UnrealEd", "Projects", "AssetRegistry", "Json", "JsonUtilities",
            "AssetTools", "ContentBrowser", "ToolMenus", "Slate", "SlateCore", "InputCore", "DesktopPlatform", "LevelEditor", "ApplicationCore" });
        // Map presentation checks and the minimap bake: surface types of physical materials.
        PrivateDependencyModuleNames.AddRange(new[] { "PhysicsCore", "Niagara", "MovieScene", "MovieSceneTracks", "ControlRig" });
        PrivateDependencyModuleNames.AddRange(new[] { "MeshDescription", "SkeletalMeshDescription", "RenderCore" });
    }
}
