#pragma once

#include "CoreMinimal.h"

namespace HUMACloth
{
struct FCanonicalSurface
{
    TArray<int32> Mapping;
    TArray<FIntVector> Faces;
};

inline FIntVector CanonicalizeFace(FIntVector Face)
{
    if (Face.Y < Face.X && Face.Y < Face.Z) return FIntVector(Face.Y, Face.Z, Face.X);
    if (Face.Z < Face.X && Face.Z < Face.Y) return FIntVector(Face.Z, Face.X, Face.Y);
    return Face;
}

inline bool BuildCanonicalSurface(TConstArrayView<int32> IDs, TConstArrayView<FVector3f> Positions,
    TConstArrayView<int32> Triangles, int32 Count, FCanonicalSurface& Surface, FString& Error)
{
    Surface.Mapping.Init(INDEX_NONE, Count);
    Surface.Faces.Reset();
    if (IDs.Num() != Positions.Num() || Triangles.Num() % 3 != 0)
    { Error = TEXT("Invalid canonical surface arrays."); return false; }
    for (int32 Render = 0; Render < IDs.Num(); ++Render)
    {
        const int32 ID = IDs[Render];
        if (ID < 0 || ID >= Count) continue;
        if (Positions[Render].ContainsNaN())
        { Error = TEXT("Non-finite canonical body position."); return false; }
        const int32 Previous = Surface.Mapping[ID];
        if (Previous != INDEX_NONE && !Positions[Render].Equals(Positions[Previous], 1.e-3f))
        { Error = TEXT("Canonical ID maps to inconsistent body positions."); return false; }
        Surface.Mapping[ID] = Render;
    }
    if (Surface.Mapping.Contains(INDEX_NONE))
    { Error = TEXT("Mesh is missing HUMA fullBody canonical IDs in UV1."); return false; }
    for (int32 Offset = 0; Offset < Triangles.Num(); Offset += 3)
    {
        FIntVector Face;
        for (int32 Corner = 0; Corner < 3; ++Corner)
        {
            const int32 Render = Triangles[Offset + Corner];
            if (!IDs.IsValidIndex(Render)) { Error = TEXT("Invalid body render triangle."); return false; }
            Face[Corner] = IDs[Render];
        }
        if (Face.X < 0 || Face.Y < 0 || Face.Z < 0 || Face.X >= Count || Face.Y >= Count || Face.Z >= Count) continue;
        if (Face.X == Face.Y || Face.Y == Face.Z || Face.X == Face.Z)
        { Error = TEXT("Degenerate canonical body render triangle."); return false; }
        Surface.Faces.Add(CanonicalizeFace(Face));
    }
    Surface.Faces.Sort([](const FIntVector& A, const FIntVector& B)
    {
        return A.X != B.X ? A.X < B.X : A.Y != B.Y ? A.Y < B.Y : A.Z < B.Z;
    });
    if (Surface.Faces.IsEmpty()) { Error = TEXT("Canonical body surface has no triangles."); return false; }
    return true;
}

inline bool BuildCanonicalFaceWinding(const FCanonicalSurface& Surface, TConstArrayView<int32> BodyQuads,
    TArray<int8>& Winding, FString& Error)
{
    Winding.Reset();
    const int32 Count = Surface.Mapping.Num();
    if (Count == 0 || BodyQuads.IsEmpty() || BodyQuads.Num() % 4 != 0)
    { Error = TEXT("Stored canonical body faces are missing or invalid."); return false; }
    TSet<FIntVector> Faces;
    Faces.Reserve(Surface.Faces.Num());
    for (const FIntVector& Face : Surface.Faces)
    {
        if (Face.X < 0 || Face.Y < 0 || Face.Z < 0 || Face.X >= Count || Face.Y >= Count || Face.Z >= Count ||
            Face.X == Face.Y || Face.Y == Face.Z || Face.X == Face.Z)
        { Error = TEXT("Invalid canonical body triangle."); return false; }
        const FIntVector Key = CanonicalizeFace(Face);
        if (Faces.Contains(Key))
        { Error = TEXT("Duplicate canonical body triangle."); return false; }
        Faces.Add(Key);
    }
    TSet<FIntVector> Remaining = Faces;
    Winding.Reserve(BodyQuads.Num() / 4);
    for (int32 Offset = 0; Offset < BodyQuads.Num(); Offset += 4)
    {
        int32 Corners[4], NumCorners = 0;
        for (int32 Corner = 0; Corner < 4; ++Corner)
        {
            const int32 ID = BodyQuads[Offset + Corner];
            if (ID < 0 || ID >= Count)
            { Error = TEXT("Stored body face has an out-of-range canonical ID."); return false; }
            if (NumCorners == 0 || Corners[NumCorners - 1] != ID) Corners[NumCorners++] = ID;
        }
        // Only adjacent repeated corners (including the closing edge) can pad a triangle.
        if (NumCorners > 1 && Corners[0] == Corners[NumCorners - 1]) --NumCorners;
        if (NumCorners < 3)
        { Error = TEXT("Stored body face has fewer than three distinct corners."); return false; }
        for (int32 Corner = 0; Corner < NumCorners; ++Corner)
            for (int32 Previous = 0; Previous < Corner; ++Previous)
                if (Corners[Corner] == Corners[Previous])
                { Error = TEXT("Stored body face has a non-adjacent repeated canonical ID."); return false; }

        const int32 NumTriangles = NumCorners - 2;
        FIntVector Match[2];
        int8 MatchWinding = 0;
        int32 Matches = 0;
        for (int32 Diagonal = 0; Diagonal < (NumCorners == 4 ? 2 : 1); ++Diagonal)
            for (int8 Sign : {int8(1), int8(-1)})
            {
                FIntVector Candidate[2];
                Candidate[0] = FIntVector(Corners[0], Corners[1], Corners[Diagonal ? 3 : 2]);
                if (NumTriangles == 2)
                    Candidate[1] = Diagonal ? FIntVector(Corners[1], Corners[2], Corners[3]) :
                        FIntVector(Corners[0], Corners[2], Corners[3]);
                for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle)
                {
                    if (Sign < 0) Swap(Candidate[Triangle].Y, Candidate[Triangle].Z);
                    Candidate[Triangle] = CanonicalizeFace(Candidate[Triangle]);
                }
                if (!Faces.Contains(Candidate[0]) || (NumTriangles == 2 && !Faces.Contains(Candidate[1]))) continue;
                ++Matches;
                MatchWinding = Sign;
                for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle) Match[Triangle] = Candidate[Triangle];
            }
        if (Matches != 1)
        {
            Error = FString::Printf(TEXT("Stored body face %d is not uniquely covered by consistently wound canonical triangles."), Offset / 4);
            return false;
        }
        for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle)
            if (Remaining.Remove(Match[Triangle]) != 1)
            { Error = TEXT("Stored body faces reuse a canonical triangle."); return false; }
        Winding.Add(MatchWinding);
    }
    if (!Remaining.IsEmpty())
    { Error = TEXT("Body LOD0 has canonical triangles outside the stored body faces."); return false; }
    return true;
}

inline bool HaveCompatibleCanonicalTopology(const FCanonicalSurface& Reference, const FCanonicalSurface& Target,
    TConstArrayView<int32> BodyQuads, FString& Error)
{
    if (Reference.Mapping.Num() != Target.Mapping.Num())
    { Error = TEXT("Body canonical ID counts differ."); return false; }
    TArray<int8> ReferenceWinding, TargetWinding;
    if (!BuildCanonicalFaceWinding(Reference, BodyQuads, ReferenceWinding, Error))
    { Error = TEXT("Baked body LOD0: ") + Error; return false; }
    if (!BuildCanonicalFaceWinding(Target, BodyQuads, TargetWinding, Error))
    { Error = TEXT("Target body LOD0: ") + Error; return false; }
    if (ReferenceWinding != TargetWinding)
    { Error = TEXT("Target LOD0 winding differs from the baked body in canonical ID space. Re-bake for this body."); return false; }
    return true;
}
}
