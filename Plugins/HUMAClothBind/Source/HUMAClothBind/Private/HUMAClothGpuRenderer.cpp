#include "HUMAClothGpuState.h"
#include "HUMAClothSourceTransfer.h"
#include "HUMAClothBatch.h"
#include "HUMAClothVelocityVertexFactory.h"
#include "CoreGlobals.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Engine/Engine.h"
#include "Dom/JsonObject.h"
#include "GlobalShader.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "PrimitiveSceneInfo.h"
#include "PrimitiveSceneProxy.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SceneInterface.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "ShaderParameterStruct.h"
#include "SkeletalMeshDeformerHelpers.h"
#include "SkeletalMeshSceneProxy.h"
#include "SkeletalRenderPublic.h"
#if ENGINE_MINOR_VERSION >= 7
#include "SkinningSceneExtensionProxy.h"
#endif
#include "Serialization/JsonSerializer.h"
#include "HAL/IConsoleManager.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"

CSV_DEFINE_CATEGORY(HUMACloth, true);
DECLARE_GPU_STAT_NAMED(HUMAClothBodyNormals, TEXT("HUMACloth BodyNormals"));
DECLARE_GPU_STAT_NAMED(HUMAClothAnchors, TEXT("HUMACloth Anchors"));
DECLARE_GPU_STAT_NAMED(HUMAClothDeform, TEXT("HUMACloth Deform"));
DECLARE_GPU_STAT_NAMED(HUMAClothTangents, TEXT("HUMACloth Tangents"));
DECLARE_GPU_STAT_NAMED(HUMAClothCopies, TEXT("HUMACloth Copies"));

class FHUMAClothSoftBindCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FHUMAClothSoftBindCS);
    SHADER_USE_PARAMETER_STRUCT(FHUMAClothSoftBindCS, FGlobalShader);
    class FPass : SHADER_PERMUTATION_INT("HUMA_PASS", 5);
    using FPermutationDomain = TShaderPermutationDomain<FPass>;
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, BodyCount)
        SHADER_PARAMETER(uint32, AnchorCount)
        SHADER_PARAMETER(uint32, ClothCount)
        SHADER_PARAMETER(uint32, Slots)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, BodyPositions)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, Mapping)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, Quads)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BodyOffsets)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BodyIncidents)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AnchorTriangles)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, Bary)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, Rest)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, RestPosition)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, RestTangent)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, RestBitangent)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, RestNormal)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, InfluenceAnchors)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, InfluenceWeights)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, Triangles)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ClothOffsets)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ClothIncidents)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float2>, UV)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, BodyNormals)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float3>, Anchors)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, Positions)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, PreviousPositions)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, OutBodyNormals)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, OutAnchors)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutPositions)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutTangents)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float3>, OutValidation)
    END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FHUMAClothSoftBindCS, "/Plugin/HUMAClothBind/Private/HUMAClothSoftBind.usf", "SoftBindCS", SF_Compute);

