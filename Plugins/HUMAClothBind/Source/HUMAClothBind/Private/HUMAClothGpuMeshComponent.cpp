#include "HUMAClothGpuMeshComponent.h"
#include "HUMAClothBindAsset.h"
#include "HUMAClothGpuState.h"
#include "HUMAClothVelocityVertexFactory.h"
#include "Components/SkeletalMeshComponent.h"
#include "DynamicMeshBuilder.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "LocalVertexFactory.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"
#include "Materials/MaterialRenderProxy.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveUniformShaderParametersBuilder.h"
#include "PrimitiveViewRelevance.h"
#include "RenderGraphUtils.h"
#include "SceneInterface.h"
#include "StaticMeshResources.h"

FHUMAClothGpuState::FHUMAClothGpuState(const UHUMAClothBindAsset& Asset, TConstArrayView<int32> Mapping, bool bIncludeBodyLODs)
{
    BindBodyLOD = Asset.BodyLODIndex;
    Slots = Asset.Slots;
    auto Topology = MakeShared<FHUMAClothBodyTopology, ESPMode::ThreadSafe>();
    for (int32 Value : (Mapping.IsEmpty() ? TConstArrayView<int32>(Asset.CanonicalToRender) : Mapping)) Topology->Mapping.Add(Value);
    for (int32 Value : Asset.BodyQuads) Topology->Quads.Add(Value);
    for (int32 Value : Asset.ClothTriangles) Triangles.Add(Value);
    for (const FIntVector& Triangle : Asset.AnchorTriangles) { AnchorTriangles.Add(Triangle.X); AnchorTriangles.Add(Triangle.Y); AnchorTriangles.Add(Triangle.Z); }
    for (int32 Value : Asset.InfluenceAnchors) InfluenceAnchors.Add(Value);
    InfluenceWeights = Asset.InfluenceWeights;
    for (const FVector& Value : Asset.ClothRestPositions) Rest.Add(FVector3f(Value));
    for (const FVector& Value : Asset.AnchorBarycentrics) Bary.Add(FVector3f(Value));
    for (const FVector& Value : Asset.AnchorRestPositions) RestPosition.Add(FVector3f(Value));
    for (const FVector& Value : Asset.AnchorRestTangents) RestTangent.Add(FVector3f(Value));
    for (const FVector& Value : Asset.AnchorRestBitangents) RestBitangent.Add(FVector3f(Value));
    for (const FVector& Value : Asset.AnchorRestNormals) RestNormal.Add(FVector3f(Value));
    for (const FVector2D& Value : Asset.ClothUVs) UV.Add(FVector2f(Value));
    auto BuildIncidents = [](int32 Count, const TArray<uint32>& Faces, int32 Corners, bool RepeatCorners, TArray<uint32>& Offsets, TArray<uint32>& Incidents)
    {
        Offsets.Init(0, Count + 1);
        auto Visit = [&](auto&& AddCorner)
        {
            for (int32 Face = 0; Face < Faces.Num() / Corners; ++Face)
                for (int32 Corner = 0; Corner < Corners; ++Corner)
                {
                    const uint32 Vertex = Faces[Face * Corners + Corner];
                    bool Duplicate = false;
                    for (int32 Previous = 0; Previous < Corner; ++Previous) Duplicate |= Faces[Face * Corners + Previous] == Vertex;
                    if (!Duplicate || RepeatCorners) AddCorner(Vertex, uint32(Face));
                }
        };
        Visit([&](uint32 Vertex, uint32) { ++Offsets[Vertex + 1]; });
        for (int32 Vertex = 1; Vertex <= Count; ++Vertex) Offsets[Vertex] += Offsets[Vertex - 1];
        Incidents.SetNumUninitialized(Offsets.Last());
        TArray<uint32> Cursor = Offsets;
        Visit([&](uint32 Vertex, uint32 Face) { Incidents[Cursor[Vertex]++] = Face; });
    };
    BuildIncidents(Topology->Mapping.Num(), Topology->Quads, 4, Asset.BindMethod == TEXT("body_field"), Topology->Offsets, Topology->Incidents);
    Topology->UpdateSignature();
    BodyTopology = Topology;
    BuildIncidents(Rest.Num(), Triangles, 3, false, ClothOffsets, ClothIncidents);
    if (bIncludeBodyLODs)
        for (const auto& Entry : Asset.AdditionalBodyLODs)
            BodyLODs.Add(Entry->BodyLODIndex, MakeShared<FHUMAClothGpuState, ESPMode::ThreadSafe>(*Entry));
}

