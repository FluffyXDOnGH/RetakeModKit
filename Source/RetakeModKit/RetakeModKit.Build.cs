using UnrealBuildTool;
public class RetakeModKit : ModuleRules { public RetakeModKit(ReadOnlyTargetRules Target) : base(Target) { PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs; PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" }); } }
