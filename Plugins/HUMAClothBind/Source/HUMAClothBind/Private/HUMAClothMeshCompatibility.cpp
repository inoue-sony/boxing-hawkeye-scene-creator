#include "HUMAClothMeshCompatibility.h"
#include "HUMAClothBindAsset.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"
#if WITH_EDITOR
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#endif

namespace
{
bool ReadSurface(USkeletalMesh* Mesh, int32 Count, HUMACloth::FCanonicalSurface& Surface, FString& Error)
{
    const auto* Data = Mesh ? Mesh->GetResourceForRendering() : nullptr;
    if (!Data || Data->LODRenderData.IsEmpty())
    { Error = TEXT("Body mesh has no LOD0 render data."); return false; }
    const auto& LOD = Data->LODRenderData[0];
    const auto& Vertices = LOD.StaticVertexBuffers.StaticMeshVertexBuffer;
    const auto& Positions = LOD.StaticVertexBuffers.PositionVertexBuffer;
    const auto* Buffer = LOD.MultiSizeIndexContainer.GetIndexBuffer();
    if (Vertices.GetNumTexCoords() < 2 || !Vertices.GetUseFullPrecisionUVs())
    { Error = TEXT("Compatible body meshes need exact canonical IDs in Full Precision UV1."); return false; }
    if (!Vertices.GetTexCoordData() || !Positions.GetVertexData() || !Buffer || Buffer->Num() == 0)
    { Error = TEXT("Mesh compatibility requires CPU-accessible LOD0 positions, UVs and indices. Enable CPU access before cooking."); return false; }
    const TArray<int32>* ImportMap = nullptr;
#if WITH_EDITOR
    if (const auto* Model = Mesh->GetImportedModel(); Model && !Model->LODModels.IsEmpty() &&
        Model->LODModels[0].MeshToImportVertexMap.Num() == int32(LOD.GetNumVertices()))
        ImportMap = &Model->LODModels[0].MeshToImportVertexMap;
#endif
    TArray<int32> IDs, Triangles;
    TArray<FVector3f> Points;
    for (uint32 Render = 0; Render < LOD.GetNumVertices(); ++Render)
    {
        const float Encoded = Vertices.GetVertexUV(Render, 1).X;
        if (!FMath::IsFinite(Encoded) || FMath::Abs(Encoded) > 1.e7f)
        { Error = TEXT("Invalid canonical ID in body UV1."); return false; }
        int32 ID = FMath::RoundToInt(Encoded);
        if (Encoded < 0) ID = ImportMap ? (*ImportMap)[Render] : INDEX_NONE;
        else if (!FMath::IsNearlyEqual(Encoded, float(ID), 1.e-3f))
        { Error = TEXT("Body canonical IDs must be exact integers."); return false; }
        IDs.Add(ID); Points.Add(Positions.VertexPosition(Render));
    }
    for (const auto& Section : LOD.RenderSections)
        for (uint32 Index = Section.BaseIndex; Index < Section.BaseIndex + Section.NumTriangles * 3; ++Index)
            Triangles.Add(Buffer->Get(Index));
    return HUMACloth::BuildCanonicalSurface(IDs, Points, Triangles, Count, Surface, Error);
}
}

bool UHUMAClothBindAsset::BuildSourceMapping(USkeletalMesh* Mesh, TArray<int32>& Mapping, FString& Error) const
{
    Mapping.Reset();
    if (!Mesh || !BodyMesh) { Error = TEXT("Select both a baked body mesh and a target body mesh."); return false; }
    if (Mesh == BodyMesh) { Mapping = CanonicalToRender; return true; }
    if (BodyLODIndex != 0) { Error = TEXT("Cross-mesh reuse requires the LOD0 Bind asset."); return false; }
    HUMACloth::FCanonicalSurface Reference, Target;
    if (!ReadSurface(BodyMesh, CanonicalToRender.Num(), Reference, Error) ||
        !ReadSurface(Mesh, CanonicalToRender.Num(), Target, Error)) return false;
    if (!HUMACloth::HaveCompatibleCanonicalTopology(Reference, Target, BodyQuads, Error)) return false;
    Mapping = MoveTemp(Target.Mapping);
    return true;
}

FString UHUMAClothBindAsset::GetBodyMeshCompatibilityError(USkeletalMesh* Mesh) const
{
    TArray<int32> Mapping;
    FString Error;
    if (!IsBakeCurrent(Error) || !BuildSourceMapping(Mesh, Mapping, Error)) return Error;
    return FString();
}

bool UHUMAClothBindAsset::BuildSourceLODs(USkeletalMesh* Mesh, FHUMAClothSourceLODs& LODs, FString& Error) const
{
    LODs.Reset();
    Error.Reset();
    TArray<int32> RootMapping;
    if (!BuildSourceMapping(Mesh, RootMapping, Error)) return false;
    FHUMAClothSourceLOD& Root = LODs.Add(BodyLODIndex);
    Root.BindBodyLOD = BodyLODIndex;
    Root.Mapping = RootMapping;
    if (Mesh == BodyMesh)
    {
        for (const auto& Entry : AdditionalBodyLODs)
        {
            if (!Entry) { Error = TEXT("Missing baked body LOD table."); LODs.Reset(); return false; }
            auto& LOD = LODs.Add(Entry->BodyLODIndex);
            LOD.BindBodyLOD = Entry->BodyLODIndex;
            LOD.Mapping = Entry->CanonicalToRender;
        }
        return true;
    }
    const auto* Data = Mesh->GetResourceForRendering();
    for (int32 Index = 1; Index < Data->LODRenderData.Num(); ++Index)
    {
        FHUMAClothSourceLOD LOD;
        LOD.BindBodyLOD = BodyLODIndex;
        if (!BuildHUMAClothLODTransfer(Mesh, Index, RootMapping, LOD.Transfer, Error))
        { LODs.Reset(); return false; }
        LOD.Mapping.SetNumUninitialized(RootMapping.Num());
        for (int32 Vertex = 0; Vertex < LOD.Mapping.Num(); ++Vertex) LOD.Mapping[Vertex] = Vertex;
        LODs.Add(Index, MoveTemp(LOD));
    }
    return true;
}
