using UnrealBuildTool;
public class RetakeModKitServerTarget : TargetRules { public RetakeModKitServerTarget(TargetInfo Target) : base(Target) { Type = TargetType.Server; DefaultBuildSettings = BuildSettingsVersion.Latest; IncludeOrderVersion = EngineIncludeOrderVersion.Latest; ExtraModuleNames.Add("RetakeModKit"); } }
