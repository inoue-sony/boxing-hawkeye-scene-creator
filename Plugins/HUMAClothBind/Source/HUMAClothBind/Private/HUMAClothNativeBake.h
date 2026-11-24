#pragma once

#include "CoreMinimal.h"

class UHUMAClothBindAsset;

#if WITH_EDITOR
bool BakeHUMANativeLOD(UHUMAClothBindAsset& Output, TConstArrayView<int32> BodyTriangles,
    TConstArrayView<int32> ClothVertexIDs, float SigmaMin, float DistanceScale, FString& Error,
    const UHUMAClothBindAsset* SharedWeights = nullptr);
#endif