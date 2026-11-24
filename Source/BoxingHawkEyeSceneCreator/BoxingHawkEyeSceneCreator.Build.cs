using UnrealBuildTool;

// プロジェクト本体のモジュール。機能は Blueprint とプラグイン（Plugins/）側にあり、
// このモジュールは Visual Studio からプロジェクトとプラグインをまとめてビルドするための入口。
public class BoxingHawkEyeSceneCreator : ModuleRules
{
	public BoxingHawkEyeSceneCreator(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
	}
}