namespace
{
TAutoConsoleVariable<int32> Optimization(TEXT("r.HUMACloth.Optimization"), 2,
    TEXT("Cloth update path: 0=legacy rollback, 1=pass-specific setup, 2=body-group batching (default)."), ECVF_RenderThreadSafe);
TArray<TSharedPtr<FHUMAClothGpuState, ESPMode::ThreadSafe>> States;
TArray<TWeakPtr<const FHUMAClothBodyTopology, ESPMode::ThreadSafe>> BodyTopologies;
TSharedPtr<FSceneViewExtensionBase, ESPMode::ThreadSafe> Extension;
FDelegateHandle EngineInitHandle;
struct FBodyInput { FSkeletalMeshObject* Mesh = nullptr; int32 LOD = INDEX_NONE; FRDGBufferRef Positions = nullptr; };
struct FBodyNormals { FRDGBufferRef Buffer = nullptr; int32 Group = INDEX_NONE; };

struct FBatchCache
{
    FSceneInterface* Scene = nullptr;
    TArray<TWeakPtr<FHUMAClothGpuState, ESPMode::ThreadSafe>> Members;
    TArray<const FHUMAClothGpuState*> Data;
    TSharedPtr<FHUMAClothGpuState, ESPMode::ThreadSafe> Tables;
    TArray<FHUMAClothBatchRange> Ranges;
};
TArray<FBatchCache> BatchCache;

void ReadSnapshot(FHUMAClothGpuState& State)
{
    if (!State.bSnapshotPending || !State.bRenderedSnapshotPending || !State.SnapshotReadback->IsReady()
        || !State.RenderedSnapshotReadback->IsReady() || !State.TangentSnapshotReadback || !State.TangentSnapshotReadback->IsReady()) return;
    const uint32 Count = State.SnapshotBodyCount + State.Rest.Num() * 2;
    const FVector3f* Positions = static_cast<const FVector3f*>(State.SnapshotReadback->Lock(Count * sizeof(FVector3f)));
    const FVector3f* RenderedBody = static_cast<const FVector3f*>(State.RenderedSnapshotReadback->Lock(State.SnapshotBodyCount * sizeof(FVector3f)));
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    auto SetVertices = [&](const TCHAR* Name, const FVector3f* Source, uint32 First, uint32 Last)
    {
        TArray<TSharedPtr<FJsonValue>> Vertices;
        Vertices.Reserve(Last - First);
        for (uint32 Index = First; Index < Last; ++Index)
        {
            const FVector3f Point = Source[Index];
            TArray<TSharedPtr<FJsonValue>> Row;
            Row.Add(MakeShared<FJsonValueNumber>(Point.X)); Row.Add(MakeShared<FJsonValueNumber>(Point.Y)); Row.Add(MakeShared<FJsonValueNumber>(Point.Z));
            Vertices.Add(MakeShared<FJsonValueArray>(Row));
        }
        Object->SetArrayField(Name, Vertices);
    };
    SetVertices(TEXT("body_positions"), Positions, 0, State.SnapshotBodyCount);
    SetVertices(TEXT("cloth_positions"), Positions, State.SnapshotBodyCount, State.SnapshotBodyCount + State.Rest.Num());
    SetVertices(TEXT("cloth_previous_positions"), Positions, State.SnapshotBodyCount + State.Rest.Num(), Count);
    SetVertices(TEXT("rendered_body_positions"), RenderedBody, 0, State.SnapshotBodyCount);
    Object->SetNumberField(TEXT("body_lod"), State.SnapshotLOD);
    const auto* SourceData = State.GetLODData(State.SnapshotLOD);
    Object->SetNumberField(TEXT("bind_body_lod"), SourceData->BindBodyLOD);
    Object->SetBoolField(TEXT("source_lod_transfer"), SourceData->SourceTransfer.IsValid());
    if (SourceData->SourceTransfer)
    {
        Object->SetNumberField(TEXT("source_render_vertices"), SourceData->SourceTransfer->SourceVertexCount);
        Object->SetNumberField(TEXT("source_projection_max_cm"), SourceData->SourceTransfer->MaxRestDistanceCm);
    }
    Object->SetNumberField(TEXT("source_component_id"), State.PrimitiveId.PrimIDValue);
    Object->SetNumberField(TEXT("body_normal_evaluation"), double(State.SnapshotFamily));
    Object->SetNumberField(TEXT("body_normal_group"), State.SnapshotBodyNormalGroup);
    Object->SetStringField(TEXT("body_topology_signature"), FString::Printf(TEXT("%08x"), State.SnapshotBodyTopologySignature));
    Object->SetBoolField(TEXT("body_normals_reused"), State.bSnapshotBodyNormalsReused);
    Object->SetNumberField(TEXT("velocity_frame"), double(State.SnapshotFrame));
    Object->SetBoolField(TEXT("velocity_history_reset"), State.bSnapshotHistoryReset);
    Object->SetNumberField(TEXT("optimization_path"), State.SnapshotOptimizationPath);
    Object->SetNumberField(TEXT("batch_parts"), State.SnapshotBatchParts);
    Object->SetNumberField(TEXT("compute_dispatches"), State.SnapshotComputeDispatches);
    const uint32 TangentCount = State.Rest.Num() * 2;
    const auto* Packed = static_cast<const uint32*>(State.TangentSnapshotReadback->Lock(TangentCount * sizeof(uint32)));
    TArray<TSharedPtr<FJsonValue>> PackedValues;
    PackedValues.Reserve(TangentCount);
    for (uint32 Index = 0; Index < TangentCount; ++Index) PackedValues.Add(MakeShared<FJsonValueNumber>(double(Packed[Index])));
    Object->SetArrayField(TEXT("packed_tangents"), PackedValues);
    State.TangentSnapshotReadback->Unlock();
    Object->SetBoolField(TEXT("collision"), false);
    State.SnapshotReadback->Unlock();
    State.RenderedSnapshotReadback->Unlock();
    FString Text;
    FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
    FScopeLock Lock(&State.Mutex);
    if (!FFileHelper::SaveStringToFile(Text, *State.SnapshotPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) UE_LOG(LogTemp, Error, TEXT("HUMAClothBind snapshot write failed: %s"), *State.SnapshotPath);
    State.bSnapshotPending = false;
    State.bRenderedSnapshotPending = false;
}

template<typename ValueType>
FRDGBufferRef Immutable(FRDGBuilder& Graph, const TCHAR* Name, const TArray<ValueType>& Values, TRefCountPtr<FRDGPooledBuffer>& Pool)
{
    if (!Pool)
    {
        FRDGBufferRef Buffer = CreateStructuredBuffer(Graph, Name, sizeof(ValueType), Values.Num(), Values.GetData(), Values.Num() * sizeof(ValueType));
        Pool = Graph.ConvertToExternalBuffer(Buffer);
    }
    return Graph.RegisterExternalBuffer(Pool, Name);
}

TArray<FRDGBufferRef> PrepareBuffers(FRDGBuilder& Graph, FHUMAClothGpuState& Data)
{
    Data.Immutable.SetNum(17);
    TArray<FRDGBufferRef> Static;
    Static.SetNum(17);
    const auto& Body = *Data.BodyTopology;
    Static[0] = Immutable(Graph, TEXT("HUMACloth.Mapping"), Body.Mapping, Body.Immutable[0]);
    Static[1] = Immutable(Graph, TEXT("HUMACloth.Quads"), Body.Quads, Body.Immutable[1]);
    Static[2] = Immutable(Graph, TEXT("HUMACloth.BodyOffsets"), Body.Offsets, Body.Immutable[2]);
    Static[3] = Immutable(Graph, TEXT("HUMACloth.BodyIncidents"), Body.Incidents, Body.Immutable[3]);
    for (int32 Index = 0; Index < 4; ++Index) Data.Immutable[Index] = Body.Immutable[Index];
    Static[4] = Immutable(Graph, TEXT("HUMACloth.AnchorTriangles"), Data.AnchorTriangles, Data.Immutable[4]);
    Static[5] = Immutable(Graph, TEXT("HUMACloth.Bary"), Data.Bary, Data.Immutable[5]);
    Static[6] = Immutable(Graph, TEXT("HUMACloth.Rest"), Data.Rest, Data.Immutable[6]);
    Static[7] = Immutable(Graph, TEXT("HUMACloth.RestPosition"), Data.RestPosition, Data.Immutable[7]);
    Static[8] = Immutable(Graph, TEXT("HUMACloth.RestTangent"), Data.RestTangent, Data.Immutable[8]);
    Static[9] = Immutable(Graph, TEXT("HUMACloth.RestBitangent"), Data.RestBitangent, Data.Immutable[9]);
    Static[10] = Immutable(Graph, TEXT("HUMACloth.RestNormal"), Data.RestNormal, Data.Immutable[10]);
    Static[11] = Immutable(Graph, TEXT("HUMACloth.InfluenceAnchors"), Data.InfluenceAnchors, Data.Immutable[11]);
    Static[12] = Immutable(Graph, TEXT("HUMACloth.InfluenceWeights"), Data.InfluenceWeights, Data.Immutable[12]);
    Static[13] = Immutable(Graph, TEXT("HUMACloth.Triangles"), Data.Triangles, Data.Immutable[13]);
    Static[14] = Immutable(Graph, TEXT("HUMACloth.ClothOffsets"), Data.ClothOffsets, Data.Immutable[14]);
    Static[15] = Immutable(Graph, TEXT("HUMACloth.ClothIncidents"), Data.ClothIncidents, Data.Immutable[15]);
    Static[16] = Immutable(Graph, TEXT("HUMACloth.UV"), Data.UV, Data.Immutable[16]);
    return Static;
}

struct FPassViews
{
    TStaticArray<FRDGBufferSRVRef, 17> Static;
    FRDGBufferSRVRef Body = nullptr, Normals = nullptr, Anchors = nullptr, Positions = nullptr, Previous = nullptr;
    FRDGBufferUAVRef OutNormals = nullptr, OutAnchors = nullptr, OutPositions = nullptr, OutTangents = nullptr, Validation = nullptr;
};

FPassViews MakeViews(FRDGBuilder& Graph, TConstArrayView<FRDGBufferRef> Static, FRDGBufferRef Body,
    FRDGBufferRef Normals, FRDGBufferRef Anchors, FRDGBufferRef Positions, FRDGBufferRef Previous,
    FRDGBufferRef Tangents, FRDGBufferRef Validation)
{
    FPassViews Views{};
    for (int32 Index = 0; Index < 17; ++Index) Views.Static[Index] = Graph.CreateSRV(Static[Index]);
    Views.Body = Graph.CreateSRV(Body, PF_R32_FLOAT);
    Views.Normals = Graph.CreateSRV(Normals);
    if (Anchors) Views.Anchors = Graph.CreateSRV(Anchors);
    Views.Positions = Graph.CreateSRV(Positions, PF_R32_FLOAT);
    if (Previous) Views.Previous = Graph.CreateSRV(Previous, PF_R32_FLOAT);
    Views.OutNormals = Graph.CreateUAV(Normals);
    if (Anchors) Views.OutAnchors = Graph.CreateUAV(Anchors);
    Views.OutPositions = Graph.CreateUAV(Positions, PF_R32_FLOAT);
    Views.OutTangents = Graph.CreateUAV(Tangents, PF_R32_UINT);
    if (Validation) Views.Validation = Graph.CreateUAV(Validation);
    return Views;
}

void BindPass(FHUMAClothSoftBindCS::FParameters& P, const FPassViews& V, int32 Pass, bool Legacy)
{
    if (Legacy || Pass == 0 || Pass == 1 || Pass == 4) { P.BodyPositions = V.Body; P.Mapping = V.Static[0]; }
    if (Legacy || Pass == 0)
    { P.Quads = V.Static[1]; P.BodyOffsets = V.Static[2]; P.BodyIncidents = V.Static[3]; P.OutBodyNormals = V.OutNormals; }
    if (Legacy || Pass == 1)
    { P.AnchorTriangles = V.Static[4]; P.Bary = V.Static[5]; P.BodyNormals = V.Normals; P.OutAnchors = V.OutAnchors; }
    if (Legacy || Pass == 2)
    {
        P.Rest = V.Static[6]; P.RestPosition = V.Static[7]; P.RestTangent = V.Static[8];
        P.RestBitangent = V.Static[9]; P.RestNormal = V.Static[10]; P.InfluenceAnchors = V.Static[11];
        P.InfluenceWeights = V.Static[12]; P.Anchors = V.Anchors; P.OutPositions = V.OutPositions;
    }
    if (Legacy || Pass == 3)
    { P.Triangles = V.Static[13]; P.ClothOffsets = V.Static[14]; P.ClothIncidents = V.Static[15]; P.UV = V.Static[16]; P.OutTangents = V.OutTangents; }
    if (Legacy || Pass == 3 || Pass == 4) P.Positions = V.Positions;
    if (Legacy || Pass == 4) { P.PreviousPositions = V.Previous; P.OutValidation = V.Validation; }
}

void Dispatch(FRDGBuilder& Graph, FHUMAClothSoftBindCS::FParameters* Parameters, int32 Pass, uint32 Count,
    ERHIFeatureLevel::Type FeatureLevel)
{
    FHUMAClothSoftBindCS::FPermutationDomain Permutation;
    Permutation.Set<FHUMAClothSoftBindCS::FPass>(Pass);
    TShaderMapRef<FHUMAClothSoftBindCS> Shader(GetGlobalShaderMap(FeatureLevel), Permutation);
    ClearUnusedGraphResources(Shader, Parameters);
    auto Add = [&] { FComputeShaderUtils::AddPass(Graph, RDG_EVENT_NAME("HUMACloth GPU SoftBind %d", Pass), Shader, Parameters, FComputeShaderUtils::GetGroupCount(Count, 256)); };
    if (Pass == 1) { RDG_EVENT_SCOPE_STAT(Graph, HUMAClothAnchors, "HUMACloth Anchors"); Add(); }
    else if (Pass == 2) { RDG_EVENT_SCOPE_STAT(Graph, HUMAClothDeform, "HUMACloth Deform"); Add(); }
    else if (Pass == 3) { RDG_EVENT_SCOPE_STAT(Graph, HUMAClothTangents, "HUMACloth Tangents"); Add(); }
    else Add();
}

FSkeletalMeshObject* FindBodyMesh(FSceneInterface* Scene, FPrimitiveComponentId PrimitiveId)
{
    if (const FPrimitiveSceneInfo* Info = Scene->GetPrimitiveSceneInfo(PrimitiveId))
    {
        if (const FPrimitiveSceneProxy* Proxy = Info->Proxy)
        {
#if ENGINE_MINOR_VERSION >= 7
            if (const FSkinningSceneExtensionProxy* Skinning = Proxy->GetSkinningSceneExtensionProxy())
                return const_cast<FSkeletalMeshObject*>(Skinning->GetMeshObject());
            else
#endif
            if (Proxy->IsSkinnedMesh() && !Proxy->IsNaniteMesh())
                return const_cast<FSkeletalMeshObject*>(static_cast<const FSkeletalMeshSceneProxy*>(Proxy)->GetMeshObject());
        }
    }
    return nullptr;
}

void InternBodyTopology(FHUMAClothGpuState& Data)
{
    for (int32 Index = BodyTopologies.Num() - 1; Index >= 0; --Index)
    {
        const auto Existing = BodyTopologies[Index].Pin();
        if (!Existing) { BodyTopologies.RemoveAtSwap(Index); continue; }
        if (Existing->IsCompatible(*Data.BodyTopology)) { Data.BodyTopology = Existing; return; }
    }
    BodyTopologies.Add(Data.BodyTopology);
}

void CaptureRenderedBody(FRDGBuilder& Graph, FSceneInterface* Scene)
{
    FHUMAClothTransferInputs TransferInputs;
    for (const auto& State : States)
    {
        if (State->Scene != Scene || !State->bSnapshotPending || State->bRenderedSnapshotPending) continue;
        FSkeletalMeshObject* Mesh = FindBodyMesh(Scene, State->PrimitiveId);
        FHUMAClothGpuState* Data = State->GetLODData(State->SnapshotLOD);
        FRDGBufferRef Body = Mesh && Data && Mesh->GetLOD() == State->SnapshotLOD ? FSkeletalMeshDeformerHelpers::GetAllocatedPositionBuffer(Graph, Mesh, State->SnapshotLOD) : nullptr;
        if (Body) Body = ResolveHUMAClothSourceLOD(Graph, Body, Data->SourceTransfer.Get(), State->PrimitiveId, Scene->GetFeatureLevel(), TransferInputs);
        TRefCountPtr<FRDGPooledBuffer> PositionPool;
        { FScopeLock Lock(&State->Mutex); PositionPool = State->Positions; }
        if (!Body || !PositionPool) continue;
        const uint32 BodyCount = Data->BodyTopology->Mapping.Num();
        FRDGBufferDesc OutputDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), BodyCount);
        OutputDesc.Usage |= BUF_SourceCopy;
        FRDGBufferRef Output = Graph.CreateBuffer(OutputDesc, TEXT("HUMACloth.RenderedBodyValidationOnly"));
        auto* Parameters = Graph.AllocParameters<FHUMAClothSoftBindCS::FParameters>();
        Parameters->BodyCount = BodyCount; Parameters->ClothCount = 0;
        Parameters->BodyPositions = Graph.CreateSRV(Body, PF_R32_FLOAT);
        Parameters->Mapping = Graph.CreateSRV(Graph.RegisterExternalBuffer(Data->Immutable[0]));
        Parameters->Positions = Graph.CreateSRV(Graph.RegisterExternalBuffer(PositionPool), PF_R32_FLOAT);
        Parameters->PreviousPositions = Parameters->Positions;
        Parameters->OutValidation = Graph.CreateUAV(Output);
        FHUMAClothSoftBindCS::FPermutationDomain Permutation;
        Permutation.Set<FHUMAClothSoftBindCS::FPass>(4);
        TShaderMapRef<FHUMAClothSoftBindCS> Shader(GetGlobalShaderMap(Scene->GetFeatureLevel()), Permutation);
        ClearUnusedGraphResources(Shader, Parameters);
        FComputeShaderUtils::AddPass(Graph, RDG_EVENT_NAME("HUMACloth Validate Rendered Body"), Shader, Parameters, FComputeShaderUtils::GetGroupCount(BodyCount, 256));
        if (!State->RenderedSnapshotReadback) State->RenderedSnapshotReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("HUMACloth.RenderedBodyValidationOnly"));
        AddEnqueueCopyPass(Graph, State->RenderedSnapshotReadback.Get(), Output, BodyCount * sizeof(FVector3f));
        State->bRenderedSnapshotPending = true;
    }
}

