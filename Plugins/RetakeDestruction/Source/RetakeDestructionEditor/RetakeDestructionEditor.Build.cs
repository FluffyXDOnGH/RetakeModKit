using UnrealBuildTool;

public class RetakeDestructionEditor : ModuleRules
{
    public RetakeDestructionEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false; // Keep each plugin source file independently compilable.
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
        // Fracture conversion: Dataflow graphs, asset copies and the level-editor menu.
        // RetakeMapSDK: door leaf conversion links a placement to an ARetakeDoor.
        PrivateDependencyModuleNames.AddRange(new[] { "RetakeDestruction", "RetakeMapSDK", "UnrealEd", "AssetTools", "AssetRegistry", "ToolMenus", "PropertyEditor",
            "Slate", "SlateCore", "MeshDescription", "StaticMeshDescription", "PhysicsCore", "Chaos", "GeometryCollectionEngine",
            "DataflowCore", "DataflowEngine", "DataflowEditor", "GeometryCollectionNodes" });
    }
}
