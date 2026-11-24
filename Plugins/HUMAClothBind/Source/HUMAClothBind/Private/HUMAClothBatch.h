#pragma once

#include "HUMAClothGpuState.h"

struct FHUMAClothBatchRange
{
    uint32 FirstVertex = 0;
    uint32 VertexCount = 0;
};

inline bool BuildHUMAClothBatch(TConstArrayView<const FHUMAClothGpuState*> Parts,
    FHUMAClothGpuState& Output, TArray<FHUMAClothBatchRange>& Ranges, FString& Error)
{
    if (Parts.Num() < 2 || !Parts[0] || !Parts[0]->BodyTopology)
    { Error = TEXT("Batch requires at least two valid garment tables."); return false; }
    int64 Vertices = 0, Anchors = 0, Triangles = 0, Incidents = 0;
    for (const auto* Part : Parts)
    {
        if (!Part || Part->BodyTopology != Parts[0]->BodyTopology || Part->Slots != Parts[0]->Slots
            || Part->Rest.IsEmpty() || Part->Bary.IsEmpty() || Part->UV.Num() != Part->Rest.Num()
            || Part->AnchorTriangles.Num() != Part->Bary.Num() * 3 || Part->Triangles.Num() % 3
            || Part->RestPosition.Num() != Part->Bary.Num() || Part->RestTangent.Num() != Part->Bary.Num()
            || Part->RestBitangent.Num() != Part->Bary.Num() || Part->RestNormal.Num() != Part->Bary.Num()
            || Part->ClothOffsets.Num() != Part->Rest.Num() + 1
            || Part->ClothOffsets.Last() != uint32(Part->ClothIncidents.Num())
            || int64(Part->InfluenceAnchors.Num()) != int64(Part->Rest.Num()) * Part->Slots
            || Part->InfluenceWeights.Num() != Part->InfluenceAnchors.Num())
        { Error = TEXT("Garment tables cannot share this batch topology/layout."); return false; }
        Vertices += Part->Rest.Num(); Anchors += Part->Bary.Num();
        Triangles += Part->Triangles.Num() / 3; Incidents += Part->ClothIncidents.Num();
    }
    if (Vertices > MAX_int32 / (3 * sizeof(float)) || Anchors > MAX_int32 / (4 * sizeof(FVector3f))
        || Vertices * Parts[0]->Slots > MAX_int32 || Triangles > MAX_int32 / 3 || Incidents > MAX_int32)
    { Error = TEXT("Batched garment buffers exceed supported index/buffer limits."); return false; }
    Output.BodyTopology = Parts[0]->BodyTopology;
    Output.Slots = Parts[0]->Slots;
    Output.Rest.Reserve(Vertices); Output.UV.Reserve(Vertices);
    Output.Bary.Reserve(Anchors); Output.AnchorTriangles.Reserve(Anchors * 3);
    Output.RestPosition.Reserve(Anchors); Output.RestTangent.Reserve(Anchors);
    Output.RestBitangent.Reserve(Anchors); Output.RestNormal.Reserve(Anchors);
    Output.InfluenceAnchors.Reserve(Vertices * Output.Slots); Output.InfluenceWeights.Reserve(Vertices * Output.Slots);
    Output.Triangles.Reserve(Triangles * 3); Output.ClothOffsets.Reserve(Vertices + 1);
    Output.ClothIncidents.Reserve(Incidents);
    for (const auto* Part : Parts)
    {
        const uint32 VertexBase = Output.Rest.Num(), AnchorBase = Output.Bary.Num();
        const uint32 TriangleBase = Output.Triangles.Num() / 3, IncidentBase = Output.ClothIncidents.Num();
        Ranges.Add({VertexBase, uint32(Part->Rest.Num())});
        Output.Rest.Append(Part->Rest); Output.UV.Append(Part->UV);
        Output.AnchorTriangles.Append(Part->AnchorTriangles); Output.Bary.Append(Part->Bary);
        Output.RestPosition.Append(Part->RestPosition); Output.RestTangent.Append(Part->RestTangent);
        Output.RestBitangent.Append(Part->RestBitangent); Output.RestNormal.Append(Part->RestNormal);
        Output.InfluenceWeights.Append(Part->InfluenceWeights);
        for (const uint32 Anchor : Part->InfluenceAnchors)
        {
            if (Anchor >= uint32(Part->Bary.Num())) { Error = TEXT("Invalid batch anchor index."); return false; }
            Output.InfluenceAnchors.Add(Anchor + AnchorBase);
        }
        for (const uint32 Vertex : Part->Triangles)
        {
            if (Vertex >= uint32(Part->Rest.Num())) { Error = TEXT("Invalid batch triangle index."); return false; }
            Output.Triangles.Add(Vertex + VertexBase);
        }
        for (int32 Vertex = 0; Vertex < Part->Rest.Num(); ++Vertex)
        {
            if (Part->ClothOffsets[Vertex] > Part->ClothOffsets[Vertex + 1])
            { Error = TEXT("Invalid batch incidence offsets."); return false; }
            Output.ClothOffsets.Add(Part->ClothOffsets[Vertex] + IncidentBase);
        }
        for (const uint32 Triangle : Part->ClothIncidents)
        {
            if (Triangle >= uint32(Part->Triangles.Num() / 3)) { Error = TEXT("Invalid batch incident triangle."); return false; }
            Output.ClothIncidents.Add(Triangle + TriangleBase);
        }
    }
    Output.ClothOffsets.Add(Output.ClothIncidents.Num());
    return true;
}