BEGIN_SHADER_PARAMETER_STRUCT(FBatchCopyParameters, )
    RDG_BUFFER_ACCESS_ARRAY(Buffers)
END_SHADER_PARAMETER_STRUCT()

struct FBatchCopy
{
    FRDGBufferRef Source = nullptr, Destination = nullptr;
    uint64 SourceOffset = 0, DestinationOffset = 0, Bytes = 0;
};

void CopyBatch(FRDGBuilder& Graph, TArray<FBatchCopy>&& Copies, const TCHAR* Label)
{
    if (Copies.IsEmpty()) return;
    RDG_EVENT_SCOPE_STAT(Graph, HUMAClothCopies, "HUMACloth Copies");
    auto* Parameters = Graph.AllocParameters<FBatchCopyParameters>();
    for (const FBatchCopy& Copy : Copies)
    {
        Parameters->Buffers.Emplace(Copy.Source, ERHIAccess::CopySrc);
        Parameters->Buffers.Emplace(Copy.Destination, ERHIAccess::CopyDest);
    }
    Graph.AddPass(RDG_EVENT_NAME("HUMACloth %s (%d copies)", Label, Copies.Num()), Parameters, ERDGPassFlags::Copy,
        [Copies = MoveTemp(Copies)](FRHICommandList& Commands)
        {
            for (const FBatchCopy& Copy : Copies)
                Commands.CopyBufferRegion(Copy.Destination->GetRHI(), Copy.DestinationOffset, Copy.Source->GetRHI(), Copy.SourceOffset, Copy.Bytes);
        });
}

