#pragma once

#include "CoreMinimal.h"

class USkeletalMesh;
struct FHUMAClothSourceTransferBuffers;

struct HUMACLOTHBIND_API FHUMAClothLODTransfer
{
    int32 SourceVertexCount = 0;
    TArray<FIntVector> Triangles;
    TArray<FVector3f> Barycentrics;
    double MaxRestDistanceCm = 0;
    double RmsRestDistanceCm = 0;
    mutable TSharedPtr<FHUMAClothSourceTransferBuffers, ESPMode::ThreadSafe> GPU;

    bool Evaluate(TConstArrayView<FVector3f> Source, TArray<FVector3f>& Body, FString& Error) const;
};

struct FHUMAClothSourceLOD
{
    int32 BindBodyLOD = 0;
    TArray<int32> Mapping;
    TSharedPtr<const FHUMAClothLODTransfer, ESPMode::ThreadSafe> Transfer;
};

using FHUMAClothSourceLODs = TMap<int32, FHUMAClothSourceLOD>;

bool BuildHUMAClothLODTransfer(USkeletalMesh* Mesh, int32 LODIndex, TConstArrayView<int32> CanonicalToRender,
    TSharedPtr<const FHUMAClothLODTransfer, ESPMode::ThreadSafe>& Transfer, FString& Error);
