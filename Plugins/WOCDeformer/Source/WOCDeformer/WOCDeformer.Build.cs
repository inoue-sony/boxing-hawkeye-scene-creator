// Copyright (c) 2025 WOCDeformer Plugin. All Rights Reserved.

using UnrealBuildTool;

public class WOCDeformer : ModuleRules
{
	public WOCDeformer(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Security hardening: enable SDL checks and Control Flow Guard
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicDefinitions.Add("_CRT_SECURE_NO_WARNINGS=0");
		}
		bUseUnity = false; // Improves per-TU type checking and warning accuracy
		ShadowVariableWarningLevel = WarningLevel.Error;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"NNE",
			"NNERuntimeORT",  // NNERuntimeORTCpu
			"RenderCore",
			"RHI",
			"ComputeFramework",
			"OptimusCore",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AnimationCore",
			"AnimGraphRuntime",
		});

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"DesktopPlatform",
				"Slate",
				"SlateCore",
			});
		}
	}
}