struct FBatchMember
{
    TSharedPtr<FHUMAClothGpuState, ESPMode::ThreadSafe> State;
    FHUMAClothGpuState* Data = nullptr;
    TRefCountPtr<FRDGPooledBuffer> PositionPool, PreviousPool, TangentPool;
};

struct FBatchWork
{
    FHUMAClothBodyNormalKey Key;
    FRDGBufferRef Body = nullptr;
    uint32 Slots = 0;
    TArray<FBatchMember> Parts;
};

struct FBatchResult
{
    FBodyNormals Normals;
    int32 Parts = 0;
    int32 Dispatches = 0;
    bool ReusedNormals = false;
};

TMap<FHUMAClothGpuState*, FBatchResult> PrepareBatches(FRDGBuilder& Graph, FSceneInterface* Scene,
    uint64 FamilySerial, uint64 FrameCounter, TMap<FPrimitiveComponentId, FBodyInput>& BodyInputs,
    TMap<FHUMAClothBodyNormalKey, FBodyNormals>& SharedNormals, FHUMAClothTransferInputs& TransferInputs)
{
    CSV_SCOPED_TIMING_STAT(HUMACloth, BatchPrepare);
    TMap<FHUMAClothGpuState*, FBatchResult> Results;
    TArray<FBatchWork> Work;
    for (const auto& State : States)
    {
        if (State->Scene != Scene || State->LastFrame == FamilySerial || !State->VelocityFactory) continue;
        FBatchMember Member;
        Member.State = State;
        {
            FScopeLock Lock(&State->Mutex);
            Member.PositionPool = State->Positions; Member.PreviousPool = State->PreviousPositions; Member.TangentPool = State->Tangents;
        }
        if (!Member.PositionPool || !Member.PreviousPool || !Member.TangentPool) continue;
        FBodyInput* Input = BodyInputs.Find(State->PrimitiveId);
        if (!Input)
        {
            FBodyInput Resolved;
            Resolved.Mesh = FindBodyMesh(Scene, State->PrimitiveId);
            if (Resolved.Mesh)
            {
                Resolved.LOD = Resolved.Mesh->GetLOD();
                Resolved.Positions = FSkeletalMeshDeformerHelpers::GetAllocatedPositionBuffer(Graph, Resolved.Mesh, Resolved.LOD);
            }
            Input = &BodyInputs.Add(State->PrimitiveId, Resolved);
        }
        Member.Data = State->GetLODData(Input->LOD);
        if (!Member.Data || !Input->Positions) continue;
        FRDGBufferRef Body = ResolveHUMAClothSourceLOD(Graph, Input->Positions, Member.Data->SourceTransfer.Get(),
            State->PrimitiveId, Scene->GetFeatureLevel(), TransferInputs);
        if (!Body) continue;
        const FHUMAClothBodyNormalKey Key{Scene, State->PrimitiveId, Input->LOD, Input->Mesh, Body, Member.Data->BodyTopology.Get()};
        auto* Group = Work.FindByPredicate([&](const FBatchWork& Item) { return Item.Key == Key && Item.Slots == State->Slots; });
        if (!Group)
        {
            auto& Added = Work.AddDefaulted_GetRef();
            Added.Key = Key; Added.Body = Body; Added.Slots = State->Slots;
            Group = &Added;
        }
        Group->Parts.Add(MoveTemp(Member));
    }
    TSet<FHUMAClothGpuState*> UsedTables;
    int32 Groups = 0, DispatchCount = 0, CopyCount = 0;
    for (const FBatchWork& Group : Work)
    {
        if (Group.Parts.Num() < 2) continue;
        auto* Cache = BatchCache.FindByPredicate([&](const FBatchCache& Item)
        {
            if (Item.Scene != Scene || Item.Members.Num() != Group.Parts.Num()) return false;
            for (int32 Index = 0; Index < Item.Members.Num(); ++Index)
                if (Item.Members[Index].Pin() != Group.Parts[Index].State || Item.Data[Index] != Group.Parts[Index].Data) return false;
            return true;
        });
        if (!Cache)
        {
            FBatchCache Added;
            Added.Scene = Scene;
            Added.Tables = MakeShared<FHUMAClothGpuState, ESPMode::ThreadSafe>();
            for (const auto& Part : Group.Parts) { Added.Members.Add(Part.State); Added.Data.Add(Part.Data); }
            FString Error;
            if (!BuildHUMAClothBatch(Added.Data, *Added.Tables, Added.Ranges, Error))
            {
                UE_LOG(LogTemp, Warning, TEXT("HUMA cloth batch rejected; using per-part setup: %s"), *Error);
                continue;
            }
            Cache = &BatchCache.Add_GetRef(MoveTemp(Added));
        }
        UsedTables.Add(Cache->Tables.Get());
        const auto Static = PrepareBuffers(Graph, *Cache->Tables);
        FBodyNormals NormalOutput;
        const FBodyNormals* Existing = SharedNormals.Find(Group.Key);
        if (Existing) NormalOutput = *Existing;
        else
        {
            RDG_EVENT_SCOPE_STAT(Graph, HUMAClothBodyNormals, "HUMACloth BodyNormals");
            NormalOutput.Buffer = Graph.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(12, Cache->Tables->BodyTopology->Mapping.Num()), TEXT("HUMACloth.BatchBodyNormals"));
            NormalOutput.Group = SharedNormals.Num();
            auto* P = Graph.AllocParameters<FHUMAClothSoftBindCS::FParameters>();
            P->BodyCount = Cache->Tables->BodyTopology->Mapping.Num();
            P->BodyPositions = Graph.CreateSRV(Group.Body, PF_R32_FLOAT);
            P->Mapping = Graph.CreateSRV(Static[0]); P->Quads = Graph.CreateSRV(Static[1]);
            P->BodyOffsets = Graph.CreateSRV(Static[2]); P->BodyIncidents = Graph.CreateSRV(Static[3]);
            P->OutBodyNormals = Graph.CreateUAV(NormalOutput.Buffer);
            Dispatch(Graph, P, 0, P->BodyCount, Scene->GetFeatureLevel());
            SharedNormals.Add(Group.Key, NormalOutput);
        }
        auto PositionDesc = FRDGBufferDesc::CreateBufferDesc(4, Cache->Tables->Rest.Num() * 3);
        auto TangentDesc = FRDGBufferDesc::CreateBufferDesc(4, Cache->Tables->Rest.Num() * 2);
        PositionDesc.Usage |= BUF_SourceCopy; TangentDesc.Usage |= BUF_SourceCopy;
        FRDGBufferRef PackedPositions = Graph.CreateBuffer(PositionDesc, TEXT("HUMACloth.BatchPositions"));
        FRDGBufferRef PackedTangents = Graph.CreateBuffer(TangentDesc, TEXT("HUMACloth.BatchTangents"));
        FRDGBufferRef Anchors = Graph.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(12, Cache->Tables->Bary.Num() * 4), TEXT("HUMACloth.BatchAnchors"));
        TArray<FBatchCopy> History, Scatter, Resets;
        for (int32 Index = 0; Index < Group.Parts.Num(); ++Index)
        {
            const auto& Part = Group.Parts[Index];
            const auto& Range = Cache->Ranges[Index];
            FRDGBufferRef Position = Graph.RegisterExternalBuffer(Part.PositionPool);
            FRDGBufferRef Previous = Graph.RegisterExternalBuffer(Part.PreviousPool);
            FRDGBufferRef Tangent = Graph.RegisterExternalBuffer(Part.TangentPool);
            Graph.UseInternalAccessMode(Position); Graph.UseInternalAccessMode(Previous); Graph.UseInternalAccessMode(Tangent);
            const bool NewFrame = Part.State->LastPositionFrame != FrameCounter;
            const bool Reset = !Part.State->bHistoryValid || (NewFrame && Part.State->LastPositionFrame + 1 != FrameCounter)
                || (Part.State->ActiveBodyLOD != INDEX_NONE && Part.State->ActiveBodyLOD != Group.Key.LOD);
            if (NewFrame && !Reset) History.Add({Position, Previous, 0, 0, uint64(Range.VertexCount) * 12});
            Scatter.Add({PackedPositions, Position, uint64(Range.FirstVertex) * 12, 0, uint64(Range.VertexCount) * 12});
            Scatter.Add({PackedTangents, Tangent, uint64(Range.FirstVertex) * 8, 0, uint64(Range.VertexCount) * 8});
            if (Reset) Resets.Add({Position, Previous, 0, 0, uint64(Range.VertexCount) * 12});
            Results.Add(Part.State.Get(), {NormalOutput, Group.Parts.Num(), Index == 0 ? (Existing ? 3 : 4) : 0, Index > 0 || Existing != nullptr});
        }
        CopyCount += History.Num() + Scatter.Num() + Resets.Num();
        CopyBatch(Graph, MoveTemp(History), TEXT("Batch history"));
        const auto Views = MakeViews(Graph, Static, Group.Body, NormalOutput.Buffer, Anchors, PackedPositions, nullptr, PackedTangents, nullptr);
        for (int32 Pass : {1, 2, 3})
        {
            auto* P = Graph.AllocParameters<FHUMAClothSoftBindCS::FParameters>();
            P->BodyCount = Cache->Tables->BodyTopology->Mapping.Num(); P->AnchorCount = Cache->Tables->Bary.Num();
            P->ClothCount = Cache->Tables->Rest.Num(); P->Slots = Cache->Tables->Slots;
            BindPass(*P, Views, Pass, false);
            Dispatch(Graph, P, Pass, Pass == 1 ? P->AnchorCount : P->ClothCount, Scene->GetFeatureLevel());
        }
        CopyBatch(Graph, MoveTemp(Scatter), TEXT("Batch scatter"));
        CopyBatch(Graph, MoveTemp(Resets), TEXT("Batch history reset"));
        ++Groups;
        DispatchCount += Existing ? 3 : 4;
    }
    BatchCache.RemoveAll([&](const FBatchCache& Item) { return Item.Scene == Scene && !UsedTables.Contains(Item.Tables.Get()); });
    CSV_CUSTOM_STAT(HUMACloth, BatchGroups, Groups, ECsvCustomStatOp::Set);
    CSV_CUSTOM_STAT(HUMACloth, BatchDispatches, DispatchCount, ECsvCustomStatOp::Set);
    CSV_CUSTOM_STAT(HUMACloth, BatchRHICopies, CopyCount, ECsvCustomStatOp::Set);
    return Results;
}

