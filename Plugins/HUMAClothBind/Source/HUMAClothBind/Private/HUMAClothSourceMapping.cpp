#include "HUMAClothSourceMapping.h"

#include "Async/ParallelFor.h"
#include "Distance/DistPoint3Triangle3.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "Engine/SkeletalMesh.h"
#include "MeshQueries.h"
#include "Rendering/SkeletalMeshRenderData.h"

bool FHUMAClothLODTransfer::Evaluate(TConstArrayView<FVector3f> Source, TArray<FVector3f>& Body, FString& Error) const
{
    Error.Reset();
    if (Source.Num() != SourceVertexCount || Triangles.IsEmpty() || Barycentrics.Num() != Triangles.Num())
    { Error = TEXT("LOD transfer input size does not match the prepared source LOD."); return false; }
    Body.SetNumUninitialized(Triangles.Num());
    for (int32 Index = 0; Index < Triangles.Num(); ++Index)
    {
        const FIntVector Triangle = Triangles[Index];
        const FVector3f Weights = Barycentrics[Index];
        if (Triangle.X < 0 || Triangle.Y < 0 || Triangle.Z < 0 || Triangle.X >= Source.Num()
            || Triangle.Y >= Source.Num() || Triangle.Z >= Source.Num() || Weights.ContainsNaN()
            || Weights.GetMin() < 0 || !FMath::IsNearlyEqual(Weights.X + Weights.Y + Weights.Z, 1.f, 1.e-5f))
        { Error = TEXT("Invalid prepared source LOD triangle/weights."); return false; }
        Body[Index] = Source[Triangle.X] * Weights.X + Source[Triangle.Y] * Weights.Y + Source[Triangle.Z] * Weights.Z;
        if (Body[Index].ContainsNaN()) { Error = TEXT("Source LOD transfer produced a non-finite body position."); return false; }
    }
    return true;
}

