#include "HUMAClothBindAsset.h"
#include "Serialization/CustomVersion.h"

namespace
{
const FGuid HUMAClothAssetVersion(0x43D5B087, 0xD35740EB, 0xB962CFE1, 0xAA90CF38);
FCustomVersionRegistration HUMAClothVersionRegistration(HUMAClothAssetVersion, 1, TEXT("HUMAClothAssetVersion"));
}

void UHUMAClothBindAsset::Serialize(FArchive& Ar)
{
    Ar.UsingCustomVersion(HUMAClothAssetVersion);
    // Older packages may omit SigmaMin because 2cm was their class default.
    if (Ar.IsLoading() && Ar.IsPersistent() && !HasAnyFlags(RF_ClassDefaultObject)
        && Ar.CustomVer(HUMAClothAssetVersion) < 1)
        SigmaMin = 2.0f;
    Super::Serialize(Ar);
}

FString UHUMAClothBindAsset::GetRequestedBindMethod() const
{
    switch (BakeMethod)
    {
    case EHUMAClothBindMethod::BodyField: return TEXT("body_field");
    case EHUMAClothBindMethod::ClothGraph: return TEXT("cloth_graph");
    default: return TEXT("unsupported");
    }
}

void UHUMAClothBindAsset::NormalizeBindMethod()
{
    if (BindMethod == TEXT("body_field_v1")) BindMethod = TEXT("body_field");
    else if (BindMethod == TEXT("garment_graph_v1")) BindMethod = TEXT("cloth_graph");
}

void UHUMAClothBindAsset::PostLoad()
{
    Super::PostLoad();
    NormalizeBindMethod();
    if (!bBakeMethodInitialized)
    {
        if (FormatVersion > 0)
            BakeMethod = BindMethod == TEXT("cloth_graph") ? EHUMAClothBindMethod::ClothGraph : EHUMAClothBindMethod::BodyField;
        bBakeMethodInitialized = true;
    }
}

#if WITH_EDITOR
void UHUMAClothBindAsset::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
    bBakeMethodInitialized = true;
    Super::PostEditChangeProperty(Event);
    if (Event.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UHUMAClothBindAsset, BakeMethod))
        BakeStatus = TEXT("Method changed. Click Bake to rebuild; existing tables have not changed.");
}
#endif

const UHUMAClothBindAsset* UHUMAClothBindAsset::GetBodyLODBind(int32 LODIndex) const
{
    if (LODIndex == BodyLODIndex) return this;
    for (const auto& Entry : AdditionalBodyLODs)
        if (Entry && Entry->BodyLODIndex == LODIndex) return Entry;
    return nullptr;
}

bool UHUMAClothBindAsset::Validate(FString& Error) const
{
    const int32 Count = ClothRestPositions.Num();
    const int32 Anchors = AnchorTriangles.Num();
    if (BindMethod != TEXT("body_field") && BindMethod != TEXT("cloth_graph"))
    { Error = TEXT("Unsupported Bind method."); return false; }
    if ((FormatVersion != 1 && FormatVersion != 2) || Count == 0 || Anchors == 0 || Slots < 1 || Slots > 32 ||
        CanonicalToRender.IsEmpty() || BodyQuads.Num() % 4 != 0 || BodyQuads.IsEmpty() ||
        AnchorBarycentrics.Num() != Anchors || AnchorRestPositions.Num() != Anchors ||
        AnchorRestTangents.Num() != Anchors || AnchorRestBitangents.Num() != Anchors || AnchorRestNormals.Num() != Anchors ||
        InfluenceAnchors.Num() != Count * Slots || InfluenceWeights.Num() != Count * Slots ||
        ClothUVs.Num() != Count || ClothTriangles.Num() % 3 != 0 || ClothTriangles.IsEmpty())
    {
        Error = TEXT("Missing or incompatible bake data. Bake this asset first.");
        return false;
    }
    for (int32 Index : BodyQuads)
    {
        if (!CanonicalToRender.IsValidIndex(Index)) { Error = TEXT("Invalid body topology."); return false; }
    }
    for (const FIntVector& Triangle : AnchorTriangles)
    {
        if (!CanonicalToRender.IsValidIndex(Triangle.X) || !CanonicalToRender.IsValidIndex(Triangle.Y) || !CanonicalToRender.IsValidIndex(Triangle.Z))
        { Error = TEXT("Invalid anchor triangle."); return false; }
    }
    for (int32 Index : ClothTriangles)
    {
        if (!ClothRestPositions.IsValidIndex(Index)) { Error = TEXT("Invalid garment topology."); return false; }
    }
    for (int32 Vertex = 0; Vertex < Count; ++Vertex)
    {
        float Sum = 0;
        for (int32 Slot = 0; Slot < Slots; ++Slot)
        {
            const int32 Index = Vertex * Slots + Slot;
            const float Weight = InfluenceWeights[Index];
            if (!AnchorTriangles.IsValidIndex(InfluenceAnchors[Index]) || !FMath::IsFinite(Weight) || Weight < 0)
            { Error = TEXT("Invalid SoftBind influence."); return false; }
            Sum += Weight;
        }
        if (!FMath::IsNearlyEqual(Sum, 1.0f, 1.e-4f)) { Error = TEXT("SoftBind weights must sum to one."); return false; }
    }
    TSet<int32> Seen;
    Seen.Add(BodyLODIndex);
    for (const auto& Entry : AdditionalBodyLODs)
    {
        if (!Entry || Seen.Contains(Entry->BodyLODIndex) || !Entry->AdditionalBodyLODs.IsEmpty() || !Entry->Validate(Error))
        { if (Error.IsEmpty()) Error = TEXT("Invalid body LOD table."); return false; }
        if (Entry->BindMethod != BindMethod || Entry->bCanonicalBodyTopology != bCanonicalBodyTopology || Entry->AnchorTriangles.Num() != Anchors ||
            Entry->ClothRestPositions != ClothRestPositions || Entry->ClothTriangles != ClothTriangles ||
            Entry->InfluenceAnchors != InfluenceAnchors || Entry->InfluenceWeights != InfluenceWeights)
        { Error = TEXT("Body LODs must share the same garment and weights."); return false; }
        Seen.Add(Entry->BodyLODIndex);
    }
    return true;
}