void Update(FRDGBuilder& Graph, FSceneInterface* Scene, bool bBeforeDepth, uint64 FamilySerial, uint64 FrameCounter)
{
    CSV_SCOPED_TIMING_STAT(HUMACloth, Prepare);
    const int32 Path = Optimization.GetValueOnRenderThread();
    if (Path < 0 || Path > 2) { UE_LOG(LogTemp, Error, TEXT("HUMA cloth optimization path must be 0, 1 or 2.")); return; }
    TMap<FPrimitiveComponentId, FBodyInput> BodyInputs;
    FHUMAClothTransferInputs TransferInputs;
    // RDG refs never escape this evaluation. Another view family/graph resolves the current input again.
    TMap<FHUMAClothBodyNormalKey, FBodyNormals> SharedNormals;
    TMap<FHUMAClothGpuState*, FBatchResult> Batched;
    if (Path == 2) Batched = PrepareBatches(Graph, Scene, FamilySerial, FrameCounter, BodyInputs, SharedNormals, TransferInputs);
    else BatchCache.Reset();
    for (const auto& State : States)
    {
        ReadSnapshot(*State);
        if (State->Scene != Scene || State->LastFrame == FamilySerial) continue;
        TRefCountPtr<FRDGPooledBuffer> PositionPool, PreviousPool, TangentPool;
        {
            FScopeLock Lock(&State->Mutex);
            PositionPool = State->Positions; PreviousPool = State->PreviousPositions; TangentPool = State->Tangents;
        }
        if (!PositionPool || !PreviousPool || !TangentPool || !State->VelocityFactory) continue;
        FBodyInput* Input = BodyInputs.Find(State->PrimitiveId);
        if (!Input)
        {
            FBodyInput Resolved;
            Resolved.Mesh = FindBodyMesh(Scene, State->PrimitiveId);
            if (Resolved.Mesh)
            {
                Resolved.LOD = Resolved.Mesh->GetLOD();
                Resolved.Positions = FSkeletalMeshDeformerHelpers::GetAllocatedPositionBuffer(Graph, Resolved.Mesh, Resolved.LOD);
            }
            Input = &BodyInputs.Add(State->PrimitiveId, Resolved);
        }
        FSkeletalMeshObject* Mesh = Input->Mesh;
        const int32 LODIndex = Input->LOD;
        FHUMAClothGpuState* Data = State->GetLODData(LODIndex);
        FRDGBufferRef Body = Data ? Input->Positions : nullptr;
        if (Body) Body = ResolveHUMAClothSourceLOD(Graph, Body, Data->SourceTransfer.Get(), State->PrimitiveId, Scene->GetFeatureLevel(), TransferInputs);
        if (!Body)
        {
            FScopeLock Lock(&State->Mutex);
            State->bReady = false;
            State->bHistoryValid = false;
            State->BodyNormalGroup = INDEX_NONE; State->BodyNormalEvaluation = FamilySerial;
            State->Status = Mesh ? FString::Printf(TEXT("Body LOD %d: %s (stage %s)"), LODIndex, Data ? TEXT("Final deformer output not allocated") : TEXT("Bind table missing"), bBeforeDepth ? TEXT("pre-depth") : TEXT("base-pass")) : TEXT("Source mesh render proxy not found");
            continue;
        }
        const FBatchResult* Batch = Batched.Find(State.Get());
        bool SnapshotRequested;
        { FScopeLock Lock(&State->Mutex); SnapshotRequested = State->bSnapshotRequested; }
        const auto Static = !Batch || SnapshotRequested ? PrepareBuffers(Graph, *Data) : TArray<FRDGBufferRef>();
        const uint32 BodyCount = Data->BodyTopology->Mapping.Num();
        const FHUMAClothBodyNormalKey Key{Scene, State->PrimitiveId, LODIndex, Mesh, Body, Data->BodyTopology.Get()};
        const FBodyNormals* Shared = SharedNormals.Find(Key);
        const bool bNormalsReused = Batch ? Batch->ReusedNormals : Shared != nullptr;
        FBodyNormals NormalOutput;
        if (Batch) NormalOutput = Batch->Normals;
        else if (Shared) NormalOutput = *Shared;
        else
        {
            RDG_EVENT_SCOPE_STAT(Graph, HUMAClothBodyNormals, "HUMACloth BodyNormals");
            NormalOutput.Buffer = Graph.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(12, BodyCount), TEXT("HUMACloth.BodyNormals"));
            NormalOutput.Group = SharedNormals.Num();
            auto* Parameters = Graph.AllocParameters<FHUMAClothSoftBindCS::FParameters>();
            Parameters->BodyCount = BodyCount;
            Parameters->BodyPositions = Graph.CreateSRV(Body, PF_R32_FLOAT);
            Parameters->Mapping = Graph.CreateSRV(Static[0]); Parameters->Quads = Graph.CreateSRV(Static[1]);
            Parameters->BodyOffsets = Graph.CreateSRV(Static[2]); Parameters->BodyIncidents = Graph.CreateSRV(Static[3]);
            Parameters->OutBodyNormals = Graph.CreateUAV(NormalOutput.Buffer);
            FHUMAClothSoftBindCS::FPermutationDomain Permutation;
            Permutation.Set<FHUMAClothSoftBindCS::FPass>(0);
            TShaderMapRef<FHUMAClothSoftBindCS> Shader(GetGlobalShaderMap(Scene->GetFeatureLevel()), Permutation);
            ClearUnusedGraphResources(Shader, Parameters);
            FComputeShaderUtils::AddPass(Graph, RDG_EVENT_NAME("HUMACloth SharedBodyNormals %u LOD %d", State->PrimitiveId.PrimIDValue, LODIndex),
                Shader, Parameters, FComputeShaderUtils::GetGroupCount(BodyCount, 256));
            SharedNormals.Add(Key, NormalOutput);
        }
        FRDGBufferRef Normals = NormalOutput.Buffer;
        FRDGBufferRef Anchors = Batch ? nullptr : Graph.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(12, Data->Bary.Num() * 4), TEXT("HUMACloth.Anchors"));
        FRDGBufferRef Positions = Graph.RegisterExternalBuffer(PositionPool);
        FRDGBufferRef Previous = Graph.RegisterExternalBuffer(PreviousPool);
        FRDGBufferRef Tangents = Graph.RegisterExternalBuffer(TangentPool);
        Graph.UseInternalAccessMode(Positions); Graph.UseInternalAccessMode(Previous); Graph.UseInternalAccessMode(Tangents);
        const bool NewFrame = State->LastPositionFrame != FrameCounter;
        const bool ResetHistory = !State->bHistoryValid || (NewFrame && State->LastPositionFrame + 1 != FrameCounter)
            || (State->ActiveBodyLOD != INDEX_NONE && State->ActiveBodyLOD != LODIndex);
        if (!Batch && NewFrame && !ResetHistory) AddCopyBufferPass(Graph, Previous, Positions);
        TArray<int32> Passes = Batch ? TArray<int32>() : TArray<int32>{1, 2, 3};
        FRDGBufferRef Validation = nullptr;
        if (SnapshotRequested)
        {
            FRDGBufferDesc ValidationDesc = FRDGBufferDesc::CreateStructuredDesc(12, BodyCount + State->Rest.Num() * 2);
            ValidationDesc.Usage |= BUF_SourceCopy;
            Validation = Graph.CreateBuffer(ValidationDesc, TEXT("HUMACloth.ValidationOnly"));
            Passes.Add(4);
        }
        TOptional<FPassViews> Views;
        if (Path != 0 && !Passes.IsEmpty()) Views = MakeViews(Graph, Static, Body, Normals, Anchors, Positions, Previous, Tangents, Validation);
        for (int32 Pass : Passes)
        {
            if (!Batch && Pass == 4 && ResetHistory) AddCopyBufferPass(Graph, Previous, Positions);
            auto* Parameters = Graph.AllocParameters<FHUMAClothSoftBindCS::FParameters>();
            Parameters->BodyCount = BodyCount; Parameters->AnchorCount = Data->Bary.Num(); Parameters->ClothCount = State->Rest.Num(); Parameters->Slots = State->Slots;
            if (Path == 0)
            {
            Parameters->BodyPositions = Graph.CreateSRV(Body, PF_R32_FLOAT);
            Parameters->Mapping = Graph.CreateSRV(Static[0]); Parameters->Quads = Graph.CreateSRV(Static[1]);
            Parameters->BodyOffsets = Graph.CreateSRV(Static[2]); Parameters->BodyIncidents = Graph.CreateSRV(Static[3]);
            Parameters->AnchorTriangles = Graph.CreateSRV(Static[4]); Parameters->Bary = Graph.CreateSRV(Static[5]);
            Parameters->Rest = Graph.CreateSRV(Static[6]); Parameters->RestPosition = Graph.CreateSRV(Static[7]);
            Parameters->RestTangent = Graph.CreateSRV(Static[8]); Parameters->RestBitangent = Graph.CreateSRV(Static[9]); Parameters->RestNormal = Graph.CreateSRV(Static[10]);
            Parameters->InfluenceAnchors = Graph.CreateSRV(Static[11]); Parameters->InfluenceWeights = Graph.CreateSRV(Static[12]);
            Parameters->Triangles = Graph.CreateSRV(Static[13]); Parameters->ClothOffsets = Graph.CreateSRV(Static[14]); Parameters->ClothIncidents = Graph.CreateSRV(Static[15]); Parameters->UV = Graph.CreateSRV(Static[16]);
            Parameters->BodyNormals = Graph.CreateSRV(Normals); Parameters->Anchors = Graph.CreateSRV(Anchors); Parameters->Positions = Graph.CreateSRV(Positions, PF_R32_FLOAT);
            Parameters->PreviousPositions = Graph.CreateSRV(Previous, PF_R32_FLOAT);
            Parameters->OutBodyNormals = Graph.CreateUAV(Normals); Parameters->OutAnchors = Graph.CreateUAV(Anchors);
            Parameters->OutPositions = Graph.CreateUAV(Positions, PF_R32_FLOAT); Parameters->OutTangents = Graph.CreateUAV(Tangents, PF_R32_UINT);
            Parameters->OutValidation = Validation ? Graph.CreateUAV(Validation) : nullptr;
            }
            else BindPass(*Parameters, Views.GetValue(), Pass, false);
            const uint32 Count = Pass == 1 ? Parameters->AnchorCount : Pass == 4 ? Parameters->BodyCount + Parameters->ClothCount * 2 : Parameters->ClothCount;
            Dispatch(Graph, Parameters, Pass, Count, Scene->GetFeatureLevel());
        }
        if (!Batch && ResetHistory && !SnapshotRequested) AddCopyBufferPass(Graph, Previous, Positions);
        State->VelocityFactory->UpdateVelocity(Graph.RHICmdList, uint32(FrameCounter), State->CurrentPositionSRV, State->PreviousPositionSRV, State->RestTangentSRV);
        State->LastPositionFrame = FrameCounter; State->bHistoryValid = true;
        if (Validation)
        {
            if (!State->SnapshotReadback) State->SnapshotReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("HUMACloth.ValidationOnly"));
            AddEnqueueCopyPass(Graph, State->SnapshotReadback.Get(), Validation, (BodyCount + State->Rest.Num() * 2) * sizeof(FVector3f));
            if (!State->TangentSnapshotReadback) State->TangentSnapshotReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("HUMACloth.TangentValidation"));
            AddEnqueueCopyPass(Graph, State->TangentSnapshotReadback.Get(), Tangents, State->Rest.Num() * 2 * sizeof(uint32));
            FScopeLock Lock(&State->Mutex);
            State->bSnapshotPending = true; State->bSnapshotRequested = false;
            State->SnapshotLOD = LODIndex; State->SnapshotBodyCount = BodyCount;
            State->SnapshotFrame = FrameCounter; State->bSnapshotHistoryReset = ResetHistory;
            State->SnapshotFamily = FamilySerial; State->SnapshotBodyNormalGroup = NormalOutput.Group;
            State->SnapshotOptimizationPath = Path;
            State->SnapshotBatchParts = Batch ? Batch->Parts : 1;
            State->SnapshotComputeDispatches = Batch ? Batch->Dispatches : bNormalsReused ? 3 : 4;
            State->bSnapshotBodyNormalsReused = bNormalsReused;
            State->SnapshotBodyTopologySignature = Data->BodyTopology->Signature;
        }
        Graph.UseExternalAccessMode(Positions, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
        Graph.UseExternalAccessMode(Previous, ERHIAccess::SRVMask);
        Graph.UseExternalAccessMode(Tangents, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
        State->LastFrame = FamilySerial;
        FScopeLock Lock(&State->Mutex);
        if (State->ActiveBodyLOD != INDEX_NONE && State->ActiveBodyLOD != LODIndex) ++State->LODTransitions;
        State->ActiveBodyLOD = LODIndex;
        State->OptimizationPath = Path; State->BatchParts = Batch ? Batch->Parts : 1;
        State->ComputeDispatches = Batch ? Batch->Dispatches : bNormalsReused ? 3 : 4;
        State->BodyNormalGroup = NormalOutput.Group; State->BodyNormalEvaluation = FamilySerial;
        if (bNormalsReused) ++State->BodyNormalReuses; else ++State->BodyNormalPasses;
        State->bReady = true; ++State->Frames; State->Status = FString::Printf(TEXT("GPU SoftBind / source LOD %d / bind LOD %d%s / path %d / batch %d / %s / no readback / body normals %s"),
            LODIndex, Data->BindBodyLOD, Data->SourceTransfer ? TEXT(" / surface transfer") : TEXT(""),
            Path, State->BatchParts, bBeforeDepth ? TEXT("before depth") : TEXT("before base pass"), bNormalsReused ? TEXT("shared") : TEXT("producer"));
    }
    // The source vertex factory also reads these buffers outside RDG resource declarations.
    for (const auto& Input : BodyInputs)
    {
        if (Input.Value.Positions)
            Graph.UseExternalAccessMode(Input.Value.Positions, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
    }
}

class FHUMAClothViewExtension final : public FSceneViewExtensionBase
{
public:
    explicit FHUMAClothViewExtension(const FAutoRegister& Register) : FSceneViewExtensionBase(Register) {}
    virtual void SetupViewFamily(FSceneViewFamily&) override {}
    virtual void SetupView(FSceneViewFamily&, FSceneView&) override {}
    virtual void BeginRenderViewFamily(FSceneViewFamily&) override {}
    virtual int32 GetPriority() const override { return -1000; }
    virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& Graph, FSceneViewFamily& Family) override { Scene = Family.Scene; FrameCounter = Family.FrameCounter; ++FamilySerial; }
    virtual void PreInitViews_RenderThread(FRDGBuilder& Graph) override { if (Scene) Update(Graph, Scene, true, FamilySerial, FrameCounter); }
    virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& Graph, FSceneViewFamily& Family) override { CaptureRenderedBody(Graph, Family.Scene); }
