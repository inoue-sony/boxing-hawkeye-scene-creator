#pragma once

#include "CoreMinimal.h"
#include "PrimitiveComponentId.h"
#include "RenderGraphResources.h"
#include "RHIGPUReadback.h"
#include "HUMAClothBodySharing.h"
#include "HUMAClothSourceMapping.h"

class FSceneInterface;
class UHUMAClothBindAsset;
class FHUMAClothVelocityVertexFactory;

struct FHUMAClothGpuState
{
    FHUMAClothGpuState() = default;
    explicit FHUMAClothGpuState(const UHUMAClothBindAsset& Asset, TConstArrayView<int32> Mapping = {}, bool bIncludeBodyLODs = true);
    FHUMAClothGpuState(const UHUMAClothBindAsset& Asset, const FHUMAClothSourceLODs& Sources);
    int32 BindBodyLOD = 0;
    TSharedPtr<const FHUMAClothLODTransfer, ESPMode::ThreadSafe> SourceTransfer;
    TMap<int32, TSharedPtr<FHUMAClothGpuState, ESPMode::ThreadSafe>> BodyLODs;
    int32 ActiveBodyLOD = INDEX_NONE;
    uint64 LODTransitions = 0;
    int32 SnapshotLOD = 0;
    uint32 SnapshotBodyCount = 0;
    uint64 SnapshotFrame = 0;
    uint64 SnapshotFamily = 0;
    int32 SnapshotBodyNormalGroup = INDEX_NONE;
    uint32 SnapshotBodyTopologySignature = 0;
    bool bSnapshotBodyNormalsReused = false;
    bool bSnapshotHistoryReset = false;
    int32 OptimizationPath = 0;
    int32 BatchParts = 1;
    int32 ComputeDispatches = 0;
    int32 SnapshotOptimizationPath = 0;
    int32 SnapshotBatchParts = 1;
    int32 SnapshotComputeDispatches = 0;
    FSceneInterface* Scene = nullptr;
    FPrimitiveComponentId PrimitiveId;
    TSharedPtr<const FHUMAClothBodyTopology, ESPMode::ThreadSafe> BodyTopology;
    TArray<uint32> ClothOffsets, ClothIncidents;
    TArray<uint32> Triangles, AnchorTriangles, InfluenceAnchors;
    TArray<float> InfluenceWeights;
    TArray<FVector3f> Rest, Bary, RestPosition, RestTangent, RestBitangent, RestNormal;
    TArray<FVector2f> UV;
    TRefCountPtr<FRDGPooledBuffer> Positions, PreviousPositions, Tangents;
    FHUMAClothVelocityVertexFactory* VelocityFactory = nullptr;
    FShaderResourceViewRHIRef CurrentPositionSRV, PreviousPositionSRV, RestTangentSRV;
    uint64 LastPositionFrame = MAX_uint64;
    bool bHistoryValid = false;
    TArray<TRefCountPtr<FRDGPooledBuffer>> Immutable;
    uint32 Slots = 24;
    FCriticalSection Mutex;
    FString Status = TEXT("Waiting for same-frame GPU SoftBind");
    uint64 Frames = 0;
    uint64 BodyNormalPasses = 0;
    uint64 BodyNormalReuses = 0;
    int32 BodyNormalGroup = INDEX_NONE;
    uint64 BodyNormalEvaluation = 0;
    uint64 LastFrame = MAX_uint64;
    bool bReady = false;
    bool bSnapshotRequested = false;
    bool bSnapshotPending = false;
    bool bRenderedSnapshotPending = false;
    FString SnapshotPath;
    TUniquePtr<FRHIGPUBufferReadback> SnapshotReadback;
    TUniquePtr<FRHIGPUBufferReadback> RenderedSnapshotReadback;
    TUniquePtr<FRHIGPUBufferReadback> TangentSnapshotReadback;
    FHUMAClothGpuState* GetLODData(int32 LODIndex) { return LODIndex == BindBodyLOD ? this : BodyLODs.FindRef(LODIndex).Get(); }
};

void RegisterHUMAClothGPU(TSharedPtr<FHUMAClothGpuState, ESPMode::ThreadSafe> State);
void UnregisterHUMAClothGPU(TSharedPtr<FHUMAClothGpuState, ESPMode::ThreadSafe> State);
void StartHUMAClothGPU();
void StopHUMAClothGPU();