bool BuildHUMAClothLODTransfer(USkeletalMesh* Mesh, int32 LODIndex, TConstArrayView<int32> CanonicalToRender,
    TSharedPtr<const FHUMAClothLODTransfer, ESPMode::ThreadSafe>& Transfer, FString& Error)
{
    using namespace UE::Geometry;
    Transfer.Reset();
    Error.Reset();
    const auto* Data = Mesh ? Mesh->GetResourceForRendering() : nullptr;
    auto Fail = [&](const FString& Reason)
    {
        Error = FString::Printf(TEXT("Source %s LOD%d transfer: %s"),
            Mesh ? *Mesh->GetPathName() : TEXT("None"), LODIndex, *Reason);
        return false;
    };
    if (!Data || LODIndex <= 0 || !Data->LODRenderData.IsValidIndex(LODIndex) || CanonicalToRender.IsEmpty())
        return Fail(TEXT("missing source LOD or canonical mapping."));
    const auto& Root = Data->LODRenderData[0];
    const auto& LOD = Data->LODRenderData[LODIndex];
    const auto* RootIndices = Root.MultiSizeIndexContainer.GetIndexBuffer();
    const auto* Indices = LOD.MultiSizeIndexContainer.GetIndexBuffer();
    const auto& RootUV = Root.StaticVertexBuffers.StaticMeshVertexBuffer;
    const auto& RootPositions = Root.StaticVertexBuffers.PositionVertexBuffer;
    const auto& Positions = LOD.StaticVertexBuffers.PositionVertexBuffer;
    if (!RootIndices || !Indices || !RootPositions.GetVertexData() || !Positions.GetVertexData()
        || !RootUV.GetTexCoordData() || RootUV.GetNumTexCoords() < 2 || !RootUV.GetUseFullPrecisionUVs())
        return Fail(TEXT("CPU-accessible reference positions/indices and exact LOD0 UV1 are required; enable mesh CPU access before cooking."));

    TMap<int32, uint8> MaterialDomains;
    for (const auto& Section : Root.RenderSections)
    {
        if (uint64(Section.BaseIndex) + uint64(Section.NumTriangles) * 3 > RootIndices->Num())
            return Fail(TEXT("invalid LOD0 section index range."));
        uint8& Domain = MaterialDomains.FindOrAdd(Section.MaterialIndex);
        for (uint32 Index = Section.BaseIndex; Index < Section.BaseIndex + Section.NumTriangles * 3; Index += 3)
        {
            bool Body = true;
            for (int32 Corner = 0; Corner < 3; ++Corner)
            {
                const uint32 Render = RootIndices->Get(Index + Corner);
                if (Render >= Root.GetNumVertices()) return Fail(TEXT("invalid LOD0 render index."));
                const float ID = RootUV.GetVertexUV(Render, 1).X;
                Body &= ID >= 0 && ID < CanonicalToRender.Num();
            }
            Domain |= Body ? 1 : 2;
        }
    }
    for (const auto& Domain : MaterialDomains)
        if (Domain.Value == 3)
            return Fail(FString::Printf(TEXT("section material slot %d mixes canonical body and auxiliary surfaces; unambiguous body sections are required."), Domain.Key));

    FDynamicMesh3 Surface;
    for (uint32 Index = 0; Index < LOD.GetNumVertices(); ++Index)
    {
        const FVector3d Point(Positions.VertexPosition(Index));
        if (Point.ContainsNaN()) return Fail(TEXT("non-finite reference position."));
        Surface.AppendVertex(Point);
    }
    for (const auto& Section : LOD.RenderSections)
    {
        const uint8* Domain = MaterialDomains.Find(Section.MaterialIndex);
        if (!Domain) return Fail(TEXT("source LOD section has no corresponding LOD0 material domain."));
        if (*Domain != 1) continue;
        if (uint64(Section.BaseIndex) + uint64(Section.NumTriangles) * 3 > Indices->Num())
            return Fail(TEXT("invalid source section index range."));
        for (uint32 Index = Section.BaseIndex; Index < Section.BaseIndex + Section.NumTriangles * 3; Index += 3)
        {
            const FIndex3i Triangle(Indices->Get(Index), Indices->Get(Index + 1), Indices->Get(Index + 2));
            if (!Surface.IsVertex(Triangle.A) || !Surface.IsVertex(Triangle.B) || !Surface.IsVertex(Triangle.C))
                return Fail(TEXT("invalid source render triangle."));
            if (Triangle.A == Triangle.B || Triangle.A == Triangle.C || Triangle.B == Triangle.C) continue;
            if (FVector3d::CrossProduct(Surface.GetVertex(Triangle.B) - Surface.GetVertex(Triangle.A),
                Surface.GetVertex(Triangle.C) - Surface.GetVertex(Triangle.A)).SquaredLength() <= 1.e-20) continue;
            if (Surface.AppendTriangle(Triangle) < 0) return Fail(TEXT("source body triangles are not manifold."));
        }
    }
    if (Surface.TriangleCount() == 0) return Fail(TEXT("source LOD has no canonical body surface."));
    for (int32 Render : CanonicalToRender)
        if (Render < 0 || uint32(Render) >= Root.GetNumVertices()) return Fail(TEXT("invalid LOD0 canonical mapping."));
    FDynamicMeshAABBTree3 Tree(&Surface, true);
    auto Result = MakeShared<FHUMAClothLODTransfer, ESPMode::ThreadSafe>();
    Result->SourceVertexCount = LOD.GetNumVertices();
    Result->Triangles.SetNumUninitialized(CanonicalToRender.Num());
    Result->Barycentrics.SetNumUninitialized(CanonicalToRender.Num());
    TArray<double> SquaredDistances;
    SquaredDistances.SetNumUninitialized(CanonicalToRender.Num());
    ParallelFor(CanonicalToRender.Num(), [&](int32 Index)
    {
        const FVector3d Point(RootPositions.VertexPosition(CanonicalToRender[Index]));
        const int32 TriangleID = Tree.FindNearestTriangle(Point, SquaredDistances[Index]);
        if (TriangleID < 0)
        { Result->Triangles[Index] = FIntVector(INDEX_NONE); return; }
        auto Query = TMeshQueries<FDynamicMesh3>::TriangleDistance(Surface, TriangleID, Point);
        Query.GetSquared();
        const auto Triangle = Surface.GetTriangle(TriangleID);
        Result->Triangles[Index] = FIntVector(Triangle.A, Triangle.B, Triangle.C);
        FVector3f Weights(Query.TriangleBaryCoords);
        Weights.X = FMath::Max(0.f, Weights.X); Weights.Y = FMath::Max(0.f, Weights.Y); Weights.Z = FMath::Max(0.f, Weights.Z);
        Result->Barycentrics[Index] = Weights / (Weights.X + Weights.Y + Weights.Z);
    });
    double SumSquared = 0;
    for (int32 Index = 0; Index < CanonicalToRender.Num(); ++Index)
    {
        if (Result->Triangles[Index].X < 0 || Result->Barycentrics[Index].ContainsNaN()
            || !FMath::IsFinite(SquaredDistances[Index])) return Fail(TEXT("nearest body surface query failed."));
        SumSquared += SquaredDistances[Index];
        Result->MaxRestDistanceCm = FMath::Max(Result->MaxRestDistanceCm, FMath::Sqrt(SquaredDistances[Index]));
    }
    Result->RmsRestDistanceCm = FMath::Sqrt(SumSquared / CanonicalToRender.Num());
    Transfer = MoveTemp(Result);
    return true;
}
