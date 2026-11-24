using UnrealBuildTool;

public class BoxingHawkEyeSceneCreatorEditorTarget : TargetRules
{
	public BoxingHawkEyeSceneCreatorEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("BoxingHawkEyeSceneCreator");
	}
}
