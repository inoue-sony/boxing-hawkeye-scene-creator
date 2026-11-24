#pragma once

#include "HUMAClothSourceMapping.h"
#include "PrimitiveComponentId.h"
#include "RenderGraphResources.h"

struct FHUMAClothTransferKey
{
    FPrimitiveComponentId Source;
    const FHUMAClothLODTransfer* Transfer = nullptr;
    const void* Positions = nullptr;

    bool operator==(const FHUMAClothTransferKey& Other) const
    { return Source == Other.Source && Transfer == Other.Transfer && Positions == Other.Positions; }
    friend uint32 GetTypeHash(const FHUMAClothTransferKey& Key)
    { return HashCombineFast(GetTypeHash(Key.Source), HashCombineFast(PointerHash(Key.Transfer), PointerHash(Key.Positions))); }
};

using FHUMAClothTransferInputs = TMap<FHUMAClothTransferKey, FRDGBufferRef>;

FRDGBufferRef ResolveHUMAClothSourceLOD(FRDGBuilder& Graph, FRDGBufferRef Positions,
    const FHUMAClothLODTransfer* Transfer, FPrimitiveComponentId Source, ERHIFeatureLevel::Type FeatureLevel,
    FHUMAClothTransferInputs& Inputs);