FHUMAClothGpuState::FHUMAClothGpuState(const UHUMAClothBindAsset& Asset, const FHUMAClothSourceLODs& Sources)
    : FHUMAClothGpuState(Asset, Sources.FindChecked(Asset.BodyLODIndex).Mapping, false)
{
    SourceTransfer = Sources.FindChecked(Asset.BodyLODIndex).Transfer;
    for (const auto& Pair : Sources)
    {
        if (Pair.Key == Asset.BodyLODIndex) continue;
        const auto* Bind = Asset.GetBodyLODBind(Pair.Value.BindBodyLOD);
        check(Bind);
        auto Child = MakeShared<FHUMAClothGpuState, ESPMode::ThreadSafe>(*Bind, Pair.Value.Mapping, false);
        Child->SourceTransfer = Pair.Value.Transfer;
        BodyLODs.Add(Pair.Key, MoveTemp(Child));
    }
}

class FHUMAOutputVertexBuffer : public FVertexBuffer
{
public:
    TSharedPtr<FHUMAClothGpuState, ESPMode::ThreadSafe> State;
    bool bTangent = false;
    bool bPrevious = false;
    FShaderResourceViewRHIRef SRV;
    TRefCountPtr<FRDGPooledBuffer> Pool;
    virtual void InitRHI(FRHICommandListBase& RHICmdList) override
    {
        auto Desc = FRDGBufferDesc::CreateBufferDesc(4, State->Rest.Num() * (bTangent ? 2 : 3));
        Desc.Usage |= BUF_VertexBuffer | BUF_ShaderResource | BUF_UnorderedAccess;
        AllocatePooledBuffer(Desc, Pool, bTangent ? TEXT("HUMACloth.Tangents") : TEXT("HUMACloth.Positions"));
        {
            FScopeLock Lock(&State->Mutex);
            (bTangent ? State->Tangents : bPrevious ? State->PreviousPositions : State->Positions) = Pool;
            if (!bTangent) { State->bHistoryValid = false; State->LastPositionFrame = MAX_uint64; }
        }
        VertexBufferRHI = Pool->GetRHI();
        FRHIBufferSRVCreateInfo Info;
        Info.Format = bTangent ? PF_R8G8B8A8_SNORM : PF_R32_FLOAT;
        SRV = Pool->GetSRV(RHICmdList, Info);
    }
    virtual void ReleaseRHI() override
    {
        {
            FScopeLock Lock(&State->Mutex);
            auto& Published = bTangent ? State->Tangents : bPrevious ? State->PreviousPositions : State->Positions;
            if (Published == Pool) Published.SafeRelease();
        }
        Pool.SafeRelease(); SRV.SafeRelease(); FVertexBuffer::ReleaseRHI();
    }
};

