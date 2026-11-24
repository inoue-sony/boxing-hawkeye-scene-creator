#include "HUMAClothBindAsset.h"
#include "HUMAClothNativeBake.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "HUMAClothPreparationSubsystem.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshResources.h"
#if WITH_EDITOR
#include "HUMAClothBindComponent.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/AutomationTest.h"
#include "Serialization/MemoryReader.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "UObject/UObjectIterator.h"
#endif

namespace
{
constexpr const TCHAR* CanonicalTopology = TEXT("huma_v1_canonical_quads");

bool ReadTopologyMode(const TSharedPtr<FJsonObject>& Object, bool& bCanonical, FString& Error)
{
    bCanonical = false;
    if (!Object->HasField(TEXT("body_topology"))) return true;
    FString Topology;
    if (!Object->TryGetStringField(TEXT("body_topology"), Topology) || Topology != CanonicalTopology)
    { Error = TEXT("Unsupported body topology mode."); return false; }
    bCanonical = true;
    return true;
}

#if WITH_EDITOR
bool LoadCanonicalTopology(TArray<int32>& Quads, TArray<int32>& Triangles, FString& Error)
{
    const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("HUMAClothBind"));
    TArray<uint8> Bytes;
    if (!Plugin || !FFileHelper::LoadFileToArray(Bytes, *(Plugin->GetBaseDir() / TEXT("Resources/Topology/HUMA_v1_fullbody_quads.bin"))))
    { Error = TEXT("Missing bundled HUMA v1 fullBody topology."); return false; }
    FMemoryReader Reader(Bytes);
    uint32 Magic = 0;
    int32 Vertices = 0, Count = 0;
    Reader << Magic << Vertices << Count;
    if (Reader.IsError() || Magic != 0x51424348 || Vertices != 66946 || Count != 267776 || Bytes.Num() != 12 + Count * 4)
    { Error = TEXT("Incompatible bundled HUMA topology."); return false; }
    Quads.SetNumUninitialized(Count);
    Reader.Serialize(Quads.GetData(), Count * 4);
    for (int32 Offset = 0; Offset < Count; Offset += 4)
    {
        const int32* Quad = &Quads[Offset];
        for (int32 Corner = 0; Corner < 4; ++Corner)
            if (Quad[Corner] < 0 || Quad[Corner] >= Vertices)
            { Error = TEXT("Invalid bundled HUMA index."); return false; }
        Triangles.Append({Quad[0], Quad[1], Quad[2], Quad[0], Quad[2], Quad[3]});
    }
    return true;
}

bool GetSourceFingerprint(const UHUMAClothBindAsset& Asset, FString& Result, FString& Error)
{
    FMD5 Hash;
    auto Add = [&](const auto& Value) { Hash.Update(reinterpret_cast<const uint8*>(&Value), sizeof(Value)); };
    auto Name = [&](const UObject* Object)
    {
        const FTCHARToUTF8 Text(Object ? *Object->GetPathName() : TEXT(""));
        const int32 Length = Text.Length();
        Add(Length); Hash.Update(reinterpret_cast<const uint8*>(Text.Get()), Length);
    };
    auto Vertices = [&](const FPositionVertexBuffer& Positions, const FStaticMeshVertexBuffer& UVs)
    {
        Add(Positions.GetNumVertices()); Add(Positions.GetStride()); Add(UVs.GetNumTexCoords()); Add(UVs.GetUseFullPrecisionUVs());
        if (!Positions.GetVertexData() || !UVs.GetTexCoordData()) return false;
        Hash.Update(static_cast<const uint8*>(Positions.GetVertexData()), Positions.GetNumVertices() * Positions.GetStride());
        Hash.Update(static_cast<const uint8*>(UVs.GetTexCoordData()), UVs.GetTexCoordSize());
        return true;
    };
    const uint32 Schema = Asset.bCanonicalBodyTopology ? 2 : 1;
    Add(Schema); Name(Asset.BodyMesh); Name(Asset.GarmentMesh);
    if (!Asset.bCanonicalBodyTopology) Add(Asset.Gender);
    Add(Asset.SigmaMin); Add(Asset.DistanceScale); Add(Asset.BodyLODIndex); Add(Asset.bBakeAllBodyLODs);
    const FVector Translation = Asset.GarmentToBody.GetTranslation(), Scale = Asset.GarmentToBody.GetScale3D();
    const FQuat Rotation = Asset.GarmentToBody.GetRotation();
    Add(Translation); Add(Scale); Add(Rotation);
    auto Skeletal = [&](const USkeletalMesh* Mesh, bool AllLODs, int32 FirstLOD)
    {
        const auto* Data = Mesh ? Mesh->GetResourceForRendering() : nullptr;
        if (!Data || !Data->LODRenderData.IsValidIndex(FirstLOD)) return false;
        const int32 End = AllLODs ? Data->LODRenderData.Num() : FirstLOD + 1;
        Add(End);
        const auto* Model = Mesh->GetImportedModel();
        for (int32 Index = FirstLOD; Index < End; ++Index)
        {
            const auto& LOD = Data->LODRenderData[Index];
            if (!Vertices(LOD.StaticVertexBuffers.PositionVertexBuffer, LOD.StaticVertexBuffers.StaticMeshVertexBuffer)) return false;
            Add(LOD.RenderSections.Num());
            const auto* Buffer = LOD.MultiSizeIndexContainer.GetIndexBuffer();
            if (!Buffer) return false;
            for (const auto& Section : LOD.RenderSections)
            {
                Add(Section.BaseIndex); Add(Section.NumTriangles); Add(Section.MaterialIndex);
                for (uint32 Offset = Section.BaseIndex; Offset < Section.BaseIndex + Section.NumTriangles * 3; ++Offset) Add(Buffer->Get(Offset));
            }
            const TArray<int32>* Map = Model && Model->LODModels.IsValidIndex(Index) ? &Model->LODModels[Index].MeshToImportVertexMap : nullptr;
            const int32 Count = Map ? Map->Num() : 0;
            Add(Count);
            if (Count) Hash.Update(reinterpret_cast<const uint8*>(Map->GetData()), Count * sizeof(int32));
        }
        return true;
    };
    if (!Skeletal(Asset.BodyMesh, Asset.FormatVersion == 2, Asset.BodyLODIndex)) { Error = TEXT("Body source CPU data is unavailable."); return false; }
    if (const auto* Mesh = Cast<USkeletalMesh>(Asset.GarmentMesh))
    {
        if (!Skeletal(Mesh, false, 0)) { Error = TEXT("Garment source CPU data is unavailable."); return false; }
    }
    else if (const auto* StaticMesh = Cast<UStaticMesh>(Asset.GarmentMesh))
    {
        const auto* Data = StaticMesh->GetRenderData();
        if (!Data || Data->LODResources.IsEmpty()) { Error = TEXT("Garment has no LOD0 data."); return false; }
        const auto& LOD = Data->LODResources[0];
        if (!Vertices(LOD.VertexBuffers.PositionVertexBuffer, LOD.VertexBuffers.StaticMeshVertexBuffer)) { Error = TEXT("Garment source CPU data is unavailable."); return false; }
        Add(LOD.Sections.Num());
        for (const auto& Section : LOD.Sections)
        {
            Add(Section.FirstIndex); Add(Section.NumTriangles); Add(Section.MaterialIndex);
            for (uint32 Offset = Section.FirstIndex; Offset < Section.FirstIndex + Section.NumTriangles * 3; ++Offset) Add(LOD.IndexBuffer.GetIndex(Offset));
        }
    }
    else { Error = TEXT("Garment source is missing or unsupported."); return false; }
    uint8 Digest[16];
    Hash.Final(Digest); Result = BytesToHex(Digest, 16);
    return true;
}
#endif

