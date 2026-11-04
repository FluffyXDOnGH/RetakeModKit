using UnrealBuildTool;
public class RetakeModKitEditorTarget : TargetRules { public RetakeModKitEditorTarget(TargetInfo Target) : base(Target) { Type = TargetType.Editor; DefaultBuildSettings = BuildSettingsVersion.Latest; IncludeOrderVersion = EngineIncludeOrderVersion.Latest; ExtraModuleNames.Add("RetakeModKit"); } }
