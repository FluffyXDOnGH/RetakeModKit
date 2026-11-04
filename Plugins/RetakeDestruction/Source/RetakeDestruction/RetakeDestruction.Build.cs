using UnrealBuildTool;

public class RetakeDestruction : ModuleRules
{
    public RetakeDestruction(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false; // Keep each plugin source file independently compilable.
        SetupIrisSupport(Target); // The runtime actor replicates natively under Iris.
        // Loaded by both the game and the content-only Map Kit: never depend on Retake game modules.
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "PhysicsCore", "GeometryCollectionEngine", "RetakeMapSDK" });
        PrivateDependencyModuleNames.AddRange(new[] { "Chaos", "ChaosSolverEngine", "Niagara" });
        // Definition validation reads a mod break effect's life cycle from its Niagara graphs.
        if (Target.bBuildEditor) PrivateDependencyModuleNames.Add("NiagaraEditor");
    }
}