template<typename ValueType>
void SetNumbers(TSharedPtr<FJsonObject> Object, const TCHAR* Name, const TArray<ValueType>& Values)
{
    TArray<TSharedPtr<FJsonValue>> Array;
    Array.Reserve(Values.Num());
    for (const auto& Value : Values) Array.Add(MakeShared<FJsonValueNumber>(double(Value)));
    Object->SetArrayField(Name, Array);
}

void SetVectors(TSharedPtr<FJsonObject> Object, const TCHAR* Name, const TArray<FVector>& Values)
{
    TArray<TSharedPtr<FJsonValue>> Array;
    Array.Reserve(Values.Num());
    for (const FVector& Value : Values)
    {
        TArray<TSharedPtr<FJsonValue>> Row;
        Row.Add(MakeShared<FJsonValueNumber>(Value.X)); Row.Add(MakeShared<FJsonValueNumber>(Value.Y)); Row.Add(MakeShared<FJsonValueNumber>(Value.Z));
        Array.Add(MakeShared<FJsonValueArray>(Row));
    }
    Object->SetArrayField(Name, Array);
}

FString SerializeSnapshot(TSharedPtr<FJsonObject> Object)
{
    FString Text;
    const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
    FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
    return Text;
}

void SetSnapshotMethod(const TSharedPtr<FJsonObject>& Object, const FString& Method)
{
    // Signatures remain geometry/settings signatures so legacy JSON still imports.
    Object->SetStringField(TEXT("bind_method"), Method);
    const TArray<TSharedPtr<FJsonValue>>* Children;
    if (Object->TryGetArrayField(TEXT("body_lods"), Children))
        for (const auto& Child : *Children) SetSnapshotMethod(Child->AsObject(), Method);
}

