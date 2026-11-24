#pragma once

#include "CoreMinimal.h"
#include "HUMAClothBindAsset.h"
#include "UObject/StrongObjectPtr.h"

struct FHUMAClothBatchItem
{
    TStrongObjectPtr<UObject> Garment;
    TStrongObjectPtr<UHUMAClothBindAsset> Existing;
    FString PackageName;
};

struct FHUMAClothBatchPlan
{
    TStrongObjectPtr<UHUMAClothBindAsset> Settings;
    TArray<FHUMAClothBatchItem> Items;
    int32 ExistingCount = 0;
};

struct FHUMAClothBatchResult
{
    int32 Requested = 0;
    TArray<UHUMAClothBindAsset*> SavedAssets;
    TArray<FString> Errors;
    bool bCancelled = false;
    bool IsSuccess() const { return Requested > 0 && SavedAssets.Num() == Requested && Errors.IsEmpty() && !bCancelled; }
};

bool PlanHUMAClothBatch(const UHUMAClothBindAsset& Settings, TConstArrayView<UObject*> Selection,
    const FString& OutputFolder, FHUMAClothBatchPlan& Plan, FString& Error);
FHUMAClothBatchResult BakeHUMAClothBatch(const FHUMAClothBatchPlan& Plan, bool bAllowOverwrite,
    TFunction<bool()> ShouldCancel = {});
