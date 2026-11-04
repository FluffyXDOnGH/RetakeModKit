using UnrealBuildTool;

public class RetakeMapSDK : ModuleRules
{
    public RetakeMapSDK(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false; // Keep each SDK source file independently compilable.
        // The SDK is loaded by both the game and the content-only Map Kit: never depend on Retake game modules.
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "GameplayTags" });
        // Ambience zones implement the Audio Gameplay Volume interaction interface in a public header (engine plugin, declared in the .uplugin).
        PublicDependencyModuleNames.Add("AudioGameplay");
        PrivateDependencyModuleNames.AddRange(new[] { "Json", "JsonUtilities", "RenderCore", "Niagara" });
        if (Target.bBuildEditor) PrivateDependencyModuleNames.Add("Projects"); // Creator-facing weapon metadata lives beside the SDK contract.
        if (Target.bBuildEditor) PrivateDependencyModuleNames.Add("AssetRegistry"); // Complete source closure for independent weapon preparation.
    }
}