bool BuildSnapshot(const UHUMAClothBindAsset& Asset, TSharedPtr<FJsonObject>& Object, FString& Error,
    int32 LODIndex, bool AllLODs, bool bCanonical)
{
#if WITH_EDITOR
    constexpr int32 BodyCount = 66946;
    if (!Asset.BodyMesh || !Asset.GarmentMesh) { Error = TEXT("Select a HUMA v1.0.0 Body Mesh and Garment Mesh."); return false; }
    const FSkeletalMeshRenderData* BodyData = Asset.BodyMesh->GetResourceForRendering();
    if (!BodyData || BodyData->LODRenderData.IsEmpty()) { Error = TEXT("Body has no LOD0 render data."); return false; }
    if (!BodyData->LODRenderData.IsValidIndex(LODIndex)) { Error = TEXT("Requested body LOD is missing."); return false; }
    const auto& BodyLOD = BodyData->LODRenderData[LODIndex];
    const auto& BodyVertices = BodyLOD.StaticVertexBuffers.StaticMeshVertexBuffer;
    if (BodyVertices.GetNumTexCoords() < 2 || !BodyVertices.GetUseFullPrecisionUVs())
    {
        Error = FString::Printf(TEXT("HUMA mesh %s LOD%d needs UV1 canonical IDs and Full Precision UVs (UV channels=%u, Full Precision=%s). Rebuild this body LOD first."),
            *Asset.BodyMesh->GetPathName(), LODIndex, BodyVertices.GetNumTexCoords(), BodyVertices.GetUseFullPrecisionUVs() ? TEXT("true") : TEXT("false"));
        return false;
    }
    const TArray<int32>* ImportMap = nullptr;
    if (const FSkeletalMeshModel* Model = Asset.BodyMesh->GetImportedModel(); Model && Model->LODModels.IsValidIndex(LODIndex) && Model->LODModels[LODIndex].MeshToImportVertexMap.Num() == int32(BodyLOD.GetNumVertices()))
        ImportMap = &Model->LODModels[LODIndex].MeshToImportVertexMap;
    TArray<int32> Mapping;
    Mapping.Init(INDEX_NONE, BodyCount);
    TArray<FVector> Body;
    Body.SetNum(BodyCount);
    for (uint32 Index = 0; LODIndex == 0 && Index < BodyLOD.GetNumVertices(); ++Index)
    {
        const float Encoded = BodyVertices.GetVertexUV(Index, 1).X;
        if (!FMath::IsFinite(Encoded)) { Error = TEXT("Non-finite body canonical ID."); return false; }
        int32 Canonical = FMath::RoundToInt(Encoded);
        if (FMath::IsNearlyEqual(Encoded, -1.f, 1.e-3f) && ImportMap) Canonical = (*ImportMap)[Index];
        else if (!FMath::IsNearlyEqual(Encoded, float(Canonical), 1.e-3f)) { Error = TEXT("Body canonical IDs are not exact integers."); return false; }
        if (Canonical < 0 || Canonical >= BodyCount) continue;
        const FVector Position(BodyLOD.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(Index));
        if (Mapping[Canonical] != INDEX_NONE && !Body[Canonical].Equals(Position, 1.e-3))
        { Error = TEXT("Canonical ID maps to inconsistent body positions."); return false; }
        Mapping[Canonical] = Index; Body[Canonical] = Position;
    }
    TArray<int32> BodyTriangles, NormalQuads;
    if (LODIndex == 0)
    {
        if (Mapping.Contains(INDEX_NONE)) { Error = TEXT("Body UV1 is missing HUMA fullBody canonical IDs."); return false; }
        if (bCanonical && !LoadCanonicalTopology(NormalQuads, BodyTriangles, Error)) return false;
    }
    else
    {
        Mapping.Reset(); Body.Reset();
        TMap<int32, int32> CompactIDs;
        TArray<int32> RenderToBody;
        RenderToBody.SetNum(BodyLOD.GetNumVertices());
        for (uint32 Index = 0; Index < BodyLOD.GetNumVertices(); ++Index)
        {
            const float Encoded = BodyVertices.GetVertexUV(Index, 1).X;
            if (!FMath::IsFinite(Encoded) || !FMath::IsNearlyEqual(Encoded, float(FMath::RoundToInt(Encoded)), 1.e-3f))
            { Error = TEXT("Body LOD needs exact canonical IDs in Full Precision UV1."); return false; }
            const int32 Key = Encoded >= 0 ? FMath::RoundToInt(Encoded) : -int32(Index) - 1;
            const FVector Position(BodyLOD.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(Index));
            int32* Existing = CompactIDs.Find(Key);
            if (Existing)
            {
                if (!Body[*Existing].Equals(Position, 1.e-3)) { Error = TEXT("Body LOD IDs have inconsistent positions."); return false; }
                RenderToBody[Index] = *Existing;
            }
            else
            {
                const int32 Compact = Body.Add(Position);
                CompactIDs.Add(Key, Compact); Mapping.Add(Index); RenderToBody[Index] = Compact;
            }
        }
        const auto* Buffer = BodyLOD.MultiSizeIndexContainer.GetIndexBuffer();
        for (const auto& Section : BodyLOD.RenderSections)
        {
            for (uint32 Index = Section.BaseIndex; Index < Section.BaseIndex + Section.NumTriangles * 3; Index += 3)
            {
                const int32 First = RenderToBody[Buffer->Get(Index)], Second = RenderToBody[Buffer->Get(Index + 1)], Third = RenderToBody[Buffer->Get(Index + 2)];
                if (First == Second || First == Third || Second == Third) continue;
                BodyTriangles.Append({First, Second, Third}); NormalQuads.Append({First, Second, Third, Third});
            }
        }
    }
    TArray<FVector> Cloth;
    TArray<FVector2D> UVs;
    TArray<int32> Indices, SectionCounts, SectionMaterials, ClothIds;
    if (const USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset.GarmentMesh))
    {
        const FSkeletalMeshRenderData* Data = Mesh->GetResourceForRendering();
        if (!Data || Data->LODRenderData.IsEmpty()) { Error = TEXT("Garment has no LOD0 data."); return false; }
        const auto& LOD = Data->LODRenderData[0];
        for (uint32 Index = 0; Index < LOD.GetNumVertices(); ++Index)
        {
            Cloth.Add(Asset.GarmentToBody.TransformPosition(FVector(LOD.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(Index))));
            UVs.Add(FVector2D(LOD.StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(Index, 0)));
        }
        if (const FSkeletalMeshModel* Model = Mesh->GetImportedModel(); Model && !Model->LODModels.IsEmpty() && Model->LODModels[0].MeshToImportVertexMap.Num() == Cloth.Num())
            ClothIds = Model->LODModels[0].MeshToImportVertexMap;
        const auto* Buffer = LOD.MultiSizeIndexContainer.GetIndexBuffer();
        for (const auto& Section : LOD.RenderSections)
        {
            SectionCounts.Add(Section.NumTriangles * 3); SectionMaterials.Add(Section.MaterialIndex);
            for (uint32 Index = Section.BaseIndex; Index < Section.BaseIndex + Section.NumTriangles * 3; ++Index) Indices.Add(Buffer->Get(Index));
        }
    }
    else if (const UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset.GarmentMesh))
    {
        const FStaticMeshRenderData* Data = StaticMesh->GetRenderData();
        if (!Data || Data->LODResources.IsEmpty()) { Error = TEXT("Garment has no LOD0 data."); return false; }
        const auto& LOD = Data->LODResources[0];
        for (uint32 Index = 0; Index < uint32(LOD.GetNumVertices()); ++Index)
        {
            Cloth.Add(Asset.GarmentToBody.TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Index))));
            UVs.Add(FVector2D(LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(Index, 0)));
        }
        for (const auto& Section : LOD.Sections)
        {
            SectionCounts.Add(Section.NumTriangles * 3); SectionMaterials.Add(Section.MaterialIndex);
            for (uint32 Index = Section.FirstIndex; Index < Section.FirstIndex + Section.NumTriangles * 3; ++Index) Indices.Add(LOD.IndexBuffer.GetIndex(Index));
        }
    }
    else { Error = TEXT("Garment must be a Static Mesh or Skeletal Mesh."); return false; }
    if (Cloth.IsEmpty() || Indices.IsEmpty()) { Error = TEXT("Garment has no triangles."); return false; }
    Object = MakeShared<FJsonObject>();
    Object->SetNumberField(TEXT("version"), 1);
    Object->SetNumberField(TEXT("body_lod"), LODIndex);
    Object->SetStringField(TEXT("body_asset"), Asset.BodyMesh->GetPathName());
    Object->SetStringField(TEXT("garment_asset"), Asset.GarmentMesh->GetPathName());
    if (bCanonical) Object->SetStringField(TEXT("body_topology"), CanonicalTopology);
    else Object->SetStringField(TEXT("gender"), Asset.Gender == EHUMAClothGender::Female ? TEXT("FEMALE") : TEXT("MALE"));
    Object->SetNumberField(TEXT("sigma_min"), Asset.SigmaMin);
    Object->SetNumberField(TEXT("distance_scale"), Asset.DistanceScale);
    SetVectors(Object, TEXT("body_positions"), Body);
    SetVectors(Object, TEXT("cloth_positions"), Cloth);
    SetNumbers(Object, TEXT("canonical_to_render"), Mapping);
    if (LODIndex > 0 || bCanonical)
    {
        SetNumbers(Object, TEXT("body_triangles"), BodyTriangles);
        SetNumbers(Object, TEXT("body_quads"), NormalQuads);
    }
    SetNumbers(Object, TEXT("cloth_triangles"), Indices);
    SetNumbers(Object, TEXT("section_index_counts"), SectionCounts);
    SetNumbers(Object, TEXT("section_materials"), SectionMaterials);
    if (!ClothIds.IsEmpty()) SetNumbers(Object, TEXT("cloth_vertex_ids"), ClothIds);
    TArray<TSharedPtr<FJsonValue>> UVArray;
    for (const FVector2D& UV : UVs)
    {
        TArray<TSharedPtr<FJsonValue>> Row;
        Row.Add(MakeShared<FJsonValueNumber>(UV.X)); Row.Add(MakeShared<FJsonValueNumber>(UV.Y));
        UVArray.Add(MakeShared<FJsonValueArray>(Row));
    }
    Object->SetArrayField(TEXT("cloth_uvs"), UVArray);
    if (AllLODs && LODIndex == 0)
    {
        TArray<TSharedPtr<FJsonValue>> LODs;
        for (int32 Index = 1; Index < BodyData->LODRenderData.Num(); ++Index)
        {
            TSharedPtr<FJsonObject> Child;
            if (!BuildSnapshot(Asset, Child, Error, Index, false, bCanonical)) return false;
            LODs.Add(MakeShared<FJsonValueObject>(Child));
        }
        if (!LODs.IsEmpty()) Object->SetArrayField(TEXT("body_lods"), LODs);
    }
    Object->SetStringField(TEXT("signature"), FMD5::HashAnsiString(*SerializeSnapshot(Object)));
    return true;
