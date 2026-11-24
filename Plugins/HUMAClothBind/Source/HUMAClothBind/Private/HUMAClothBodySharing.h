#pragma once

#include "CoreMinimal.h"
#include "Misc/Crc.h"
#include "PrimitiveComponentId.h"
#include "RenderGraphResources.h"

class FSceneInterface;

struct FHUMAClothBodyTopology
{
    TArray<uint32> Mapping, Quads, Offsets, Incidents;
    uint32 Signature = 0;
    mutable TRefCountPtr<FRDGPooledBuffer> Immutable[4];

    void UpdateSignature()
    {
        Signature = 0;
        for (const auto* Values : {&Mapping, &Quads, &Offsets, &Incidents})
        {
            Signature = HashCombineFast(Signature, GetTypeHash(Values->Num()));
            Signature = HashCombineFast(Signature, FCrc::MemCrc32(Values->GetData(), Values->Num() * sizeof(uint32)));
        }
    }

    bool IsCompatible(const FHUMAClothBodyTopology& Other) const
    {
        return Signature == Other.Signature && Mapping == Other.Mapping && Quads == Other.Quads
            && Offsets == Other.Offsets && Incidents == Other.Incidents;
    }
};

struct FHUMAClothBodyNormalKey
{
    const FSceneInterface* Scene = nullptr;
    FPrimitiveComponentId PrimitiveId;
    int32 LOD = INDEX_NONE;
    // Opaque render resource identities, never dereferenced by the sharing key.
    const void* RenderMesh = nullptr;
    const void* Positions = nullptr;
    const FHUMAClothBodyTopology* Topology = nullptr;

    bool operator==(const FHUMAClothBodyNormalKey& Other) const
    {
        return Scene == Other.Scene && PrimitiveId == Other.PrimitiveId && LOD == Other.LOD
            && RenderMesh == Other.RenderMesh && Positions == Other.Positions && Topology == Other.Topology;
    }

    friend uint32 GetTypeHash(const FHUMAClothBodyNormalKey& Key)
    {
        uint32 Hash = HashCombineFast(PointerHash(Key.Scene), GetTypeHash(Key.PrimitiveId));
        Hash = HashCombineFast(Hash, GetTypeHash(Key.LOD));
        Hash = HashCombineFast(Hash, PointerHash(Key.RenderMesh));
        Hash = HashCombineFast(Hash, PointerHash(Key.Positions));
        return HashCombineFast(Hash, PointerHash(Key.Topology));
    }
};
