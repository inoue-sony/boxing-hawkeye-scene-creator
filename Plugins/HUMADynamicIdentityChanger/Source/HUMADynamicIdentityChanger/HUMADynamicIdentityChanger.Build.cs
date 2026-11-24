using UnrealBuildTool;

public class HUMADynamicIdentityChanger : ModuleRules
{
	public HUMADynamicIdentityChanger(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "AnimGraphRuntime", "UMG" });
		PrivateDependencyModuleNames.AddRange(new[] { "Slate", "SlateCore", "InputCore", "Json" });
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("bcrypt.lib");
		}
	}
}