#else
    Error = TEXT("Source snapshots require the editor."); return false;
#endif
}

template<typename ValueType>
bool ReadNumbers(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, TArray<ValueType>& Values)
{
    const TArray<TSharedPtr<FJsonValue>>* Array;
    if (!Object->TryGetArrayField(Name, Array)) return false;
    Values.Reset(Array->Num());
    for (const auto& Value : *Array)
    {
        double Number;
        if (!Value->TryGetNumber(Number) || !FMath::IsFinite(Number)) return false;
        Values.Add(ValueType(Number));
    }
    return true;
}

bool ReadVectors(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, TArray<FVector>& Values)
{
    const TArray<TSharedPtr<FJsonValue>>* Array;
    if (!Object->TryGetArrayField(Name, Array)) return false;
    Values.Reset(Array->Num());
    for (const auto& Value : *Array)
    {
        const TArray<TSharedPtr<FJsonValue>>* Row;
        double Coordinates[3];
        if (!Value->TryGetArray(Row) || Row->Num() != 3) return false;
        for (int32 Axis = 0; Axis < 3; ++Axis)
            if (!(*Row)[Axis]->TryGetNumber(Coordinates[Axis]) || !FMath::IsFinite(Coordinates[Axis])) return false;
        Values.Add(FVector(Coordinates[0], Coordinates[1], Coordinates[2]));
    }
    return true;
}