bool UHUMAClothBindAsset::Evaluate(TConstArrayView<FVector3f> Body, TArray<FVector>& Cloth, FString& Error) const
{
    if (!Validate(Error) || Body.Num() != CanonicalToRender.Num()) { if (Error.IsEmpty()) Error = TEXT("Body vertex count mismatch."); return false; }
    TArray<FVector3f> Normals;
    Normals.Init(FVector3f::ZeroVector, Body.Num());
    for (int32 Index = 0; Index < BodyQuads.Num(); Index += 4)
    {
        const int32* Quad = &BodyQuads[Index];
        const FVector3f Area = FVector3f::CrossProduct(Body[Quad[2]] - Body[Quad[0]], Body[Quad[3]] - Body[Quad[1]]);
        for (int32 Corner = 0; Corner < 4; ++Corner)
        {
            bool Duplicate = false;
            for (int32 Previous = 0; Previous < Corner; ++Previous) Duplicate |= Quad[Previous] == Quad[Corner];
            if (!Duplicate || BindMethod == TEXT("body_field")) Normals[Quad[Corner]] += Area;
        }
    }
    for (FVector3f& Normal : Normals) Normal = Normal.GetSafeNormal(1.e-16f, FVector3f::ZAxisVector);
    TArray<FVector3f> Positions, Tangents, Bitangents, AnchorNormals;
    const int32 Count = AnchorTriangles.Num();
    Positions.SetNumUninitialized(Count); Tangents.SetNumUninitialized(Count);
    Bitangents.SetNumUninitialized(Count); AnchorNormals.SetNumUninitialized(Count);
    for (int32 Index = 0; Index < Count; ++Index)
    {
        const FIntVector Triangle = AnchorTriangles[Index];
        const FVector3f Bary(AnchorBarycentrics[Index]);
        const FVector3f Normal = (Normals[Triangle.X] * Bary.X + Normals[Triangle.Y] * Bary.Y + Normals[Triangle.Z] * Bary.Z).GetSafeNormal(1.e-16f, FVector3f::ZAxisVector);
        FVector3f Tangent = Body[Triangle.Y] - Body[Triangle.X];
        Tangent -= Normal * FVector3f::DotProduct(Tangent, Normal);
        if (Tangent.SizeSquared() <= 1.e-16f)
        {
            Tangent = Body[Triangle.Z] - Body[Triangle.X];
            Tangent -= Normal * FVector3f::DotProduct(Tangent, Normal);
        }
        if (Tangent.SizeSquared() <= 1.e-16f) Tangent = FVector3f::CrossProduct(Normal, FMath::Abs(Normal.Z) > 0.8f ? FVector3f::XAxisVector : FVector3f::YAxisVector);
        Tangent = Tangent.GetSafeNormal(1.e-16f, FVector3f::XAxisVector);
        const FVector3f Bitangent = FVector3f::CrossProduct(Normal, Tangent).GetSafeNormal(1.e-16f, FVector3f::YAxisVector);
        Positions[Index] = Body[Triangle.X] * Bary.X + Body[Triangle.Y] * Bary.Y + Body[Triangle.Z] * Bary.Z;
        Tangents[Index] = FVector3f::CrossProduct(Bitangent, Normal).GetSafeNormal();
        Bitangents[Index] = Bitangent; AnchorNormals[Index] = Normal;
    }
    Cloth.SetNumUninitialized(ClothRestPositions.Num());
    for (int32 Vertex = 0; Vertex < Cloth.Num(); ++Vertex)
    {
        FVector3f Result = FVector3f::ZeroVector;
        for (int32 Slot = 0; Slot < Slots; ++Slot)
        {
            const int32 Influence = Vertex * Slots + Slot;
            const int32 Anchor = InfluenceAnchors[Influence];
            const FVector3f Relative(ClothRestPositions[Vertex] - AnchorRestPositions[Anchor]);
            Result += InfluenceWeights[Influence] * (Positions[Anchor] +
                Tangents[Anchor] * FVector3f::DotProduct(Relative, FVector3f(AnchorRestTangents[Anchor])) +
                Bitangents[Anchor] * FVector3f::DotProduct(Relative, FVector3f(AnchorRestBitangents[Anchor])) +
                AnchorNormals[Anchor] * FVector3f::DotProduct(Relative, FVector3f(AnchorRestNormals[Anchor])));
        }
        Cloth[Vertex] = FVector(Result);
    }
    return true;
}