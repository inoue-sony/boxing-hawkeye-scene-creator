#pragma once

#include "CoreMinimal.h"
#include "PrimitiveComponentId.h"
#include "RHIGPUReadback.h"

class FSceneInterface;

struct FHUMAClothReadback : public TSharedFromThis<FHUMAClothReadback, ESPMode::ThreadSafe>
{
    FSceneInterface* Scene = nullptr;
    FPrimitiveComponentId PrimitiveId;
    uint32 RenderVertexCount = 0;
    TUniquePtr<FRHIGPUBufferReadback> Readback;
    bool bPending = false;
    FCriticalSection Mutex;
    TArray<FVector3f> Positions;
    uint64 Revision = 0;
    FString Status = TEXT("Waiting for WOC Mesh Deformer output");
};

void RegisterHUMAClothReadback(TSharedPtr<FHUMAClothReadback, ESPMode::ThreadSafe> State);
void UnregisterHUMAClothReadback(TSharedPtr<FHUMAClothReadback, ESPMode::ThreadSafe> State);
void StartHUMAClothRenderer();
void StopHUMAClothRenderer();