bool ReadBakeData(const TSharedPtr<FJsonObject>& Object, UHUMAClothBindAsset& Candidate, FString& Error)
{
    double Version = 0, SlotCount = 0, LOD = 0;
    if (!Object->TryGetNumberField(TEXT("version"), Version) || (Version != 1 && Version != 2) ||
        !Object->TryGetNumberField(TEXT("slots"), SlotCount) || SlotCount != 24 || !Object->TryGetStringField(TEXT("signature"), Candidate.BakeSignature))
    { Error = TEXT("Unsupported body LOD bake format."); return false; }
    Object->TryGetNumberField(TEXT("body_lod"), LOD);
    Candidate.FormatVersion = int32(Version); Candidate.BodyLODIndex = int32(LOD); Candidate.Slots = int32(SlotCount);
    if (!ReadTopologyMode(Object, Candidate.bCanonicalBodyTopology, Error)) return false;
    Candidate.BindMethod = TEXT("cloth_graph");
    if (Object->HasField(TEXT("bind_method")) && !Object->TryGetStringField(TEXT("bind_method"), Candidate.BindMethod))
    { Error = TEXT("Bind method must be a string."); return false; }
    Candidate.NormalizeBindMethod();
    if (Candidate.BindMethod != TEXT("body_field") && Candidate.BindMethod != TEXT("cloth_graph"))
    { Error = TEXT("Unsupported Bind method."); return false; }
    Candidate.BakeMethod = Candidate.BindMethod == TEXT("body_field") ? EHUMAClothBindMethod::BodyField : EHUMAClothBindMethod::ClothGraph;
    Candidate.bBakeMethodInitialized = true;
    bool Valid = ReadNumbers(Object, TEXT("canonical_to_render"), Candidate.CanonicalToRender) && ReadNumbers(Object, TEXT("body_quads"), Candidate.BodyQuads) &&
        ReadVectors(Object, TEXT("rest_body"), Candidate.RestBody) && ReadVectors(Object, TEXT("anchor_barycentrics"), Candidate.AnchorBarycentrics) &&
        ReadVectors(Object, TEXT("anchor_rest_positions"), Candidate.AnchorRestPositions) && ReadVectors(Object, TEXT("anchor_rest_tangents"), Candidate.AnchorRestTangents) &&
        ReadVectors(Object, TEXT("anchor_rest_bitangents"), Candidate.AnchorRestBitangents) && ReadVectors(Object, TEXT("anchor_rest_normals"), Candidate.AnchorRestNormals) &&
        ReadVectors(Object, TEXT("cloth_rest_positions"), Candidate.ClothRestPositions) && ReadNumbers(Object, TEXT("influence_anchors"), Candidate.InfluenceAnchors) &&
        ReadNumbers(Object, TEXT("influence_weights"), Candidate.InfluenceWeights) && ReadNumbers(Object, TEXT("cloth_triangles"), Candidate.ClothTriangles) &&
        ReadNumbers(Object, TEXT("section_index_counts"), Candidate.SectionIndexCounts) && ReadNumbers(Object, TEXT("section_materials"), Candidate.SectionMaterials);
    TArray<FVector> Triangles;
    Valid &= ReadVectors(Object, TEXT("anchor_triangles"), Triangles);
    for (const FVector& Triangle : Triangles) Candidate.AnchorTriangles.Add(FIntVector(int32(Triangle.X), int32(Triangle.Y), int32(Triangle.Z)));
    const TArray<TSharedPtr<FJsonValue>>* UVArray;
    if (Object->TryGetArrayField(TEXT("cloth_uvs"), UVArray))
    {
        for (const auto& Value : *UVArray)
        {
            const TArray<TSharedPtr<FJsonValue>>* Row;
            double Horizontal = 0, Vertical = 0;
            if (!Value->TryGetArray(Row) || Row->Num() != 2 || !(*Row)[0]->TryGetNumber(Horizontal) || !(*Row)[1]->TryGetNumber(Vertical)) { Valid = false; break; }
            Candidate.ClothUVs.Add(FVector2D(Horizontal, Vertical));
        }
    }
    else Valid = false;
    const TArray<TSharedPtr<FJsonValue>>* Children;
    if (Version == 2)
    {
        if (!Object->TryGetArrayField(TEXT("body_lods"), Children) || Children->IsEmpty()) return false;
        for (const auto& Value : *Children)
        {
            const TSharedPtr<FJsonObject>* ChildObject;
            if (!Value->TryGetObject(ChildObject)) return false;
            UHUMAClothBindAsset* Child = NewObject<UHUMAClothBindAsset>(&Candidate);
            if (!ReadBakeData(*ChildObject, *Child, Error)) return false;
            Candidate.AdditionalBodyLODs.Add(Child);
        }
    }
    return Valid && Candidate.Validate(Error) && Candidate.RunReferenceChecks();
}

#if WITH_EDITOR
bool NativeBakeSnapshot(const TSharedPtr<FJsonObject>& Snapshot, UHUMAClothBindAsset& Candidate, FString& Error, const UHUMAClothBindAsset* Shared = nullptr)
{
    Candidate.FormatVersion = 1;
    Candidate.bCanonicalBodyTopology = true;
    Candidate.BodyLODIndex = int32(Snapshot->GetNumberField(TEXT("body_lod")));
    Candidate.BakeSignature = Snapshot->GetStringField(TEXT("signature"));
    TArray<int32> Triangles, IDs;
    if (!ReadVectors(Snapshot, TEXT("body_positions"), Candidate.RestBody) ||
        !ReadVectors(Snapshot, TEXT("cloth_positions"), Candidate.ClothRestPositions) ||
        !ReadNumbers(Snapshot, TEXT("canonical_to_render"), Candidate.CanonicalToRender) ||
        !ReadNumbers(Snapshot, TEXT("cloth_triangles"), Candidate.ClothTriangles) ||
        !ReadNumbers(Snapshot, TEXT("section_index_counts"), Candidate.SectionIndexCounts) ||
        !ReadNumbers(Snapshot, TEXT("section_materials"), Candidate.SectionMaterials))
    { Error = TEXT("Invalid native bake snapshot."); return false; }
    ReadNumbers(Snapshot, TEXT("cloth_vertex_ids"), IDs);
    for (const auto& Value : Snapshot->GetArrayField(TEXT("cloth_uvs")))
    {
        const auto& Row = Value->AsArray();
        Candidate.ClothUVs.Add(FVector2D(Row[0]->AsNumber(), Row[1]->AsNumber()));
    }
    if (!ReadNumbers(Snapshot, TEXT("body_triangles"), Triangles) || !ReadNumbers(Snapshot, TEXT("body_quads"), Candidate.BodyQuads))
    { Error = TEXT("Missing body LOD topology."); return false; }
    if (!BakeHUMANativeLOD(Candidate, Triangles, IDs, Snapshot->GetNumberField(TEXT("sigma_min")), Snapshot->GetNumberField(TEXT("distance_scale")), Error, Shared))
    { if (Error.IsEmpty()) Error = Candidate.BakeStatus; return false; }
    return true;
}

