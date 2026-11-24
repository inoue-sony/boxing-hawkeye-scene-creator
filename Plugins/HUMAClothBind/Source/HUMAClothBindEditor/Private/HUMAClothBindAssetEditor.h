#pragma once

#include "CoreMinimal.h"
#include "AdvancedPreviewScene.h"
#include "HUMAClothBindPreviewSettings.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "UObject/StrongObjectPtr.h"

class UHUMAClothBindAsset;
class AHUMAClothPreviewActor;
class ASkeletalMeshActor;
class USkeletalMeshComponent;
class UMeshComponent;
class IDetailsView;
class SHUMAClothBindViewport;
class SDockTab;
class FSpawnTabArgs;

class FHUMAClothBindPreviewScene : public FAdvancedPreviewScene
{
public:
    FHUMAClothBindPreviewScene(UHUMAClothBindAsset* Asset, UHUMAClothBindPreviewSettings* Settings);
    virtual ~FHUMAClothBindPreviewScene() override;
    virtual void Tick(float DeltaTime) override;
    void Rebuild();
    void ApplyControls();
    FString GetStatus() const;
    USkeletalMeshComponent* GetBodyComponent() const;
    AHUMAClothPreviewActor* GetGarmentActor() const;
private:
    void Clear();
    TWeakObjectPtr<UHUMAClothBindAsset> Asset;
    TWeakObjectPtr<UHUMAClothBindPreviewSettings> Settings;
    TWeakObjectPtr<ASkeletalMeshActor> BodyActor;
    TWeakObjectPtr<AHUMAClothPreviewActor> GarmentActor;
    TWeakObjectPtr<UMeshComponent> RestGarment;
    FName AppliedMorph;
    FString SetupError;
    FString MorphError;
    float PlaybackTime = 0;
};

class FHUMAClothBindAssetEditor : public FAssetEditorToolkit
{
public:
    void Init(UHUMAClothBindAsset* Asset, EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& Host);
    virtual void RegisterTabSpawners(const TSharedRef<FTabManager>& TabManager) override;
    virtual void UnregisterTabSpawners(const TSharedRef<FTabManager>& TabManager) override;
    virtual FName GetToolkitFName() const override { return TEXT("HUMAClothBindEditor"); }
    virtual FText GetBaseToolkitName() const override { return FText::FromString(TEXT("HUMA Cloth Bind")); }
    virtual FString GetWorldCentricTabPrefix() const override { return TEXT("Cloth Bind "); }
    virtual FLinearColor GetWorldCentricTabColorScale() const override { return FLinearColor(0.1f, 0.6f, 0.4f); }
    TSharedPtr<FHUMAClothBindPreviewScene> GetPreviewScene() const { return PreviewScene; }
    UHUMAClothBindPreviewSettings* GetPreviewSettings() const { return PreviewSettings.Get(); }
    FString GetViewportStatus() const;
private:
    TSharedRef<SDockTab> SpawnViewport(const FSpawnTabArgs& Args);
    TSharedRef<SDockTab> SpawnDetails(const FSpawnTabArgs& Args);
    TSharedRef<SDockTab> SpawnPreviewSettings(const FSpawnTabArgs& Args);
    void BakeAndSave();
    void BatchBindSelectedGarments();
    void OnAssetChanged(const FPropertyChangedEvent& Event);
    void OnPreviewChanged(const FPropertyChangedEvent& Event);
    UHUMAClothBindAsset* Asset = nullptr;
    TStrongObjectPtr<UHUMAClothBindPreviewSettings> PreviewSettings;
    TSharedPtr<FHUMAClothBindPreviewScene> PreviewScene;
    TSharedPtr<SHUMAClothBindViewport> Viewport;
    TSharedPtr<IDetailsView> Details;
    TSharedPtr<IDetailsView> PreviewDetails;
};
