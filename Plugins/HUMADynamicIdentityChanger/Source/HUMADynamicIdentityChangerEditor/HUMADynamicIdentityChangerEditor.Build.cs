using UnrealBuildTool;
using System.IO;

public class HUMADynamicIdentityChangerEditor : ModuleRules
{
	public HUMADynamicIdentityChangerEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "AnimGraph", "HUMADynamicIdentityChanger" });
		PrivateDependencyModuleNames.AddRange(new[] { "UnrealEd", "BlueprintGraph", "Kismet", "AssetRegistry", "UMG", "UMGEditor", "Slate", "SlateCore", "Projects" });
		string Hdf5 = Path.Combine(PluginDirectory, "ThirdParty", "HDF5");
		PrivateIncludePaths.Add(Path.Combine(Hdf5, "include"));
		PublicAdditionalLibraries.Add(Path.Combine(Hdf5, "lib", "hdf5.lib"));
		PublicDelayLoadDLLs.Add("hdf5.dll");
		PrivateDefinitions.Add("H5_BUILT_AS_DYNAMIC_LIB=1");
		RuntimeDependencies.Add(Path.Combine(Hdf5, "bin", "hdf5.dll"));
	}
}