TSharedPtr<FJsonObject> WriteBakeData(const UHUMAClothBindAsset& Asset)
{
    auto Object = MakeShared<FJsonObject>();
    Object->SetNumberField(TEXT("version"), Asset.FormatVersion);
    Object->SetNumberField(TEXT("body_lod"), Asset.BodyLODIndex);
    Object->SetNumberField(TEXT("slots"), Asset.Slots);
    Object->SetStringField(TEXT("signature"), Asset.BakeSignature);
    Object->SetStringField(TEXT("baker"), TEXT("cpp"));
    Object->SetStringField(TEXT("bind_method"), Asset.BindMethod);
    if (Asset.bCanonicalBodyTopology) Object->SetStringField(TEXT("body_topology"), CanonicalTopology);
    SetNumbers(Object, TEXT("canonical_to_render"), Asset.CanonicalToRender);
    SetNumbers(Object, TEXT("body_quads"), Asset.BodyQuads);
    SetVectors(Object, TEXT("rest_body"), Asset.RestBody);
    SetVectors(Object, TEXT("anchor_barycentrics"), Asset.AnchorBarycentrics);
    SetVectors(Object, TEXT("anchor_rest_positions"), Asset.AnchorRestPositions);
    SetVectors(Object, TEXT("anchor_rest_tangents"), Asset.AnchorRestTangents);
    SetVectors(Object, TEXT("anchor_rest_bitangents"), Asset.AnchorRestBitangents);
    SetVectors(Object, TEXT("anchor_rest_normals"), Asset.AnchorRestNormals);
    TArray<FVector> Triangles;
    for (const FIntVector& Triangle : Asset.AnchorTriangles) Triangles.Add(FVector(Triangle.X, Triangle.Y, Triangle.Z));
    SetVectors(Object, TEXT("anchor_triangles"), Triangles);
    SetVectors(Object, TEXT("cloth_rest_positions"), Asset.ClothRestPositions);
    SetNumbers(Object, TEXT("influence_anchors"), Asset.InfluenceAnchors);
    SetNumbers(Object, TEXT("influence_weights"), Asset.InfluenceWeights);
    SetNumbers(Object, TEXT("cloth_triangles"), Asset.ClothTriangles);
    SetNumbers(Object, TEXT("section_index_counts"), Asset.SectionIndexCounts);
    SetNumbers(Object, TEXT("section_materials"), Asset.SectionMaterials);
    TArray<TSharedPtr<FJsonValue>> UVs;
    for (const FVector2D& UV : Asset.ClothUVs)
    {
        TArray<TSharedPtr<FJsonValue>> Row = {MakeShared<FJsonValueNumber>(UV.X), MakeShared<FJsonValueNumber>(UV.Y)};
        UVs.Add(MakeShared<FJsonValueArray>(Row));
    }
    Object->SetArrayField(TEXT("cloth_uvs"), UVs);
    Object->SetNumberField(TEXT("native_rest_error_cm"), Asset.MaxRestErrorCm);
    Object->SetNumberField(TEXT("native_rigid_error_cm"), Asset.MaxRigidErrorCm);
    if (!Asset.AdditionalBodyLODs.IsEmpty())
    {
        TArray<TSharedPtr<FJsonValue>> Children;
        for (const auto& Child : Asset.AdditionalBodyLODs) Children.Add(MakeShared<FJsonValueObject>(WriteBakeData(*Child)));
        Object->SetArrayField(TEXT("body_lods"), Children);
    }
    return Object;
}
#endif
}