private:
    FSceneInterface* Scene = nullptr;
    uint64 FamilySerial = 0;
    uint64 FrameCounter = 0;
};
}

void RegisterHUMAClothGPU(TSharedPtr<FHUMAClothGpuState, ESPMode::ThreadSafe> State)
{
    ENQUEUE_RENDER_COMMAND(HUMAClothGPURegister)([State](FRHICommandListImmediate& RHICmdList)
    {
        FRDGBuilder Graph(RHICmdList);
        InternBodyTopology(*State);
        PrepareBuffers(Graph, *State);
        for (auto& Entry : State->BodyLODs)
        {
            InternBodyTopology(*Entry.Value);
            Entry.Value->Immutable.SetNum(17);
            for (int32 Index : {6, 11, 12, 13, 14, 15, 16}) Entry.Value->Immutable[Index] = State->Immutable[Index];
            if (Entry.Value->BindBodyLOD == State->BindBodyLOD)
                for (int32 Index = 4; Index < 17; ++Index) Entry.Value->Immutable[Index] = State->Immutable[Index];
            PrepareBuffers(Graph, *Entry.Value);
        }
        Graph.Execute();
        States.AddUnique(State);
    });
}
void UnregisterHUMAClothGPU(TSharedPtr<FHUMAClothGpuState, ESPMode::ThreadSafe> State)
{
    ENQUEUE_RENDER_COMMAND(HUMAClothGPUUnregister)([State](FRHICommandListImmediate&)
    {
        BatchCache.RemoveAll([&](const FBatchCache& Batch)
        {
            for (const auto& Member : Batch.Members) if (Member.Pin() == State) return true;
            return false;
        });
        States.Remove(State);
    });
}
void StartHUMAClothGPU()
{
    if (GEngine) Extension = FSceneViewExtensions::NewExtension<FHUMAClothViewExtension>();
    else
    {
#if ENGINE_MINOR_VERSION >= 8
        EngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddLambda([] { Extension = FSceneViewExtensions::NewExtension<FHUMAClothViewExtension>(); });
#else
        EngineInitHandle = FCoreDelegates::OnPostEngineInit.AddLambda([] { Extension = FSceneViewExtensions::NewExtension<FHUMAClothViewExtension>(); });
#endif
    }
}
void StopHUMAClothGPU()
{
#if ENGINE_MINOR_VERSION >= 8
    FCoreDelegates::GetOnPostEngineInit().Remove(EngineInitHandle);
#else
    FCoreDelegates::OnPostEngineInit.Remove(EngineInitHandle);
#endif
    Extension.Reset();
    ENQUEUE_RENDER_COMMAND(HUMAClothGPUClear)([](FRHICommandListImmediate&) { BatchCache.Reset(); States.Reset(); BodyTopologies.Reset(); });
    FlushRenderingCommands();
}

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Tests/HUMAClothBodyFieldGPU.inl"
#endif