using UnrealBuildTool;

public class HUMAClothBindEditor : ModuleRules
{
    public HUMAClothBindEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "UnrealEd", "AssetTools", "HUMAClothBind",
            "Slate", "SlateCore", "InputCore", "PropertyEditor", "AdvancedPreviewScene", "Json", "RHI",
            "ContentBrowser", "AssetRegistry"
        });
    }
}