class FHUMAClothGpuProxy final : public FPrimitiveSceneProxy
{
public:
    FHUMAClothGpuProxy(UHUMAClothGpuMeshComponent* Component, const UHUMAClothBindAsset& Asset)
        : FPrimitiveSceneProxy(Component), State(Component->GetGPUState()), Factory(GetScene().GetFeatureLevel()), Relevance(Component->GetMaterialRelevance(GetScene().GetFeatureLevel()))
    {
        bAlwaysHasVelocity = true;
        Positions.State = State; PreviousPositions.State = State; PreviousPositions.bPrevious = true;
        Tangents.State = State; Tangents.bTangent = true;
        TArray<FDynamicMeshVertex> Vertices;
        for (int32 Index = 0; Index < State->Rest.Num(); ++Index)
        {
            FDynamicMeshVertex Vertex;
            Vertex.Position = State->Rest[Index]; Vertex.TextureCoordinate[0] = State->UV[Index]; Vertex.Color = FColor::White;
            Vertex.SetTangents(FVector3f::XAxisVector, FVector3f::YAxisVector, FVector3f::ZAxisVector);
            Vertices.Add(Vertex);
        }
        StaticBuffers.InitFromDynamicVertex(&Factory, Vertices, 1);
        for (uint32 Index : State->Triangles) Indices.Indices.Add(Index);
        Counts = Asset.SectionIndexCounts;
        for (int32 Section = 0; Section < Counts.Num(); ++Section)
        {
            UMaterialInterface* Material = Component->GetMaterial(Section);
            Materials.Add(Material ? Material : UMaterial::GetDefaultMaterial(MD_Surface));
        }
        BeginInitResource(&StaticBuffers.PositionVertexBuffer);
        BeginInitResource(&StaticBuffers.StaticMeshVertexBuffer);
        BeginInitResource(&StaticBuffers.ColorVertexBuffer);
        BeginInitResource(&Positions); BeginInitResource(&PreviousPositions); BeginInitResource(&Tangents); BeginInitResource(&Indices);
        ENQUEUE_RENDER_COMMAND(HUMAClothFactory)([this](FRHICommandListImmediate& RHICmdList)
        {
            FLocalVertexFactory::FDataType Data;
            Data.PositionComponent = FVertexStreamComponent(&Positions, 0, 12, VET_Float3);
            Data.PositionComponentSRV = Positions.SRV;
            Data.PreSkinPositionComponent = FVertexStreamComponent(&StaticBuffers.PositionVertexBuffer, 0, sizeof(FVector3f), VET_Float3);
            Data.PreSkinPositionComponentSRV = StaticBuffers.PositionVertexBuffer.GetSRV();
            Data.TangentBasisComponents[0] = FVertexStreamComponent(&Tangents, 0, 8, VET_PackedNormal);
            Data.TangentBasisComponents[1] = FVertexStreamComponent(&Tangents, 4, 8, VET_PackedNormal);
            Data.TangentsSRV = Tangents.SRV;
            StaticBuffers.StaticMeshVertexBuffer.BindPackedTexCoordVertexBuffer(&Factory, Data);
            StaticBuffers.ColorVertexBuffer.BindColorVertexBuffer(&Factory, Data);
            Factory.SetData(RHICmdList, Data);
            Factory.InitResource(RHICmdList);
            Factory.UpdateVelocity(RHICmdList, MAX_uint32, Positions.SRV, PreviousPositions.SRV, StaticBuffers.StaticMeshVertexBuffer.GetTangentsSRV());
            FScopeLock Lock(&State->Mutex);
            State->VelocityFactory = &Factory;
            State->CurrentPositionSRV = Positions.SRV; State->PreviousPositionSRV = PreviousPositions.SRV;
            State->RestTangentSRV = StaticBuffers.StaticMeshVertexBuffer.GetTangentsSRV();
        });
    }
    virtual ~FHUMAClothGpuProxy() override
    {
        {
            FScopeLock Lock(&State->Mutex);
            if (State->VelocityFactory == &Factory)
            {
                State->VelocityFactory = nullptr; State->CurrentPositionSRV.SafeRelease();
                State->PreviousPositionSRV.SafeRelease(); State->RestTangentSRV.SafeRelease();
            }
        }
        Factory.ReleaseResource(); Positions.ReleaseResource(); PreviousPositions.ReleaseResource(); Tangents.ReleaseResource(); Indices.ReleaseResource();
        StaticBuffers.PositionVertexBuffer.ReleaseResource(); StaticBuffers.StaticMeshVertexBuffer.ReleaseResource(); StaticBuffers.ColorVertexBuffer.ReleaseResource();
    }
    virtual SIZE_T GetTypeHash() const override { static uint8 Type; return reinterpret_cast<SIZE_T>(&Type); }
    virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + FPrimitiveSceneProxy::GetAllocatedSize(); }
    virtual bool CanBeOccluded() const override { return false; }
    virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
    {
        FPrimitiveViewRelevance Result;
        Result.bDrawRelevance = IsShown(View); Result.bDynamicRelevance = true; Result.bRenderInMainPass = ShouldRenderInMainPass();
        Relevance.SetPrimitiveViewRelevance(Result);
        Result.bRenderInDepthPass = ShouldRenderInDepthPass();
        Result.bVelocityRelevance = DrawsVelocity() && (Result.bOpaque || Result.bMasked) && Result.bRenderInMainPass;
        return Result;
    }
    virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& Family, uint32 Visibility, FMeshElementCollector& Collector) const override
    {
        if (!State->bReady) return;
        for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
        {
            if (!(Visibility & (1u << ViewIndex))) continue;
            int32 Offset = 0;
            for (int32 Section = 0; Section < Counts.Num(); ++Section)
            {
                FMeshBatch& Mesh = Collector.AllocateMesh();
                Mesh.VertexFactory = &Factory; Mesh.MaterialRenderProxy = Materials[Section]->GetRenderProxy();
                Mesh.Type = PT_TriangleList; Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative(); Mesh.DepthPriorityGroup = SDPG_World;
                Mesh.CastShadow = false;
                auto& Element = Mesh.Elements[0]; Element.IndexBuffer = &Indices; Element.FirstIndex = Offset;
                Element.NumPrimitives = Counts[Section] / 3; Element.MinVertexIndex = 0; Element.MaxVertexIndex = State->Rest.Num() - 1;
                auto& Uniform = Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
                FPrimitiveUniformShaderParametersBuilder Builder;
                BuildUniformShaderParameters(Builder);
                Uniform.Set(Collector.GetRHICommandList(), Builder);
                Element.PrimitiveUniformBufferResource = &Uniform.UniformBuffer;
                Collector.AddMesh(ViewIndex, Mesh);
                Offset += Counts[Section];
            }
        }
    }
