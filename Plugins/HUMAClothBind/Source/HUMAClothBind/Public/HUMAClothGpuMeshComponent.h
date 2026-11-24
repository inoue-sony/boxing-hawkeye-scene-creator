#pragma once

#include "CoreMinimal.h"
#include "Components/MeshComponent.h"
#include "HUMAClothSourceMapping.h"
#include "HUMAClothGpuMeshComponent.generated.h"

class UHUMAClothBindAsset;
class USkeletalMeshComponent;
struct FHUMAClothGpuState;

UCLASS()
class HUMACLOTHBIND_API UHUMAClothGpuMeshComponent : public UMeshComponent
{
    GENERATED_BODY()
public:
    UHUMAClothGpuMeshComponent();
    void Initialize(UHUMAClothBindAsset* Asset, USkeletalMeshComponent* Source,
        TConstArrayView<int32> Mapping = {}, bool bIncludeBodyLODs = true);
    void Initialize(UHUMAClothBindAsset* Asset, USkeletalMeshComponent* Source, const FHUMAClothSourceLODs& Sources);
    virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
    virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
    virtual int32 GetNumMaterials() const override;
    TSharedPtr<FHUMAClothGpuState, ESPMode::ThreadSafe> GetGPUState() const { return State; }
private:
    void InitializeMaterials();
    UPROPERTY(Transient)
    TObjectPtr<UHUMAClothBindAsset> BindAsset;
    UPROPERTY(Transient)
    TObjectPtr<USkeletalMeshComponent> SourceBody;
    TSharedPtr<FHUMAClothGpuState, ESPMode::ThreadSafe> State;
};