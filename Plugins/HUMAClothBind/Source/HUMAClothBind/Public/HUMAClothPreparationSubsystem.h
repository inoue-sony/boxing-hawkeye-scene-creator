#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "HUMAClothSourceMapping.h"
#include "HUMAClothPreparationSubsystem.generated.h"

class UHUMAClothBindAsset;
class USkeletalMesh;

/** Game-world lifetime cache of validated, immutable source mapping tables. */
UCLASS()
class HUMACLOTHBIND_API UHUMAClothPreparationSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    bool Prepare(UHUMAClothBindAsset* Asset, USkeletalMesh* Mesh, TArray<int32>& Mapping, FString& Error);
    bool PrepareLODs(UHUMAClothBindAsset* Asset, USkeletalMesh* Mesh, FHUMAClothSourceLODs& LODs, FString& Error);

    UFUNCTION(BlueprintPure, Category="HUMA Cloth Bind|Preparation", meta=(WorldContext="Context"))
    static UHUMAClothPreparationSubsystem* GetForWorld(UObject* Context);

    UFUNCTION(BlueprintCallable, Category="HUMA Cloth Bind|Preparation")
    void InvalidatePreparation();

    static void InvalidateAssetEverywhere(const UObject* Object);

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="HUMA Cloth Bind|Preparation")
    int32 CacheHits = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="HUMA Cloth Bind|Preparation")
    int32 CacheMisses = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="HUMA Cloth Bind|Preparation")
    double ValidationMilliseconds = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="HUMA Cloth Bind|Preparation")
    double MappingMilliseconds = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="HUMA Cloth Bind|Preparation")
    int32 SharedMappingReuses = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="HUMA Cloth Bind|Preparation")
    int32 SharedLODTransferReuses = 0;

private:
    struct FEntry
    {
        TWeakObjectPtr<UHUMAClothBindAsset> Asset;
        TWeakObjectPtr<USkeletalMesh> Mesh;
        TArray<int32> Mapping;
        FHUMAClothSourceLODs LODs;
    };
    TArray<FEntry> Entries;
    void InvalidateSource(const UObject* Object);
#if WITH_EDITOR
    FDelegateHandle ModifiedHandle;
    FDelegateHandle PropertyHandle;
    FDelegateHandle TransactionHandle;
#endif
};
