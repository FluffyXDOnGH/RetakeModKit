using UnrealBuildTool;
public class RetakeModKitTarget : TargetRules { public RetakeModKitTarget(TargetInfo Target) : base(Target) { Type = TargetType.Game; DefaultBuildSettings = BuildSettingsVersion.Latest; IncludeOrderVersion = EngineIncludeOrderVersion.Latest; ExtraModuleNames.Add("RetakeModKit"); } }
