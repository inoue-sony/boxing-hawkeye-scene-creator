// Copyright (c) 2025 WOCDeformer Plugin. All Rights Reserved.

using UnrealBuildTool;

public class WOCDeformerEditor : ModuleRules
{
	public WOCDeformerEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Security hardening
		bUseUnity = false;
		ShadowVariableWarningLevel = WarningLevel.Error;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"AssetTools",
			"AnimGraph",
			"BlueprintGraph",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"WOCDeformer",
			"AnimGraphRuntime",
			"PropertyEditor",
			"Slate",
			"SlateCore",
			"InputCore",
			"DesktopPlatform",
		});
	}
}
