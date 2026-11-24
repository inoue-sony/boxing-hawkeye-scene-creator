using UnrealBuildTool;

public class BoxingHawkEyeSceneCreatorTarget : TargetRules
{
	public BoxingHawkEyeSceneCreatorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("BoxingHawkEyeSceneCreator");
	}
}