bool UHUMAClothBindAsset::ExportSnapshot(const FString& Path)
{
    TSharedPtr<FJsonObject> Snapshot;
    FString Error;
    if (!BuildSnapshot(*this, Snapshot, Error, BodyLODIndex, bBakeAllBodyLODs, bCanonicalBodyTopology || FormatVersion == 0))
    { BakeStatus = Error; return false; }
    SetSnapshotMethod(Snapshot, GetRequestedBindMethod());
    if (!FFileHelper::SaveStringToFile(SerializeSnapshot(Snapshot), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    { BakeStatus = TEXT("Could not write source snapshot."); return false; }
    return true;
}

bool UHUMAClothBindAsset::ExportBakeData(const FString& Path) const
{
#if WITH_EDITOR
    FString Error;
    if (!IsBakeCurrent(Error))
    {
        UE_LOG(LogTemp, Error, TEXT("HUMAClothBind bake export rejected for %s: %s"), *GetName(), *Error);
        return false;
    }
    if (!FFileHelper::SaveStringToFile(SerializeSnapshot(WriteBakeData(*this)), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        UE_LOG(LogTemp, Error, TEXT("HUMAClothBind bake export write failed: %s"), *Path);
        return false;
    }
    return true;
#else
    UE_LOG(LogTemp, Error, TEXT("HUMAClothBind bake data export requires an Editor build."));
    return false;
#endif
}

bool UHUMAClothBindAsset::IsBakeCurrent(FString& Error) const
{
    if (!Validate(Error)) return false;
    if (GetRequestedBindMethod() != BindMethod)
    { Error = TEXT("Bind method changed. Click Bake to rebuild the tables."); return false; }
#if WITH_EDITOR
    FString Fingerprint;
    if (!GetSourceFingerprint(*this, Fingerprint, Error)) return false;
    if (!SourceFingerprint.IsEmpty())
    {
        if (SourceFingerprint != Fingerprint) { Error = TEXT("Source mesh or settings changed. Bake again."); return false; }
        return true;
    }
    if (CachedSourceFingerprint == Fingerprint && CachedBakeSignature == BakeSignature) return true;
    TSharedPtr<FJsonObject> Snapshot;
    if (!BuildSnapshot(*this, Snapshot, Error, BodyLODIndex, FormatVersion == 2, bCanonicalBodyTopology)) return false;
    if (Snapshot->GetStringField(TEXT("signature")) != BakeSignature)
    { Error = TEXT("Source mesh or settings changed. Bake again."); return false; }
    CachedSourceFingerprint = Fingerprint; CachedBakeSignature = BakeSignature;
#endif
    return true;
}

bool UHUMAClothBindAsset::RunReferenceChecks()
{
    FString Error;
    if (RestBody.Num() != CanonicalToRender.Num() || !Validate(Error)) { BakeStatus = TEXT("Reference body or bake data is invalid."); return false; }
    TArray<FVector3f> Body;
    for (const FVector& Position : RestBody) Body.Add(FVector3f(Position));
    TArray<FVector> Result;
    if (!Evaluate(Body, Result, Error)) { BakeStatus = Error; return false; }
    MaxRestErrorCm = 0;
    for (int32 Index = 0; Index < Result.Num(); ++Index) MaxRestErrorCm = FMath::Max(MaxRestErrorCm, float((Result[Index] - ClothRestPositions[Index]).GetAbsMax()));
    const FQuat Rotation(FVector(0.3, 0.7, -0.2).GetSafeNormal(), 0.73);
    const FVector Translation(13, -8, 5);
    for (int32 Index = 0; Index < Body.Num(); ++Index) Body[Index] = FVector3f(Rotation.RotateVector(RestBody[Index]) + Translation);
    if (!Evaluate(Body, Result, Error)) { BakeStatus = Error; return false; }
    MaxRigidErrorCm = 0;
    for (int32 Index = 0; Index < Result.Num(); ++Index) MaxRigidErrorCm = FMath::Max(MaxRigidErrorCm, float((Result[Index] - Rotation.RotateVector(ClothRestPositions[Index]) - Translation).GetAbsMax()));
    bool Valid = MaxRestErrorCm < 0.01f && MaxRigidErrorCm < 0.01f;
    for (auto& Entry : AdditionalBodyLODs) Valid &= Entry && Entry->RunReferenceChecks();
    BakeStatus = Valid ? FString::Printf(TEXT("Baked / reference checks passed / %d body LODs"), AdditionalBodyLODs.Num() + 1) : TEXT("Rest or rigid-motion reference check failed.");
    return Valid;
}

bool UHUMAClothBindAsset::ImportBake(const FString& Path)
{
    FString Text, Error;
    TSharedPtr<FJsonObject> Object;
    if (!FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) || !Object)
    { BakeStatus = TEXT("Could not read bake JSON."); return false; }
    TSharedPtr<FJsonObject> Snapshot;
    double Version = 0;
    Object->TryGetNumberField(TEXT("version"), Version);
    bool bCanonical = false;
    if (!ReadTopologyMode(Object, bCanonical, Error) ||
        !BuildSnapshot(*this, Snapshot, Error, BodyLODIndex, Version == 2, bCanonical))
    { BakeStatus = Error; return false; }
    FString Signature;
    if (!Object->TryGetStringField(TEXT("signature"), Signature) || Signature != Snapshot->GetStringField(TEXT("signature")))
    { BakeStatus = TEXT("Bake package does not match this source snapshot."); return false; }
    UHUMAClothBindAsset* Candidate = NewObject<UHUMAClothBindAsset>();
    if (!ReadBakeData(Object, *Candidate, Error))
    { BakeStatus = TEXT("Rejected body LOD bake: ") + Error; return false; }
    if (Version == 2)
    {
        const auto& Expected = Snapshot->GetArrayField(TEXT("body_lods"));
        if (Expected.Num() != Candidate->AdditionalBodyLODs.Num()) { BakeStatus = TEXT("Body LOD count mismatch."); return false; }
        for (const auto& Child : Candidate->AdditionalBodyLODs)
        {
            bool Match = false;
            for (const auto& Entry : Expected)
                Match |= Child->BodyLODIndex == int32(Entry->AsObject()->GetNumberField(TEXT("body_lod"))) && Child->BakeSignature == Entry->AsObject()->GetStringField(TEXT("signature"));
            if (!Match) { BakeStatus = TEXT("Body LOD signature mismatch."); return false; }
        }
    }
    Modify();
    AdditionalBodyLODs.Reset();
    for (const auto& Child : Candidate->AdditionalBodyLODs)
        AdditionalBodyLODs.Add(DuplicateObject<UHUMAClothBindAsset>(Child, this));
    FormatVersion = Candidate->FormatVersion; BakeSignature = Signature; Slots = Candidate->Slots;
    CanonicalToRender = MoveTemp(Candidate->CanonicalToRender); BodyQuads = MoveTemp(Candidate->BodyQuads); RestBody = MoveTemp(Candidate->RestBody);
    BindMethod = Candidate->BindMethod;
    BakeMethod = Candidate->BakeMethod;
    bCanonicalBodyTopology = Candidate->bCanonicalBodyTopology;
    bBakeMethodInitialized = true;
    AnchorTriangles = MoveTemp(Candidate->AnchorTriangles); AnchorBarycentrics = MoveTemp(Candidate->AnchorBarycentrics);
    AnchorRestPositions = MoveTemp(Candidate->AnchorRestPositions); AnchorRestTangents = MoveTemp(Candidate->AnchorRestTangents);
    AnchorRestBitangents = MoveTemp(Candidate->AnchorRestBitangents); AnchorRestNormals = MoveTemp(Candidate->AnchorRestNormals);
    ClothRestPositions = MoveTemp(Candidate->ClothRestPositions); InfluenceAnchors = MoveTemp(Candidate->InfluenceAnchors); InfluenceWeights = MoveTemp(Candidate->InfluenceWeights);
    ClothTriangles = MoveTemp(Candidate->ClothTriangles); ClothUVs = MoveTemp(Candidate->ClothUVs);
    SectionIndexCounts = MoveTemp(Candidate->SectionIndexCounts); SectionMaterials = MoveTemp(Candidate->SectionMaterials);
    MaxRestErrorCm = Candidate->MaxRestErrorCm; MaxRigidErrorCm = Candidate->MaxRigidErrorCm; BakeStatus = Candidate->BakeStatus;
#if WITH_EDITOR
    SourceFingerprint.Reset(); CachedSourceFingerprint.Reset(); CachedBakeSignature.Reset();
    GetSourceFingerprint(*this, SourceFingerprint, Error);
#endif
    MarkPackageDirty();
    UHUMAClothPreparationSubsystem::InvalidateAssetEverywhere(this);
#if WITH_EDITOR
    for (TObjectIterator<UHUMAClothBindComponent> It; It; ++It)
        if (!It->IsTemplate() && It->IsRegistered() && It->BindAsset == this) It->RebuildPreview();
#endif
    return true;
}

void UHUMAClothBindAsset::Bake()
{
#if WITH_EDITOR
    const double Start = FPlatformTime::Seconds();
    FString Error;
    TSharedPtr<FJsonObject> Snapshot;
    if (!BuildSnapshot(*this, Snapshot, Error, BodyLODIndex, bBakeAllBodyLODs, true)) { BakeStatus = Error; return; }
    FScopedSlowTask Progress(1, FText::FromString(TEXT("Baking HUMA SoftBind (native C++)")));
    Progress.MakeDialog();
    UHUMAClothBindAsset* Candidate = NewObject<UHUMAClothBindAsset>();
    bBakeMethodInitialized = true;
    Candidate->BakeMethod = BakeMethod;
    if (!NativeBakeSnapshot(Snapshot, *Candidate, Error)) { BakeStatus = Error; return; }
    const TArray<TSharedPtr<FJsonValue>>* Children;
    if (Snapshot->TryGetArrayField(TEXT("body_lods"), Children))
    {
        for (const auto& Value : *Children)
        {
            UHUMAClothBindAsset* Child = NewObject<UHUMAClothBindAsset>(Candidate);
            if (!NativeBakeSnapshot(Value->AsObject(), *Child, Error, Candidate)) { BakeStatus = Error; return; }
            Candidate->AdditionalBodyLODs.Add(Child);
        }
        if (!Candidate->AdditionalBodyLODs.IsEmpty()) Candidate->FormatVersion = 2;
    }
    if (!Candidate->RunReferenceChecks()) { BakeStatus = Candidate->BakeStatus; return; }
    const FString Directory = FPaths::ProjectSavedDir() / TEXT("HUMAClothBind");
    SetSnapshotMethod(Snapshot, GetRequestedBindMethod());
    IFileManager::Get().MakeDirectory(*Directory, true);
    const FString SnapshotPath = FPaths::ConvertRelativePathToFull(Directory / (GetName() + TEXT("_source.json")));
    const FString PackagePath = FPaths::ConvertRelativePathToFull(Directory / (GetName() + TEXT("_bake.json")));
    if (!FFileHelper::SaveStringToFile(SerializeSnapshot(Snapshot), *SnapshotPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ||
        !FFileHelper::SaveStringToFile(SerializeSnapshot(WriteBakeData(*Candidate)), *PackagePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    { BakeStatus = TEXT("Could not save native bake package."); return; }
    if (ImportBake(PackagePath)) UE_LOG(LogTemp, Display, TEXT("HUMAClothBind native bake %s: %.3f s"), *GetName(), FPlatformTime::Seconds() - Start);
#endif
}

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMANativeSampleBakeTest, "HUMAClothBind.NativeBake.SampleBodyLODs", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHUMANativeSampleBakeTest::RunTest(const FString&)
{
    const TArray<FString> Names = {TEXT("CB_Female_sweater"), TEXT("CB_Female_skirt"), TEXT("CB_Male_shirt"), TEXT("CB_Male_tee"), TEXT("CB_Male_pants"), TEXT("CB_Male_buttons")};
    for (const FString& Name : Names)
    {
        const auto* Source = LoadObject<UHUMAClothBindAsset>(nullptr, *(TEXT("/Game/HUMAClothBind/Bindings/") + Name));
        if (!TestNotNull(*Name, Source)) return false;
        auto* Candidate = DuplicateObject<UHUMAClothBindAsset>(Source, GetTransientPackage(), *FString(TEXT("Native_") + Name));
        Candidate->BakeSignature.Reset();
        Candidate->Bake();
        if (!TestFalse(*Candidate->BakeStatus, Candidate->BakeSignature.IsEmpty())) return false;
        TestEqual(TEXT("All four body LODs baked"), Candidate->AdditionalBodyLODs.Num(), 3);
        FString Error;
        TestTrue(TEXT("Fresh source fingerprint"), Candidate->IsBakeCurrent(Error));
        Candidate->SigmaMin += 0.5f;
        TestFalse(TEXT("Setting edit invalidates fingerprint"), Candidate->IsBakeCurrent(Error));
        Candidate->SigmaMin -= 0.5f;
        TestTrue(TEXT("Restored settings validate"), Candidate->IsBakeCurrent(Error));
    }
    return true;
}
#endif