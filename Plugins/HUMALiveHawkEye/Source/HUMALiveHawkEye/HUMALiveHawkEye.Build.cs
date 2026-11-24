using UnrealBuildTool;

public class HUMALiveHawkEye : ModuleRules
{
	public HUMALiveHawkEye(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "HUMADynamicIdentityChanger" });
		PrivateDependencyModuleNames.AddRange(new[] { "NNE", "Json", "ProceduralMeshComponent" });
		PrivateIncludePaths.Add(System.IO.Path.Combine(ModuleDirectory, "Private", "Core"));
	}
}