private:
    TSharedPtr<FHUMAClothGpuState, ESPMode::ThreadSafe> State;
    FStaticMeshVertexBuffers StaticBuffers;
    FHUMAOutputVertexBuffer Positions, PreviousPositions, Tangents;
    FDynamicMeshIndexBuffer32 Indices;
    FHUMAClothVelocityVertexFactory Factory;
    FMaterialRelevance Relevance;
    TArray<int32> Counts;
    TArray<UMaterialInterface*> Materials;
};

UHUMAClothGpuMeshComponent::UHUMAClothGpuMeshComponent()
{
    SetMobility(EComponentMobility::Movable);
    SetCollisionEnabled(ECollisionEnabled::NoCollision);
    CastShadow = false;
}

void UHUMAClothGpuMeshComponent::Initialize(UHUMAClothBindAsset* Asset, USkeletalMeshComponent* Source,
    TConstArrayView<int32> Mapping, bool bIncludeBodyLODs)
{
    BindAsset = Asset; SourceBody = Source;
    State = MakeShared<FHUMAClothGpuState, ESPMode::ThreadSafe>(*Asset, Mapping, bIncludeBodyLODs);
    State->Scene = GetWorld()->Scene; State->PrimitiveId = Source->GetPrimitiveSceneId();
    InitializeMaterials();
}

void UHUMAClothGpuMeshComponent::Initialize(UHUMAClothBindAsset* Asset, USkeletalMeshComponent* Source,
    const FHUMAClothSourceLODs& Sources)
{
    BindAsset = Asset; SourceBody = Source;
    State = MakeShared<FHUMAClothGpuState, ESPMode::ThreadSafe>(*Asset, Sources);
    State->Scene = GetWorld()->Scene; State->PrimitiveId = Source->GetPrimitiveSceneId();
    InitializeMaterials();
}

void UHUMAClothGpuMeshComponent::InitializeMaterials()
{
    for (int32 Section = 0; Section < BindAsset->SectionMaterials.Num(); ++Section)
    {
        const int32 Material = BindAsset->SectionMaterials[Section];
        if (UStaticMesh* Mesh = Cast<UStaticMesh>(BindAsset->GarmentMesh)) SetMaterial(Section, Mesh->GetMaterial(Material));
        if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(BindAsset->GarmentMesh)) SetMaterial(Section, Mesh->GetMaterials().IsValidIndex(Material) ? Mesh->GetMaterials()[Material].MaterialInterface : nullptr);
    }
}

FPrimitiveSceneProxy* UHUMAClothGpuMeshComponent::CreateSceneProxy()
{
    return BindAsset && State ? new FHUMAClothGpuProxy(this, *BindAsset) : nullptr;
}

FBoxSphereBounds UHUMAClothGpuMeshComponent::CalcBounds(const FTransform& LocalToWorld) const
{
    return FBoxSphereBounds(FBox(FVector(-5000), FVector(5000))).TransformBy(LocalToWorld);
}

int32 UHUMAClothGpuMeshComponent::GetNumMaterials() const
{
    return BindAsset ? BindAsset->SectionMaterials.Num() : 0;
}