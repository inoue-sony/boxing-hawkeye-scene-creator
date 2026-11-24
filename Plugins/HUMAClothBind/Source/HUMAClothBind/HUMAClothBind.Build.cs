using UnrealBuildTool;

public class HUMAClothBind : ModuleRules
{
    public HUMAClothBind(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        // 複数の .cpp が匿名名前空間に同名の変数（States など）を持つため、ユニティビルドでまとめると衝突する
        bUseUnity = false;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "ProceduralMeshComponent" });
        PrivateDependencyModuleNames.AddRange(new[] { "RenderCore", "RHI", "Renderer", "Projects", "Json", "JsonUtilities", "GeometryCore" });
        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.AddRange(new[] { "UnrealEd", "AssetTools", "PropertyEditor", "Slate", "SlateCore" });
            RuntimeDependencies.Add("$(PluginDir)/Resources/Topology/HUMA_v1_fullbody_quads.bin");
            RuntimeDependencies.Add("$(PluginDir)/Resources/Topology/HUMA_v1_female_triangles.bin");
            RuntimeDependencies.Add("$(PluginDir)/Resources/Topology/HUMA_v1_male_triangles.bin");
        }
    }
}