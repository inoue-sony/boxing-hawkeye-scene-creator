// Copyright (c) 2025 WOCDeformer Plugin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"
#include "NNE.h"
#include "NNEModelData.h"
#include "NNERuntimeCPU.h"
#include "UObject/ObjectKey.h"
#include "RenderGraphResources.h"

#include "WOCBatchSubsystem.generated.h"

struct FWOCDeformerInferenceCore;
class FWOCModelCache;

/**
 * World Subsystem that batches ONNX inference for all WOC AnimNodes.
 *
 * Timeline within a single frame:
 *   1. PreUpdate (GT, sequential) : AnimNodes call RegisterSlot()
 *   2. Evaluate  (WT, parallel)   : AnimNodes call WriteInput() / WriteRGlobal()
 *   3. Tick      (GT, after all tick groups) : RunBatchInference + GPU upload
 *
 * Result: same-frame coefficients with no latency, batched for efficiency.
 */
UCLASS()
class WOCDEFORMER_API UWOCBatchSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// USubsystem
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override { return true; }
	virtual bool IsTickableInEditor() const override { return false; }

	/**
	 * Register a slot for this frame's batch inference.
	 * Called from AnimNode PreUpdate (Game Thread).
	 * @return Opaque slot handle (pass to WriteInput/WriteRGlobal)
	 */
	int32 RegisterSlot(UNNEModelData* ModelData, const FString& RuntimeName,
	                   FWOCDeformerInferenceCore* Core);

	/**
	 * Write R_local input to the batch buffer at the given slot.
	 * Called from AnimNode Evaluate (worker thread). Lock-free (disjoint slot regions).
	 */
	void WriteInput(int32 SlotHandle, TConstArrayView<float> RLocalData);

	/**
	 * Write R_global data for GPU upload at the given slot.
	 * Called from AnimNode Evaluate (worker thread). Lock-free.
	 */
	void WriteRGlobal(int32 SlotHandle, TConstArrayView<float> RGlobalData);

private:
	friend class FWOCBatchEmptyFramesTest;
	// --- Per-model batch group ---
	struct FBatchGroup
	{
		TWeakObjectPtr<UNNEModelData> ModelData;
		FString RuntimeName;
		TSharedPtr<UE::NNE::IModelCPU> SharedModel;
		TSharedPtr<UE::NNE::IModelInstanceCPU> BatchInstance;
		uint32 NumJoints    = 0;
		uint32 JN           = 0;
		uint32 BatchModelSize = 0;

		struct FSlotInfo
		{
			const FWOCDeformerInferenceCore* Identity = nullptr;
			TRefCountPtr<FRDGPooledBuffer> Coefficients;
			TRefCountPtr<FRDGPooledBuffer> Rotations;
		};
		TArray<FSlotInfo> Slots;

		// Flat buffers: [SlotCount * NumJoints * 9] / [SlotCount * JN]
		TArray<float> InputBuffer;
		TArray<float> OutputBuffer;
		TArray<float> RGlobalBuffer;

		void EnsureBatchModel(uint32 BatchSize);
		void RunBatchInference(uint32 SlotCount);

		/** Quantize batch size upward to reduce ORT model reconstruction */
		static uint32 QuantizeBatchSize(uint32 N);
	};

	TMap<TObjectKey<UNNEModelData>, FBatchGroup> BatchGroups;

	// Flat slot handle → group mapping
	struct FSlotMapping
	{
		TObjectKey<UNNEModelData> GroupKey;
		uint32 IndexInGroup = 0;
	};
	TArray<FSlotMapping> SlotMappings;

	uint64 CurrentFrameNumber = UINT64_MAX;

	void ResetFrame();
	void FlushBatchResults();
};
