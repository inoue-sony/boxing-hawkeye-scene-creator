// Copyright (c) 2025 WOCDeformer Plugin. All Rights Reserved.

#include "WOCBatchSubsystem.h"
#include "WOCDeformerCore.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHI.h"

DEFINE_LOG_CATEGORY_STATIC(LogWOCBatch, Log, All);

DECLARE_STATS_GROUP(TEXT("WOCBatch"), STATGROUP_WOCBatch, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("WOC Batch Tick"),       STAT_WOCBatch_Tick,      STATGROUP_WOCBatch);
DECLARE_CYCLE_STAT(TEXT("WOC Batch Inference"),   STAT_WOCBatch_Infer,    STATGROUP_WOCBatch);
DECLARE_CYCLE_STAT(TEXT("WOC Batch GPU Upload"),  STAT_WOCBatch_Upload,   STATGROUP_WOCBatch);

// ===========================================================================
// UWOCBatchSubsystem — Lifecycle
// ===========================================================================

bool UWOCBatchSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// Create in game worlds and PIE, not in editor preview worlds
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->IsGameWorld() || World->IsPlayInEditor());
}

void UWOCBatchSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_LOG(LogWOCBatch, Log, TEXT("UWOCBatchSubsystem initialized."));
}

void UWOCBatchSubsystem::Deinitialize()
{
	for (auto& Pair : BatchGroups)
	{
		Pair.Value.BatchInstance.Reset();
		Pair.Value.SharedModel.Reset();
		FWOCModelCache::ReleaseByKey(Pair.Key);
	}
	BatchGroups.Empty();
	SlotMappings.Empty();
	Super::Deinitialize();
}

TStatId UWOCBatchSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWOCBatchSubsystem, STATGROUP_WOCBatch);
}

// ===========================================================================
// Frame Management
// ===========================================================================

void UWOCBatchSubsystem::ResetFrame()
{
	SlotMappings.Reset();
	for (auto& Pair : BatchGroups)
	{
		Pair.Value.Slots.Reset();
	}
}

// ===========================================================================
// RegisterSlot — called from PreUpdate (GT, sequential)
// ===========================================================================

int32 UWOCBatchSubsystem::RegisterSlot(
	UNNEModelData* ModelData, const FString& RuntimeName,
	FWOCDeformerInferenceCore* Core)
{
	check(IsInGameThread());

	if (!ModelData || !Core || !Core->bGPUBufferReady) { return INDEX_NONE; }

	// Detect new frame
	const uint64 Frame = (uint64)GFrameCounter;
	if (Frame != CurrentFrameNumber)
	{
		ResetFrame();
		CurrentFrameNumber = Frame;
	}

	const TObjectKey<UNNEModelData> Key(ModelData);

	// Find or create batch group
	FBatchGroup* Group = BatchGroups.Find(Key);

	// Check if this Core is already registered in this frame (prevents duplicate buffer uploads)
	if (Group)
	{
		for (int32 i = 0; i < Group->Slots.Num(); ++i)
		{
			if (Group->Slots[i].Identity == Core && Group->Slots[i].Coefficients == Core->CoeffGPUBuffer)
			{
				// Return existing handle for this Core
				for (int32 h = 0; h < SlotMappings.Num(); ++h)
				{
					if (SlotMappings[h].GroupKey == Key && SlotMappings[h].IndexInGroup == i)
					{
						return h;
					}
				}
			}
		}
	}

	if (!Group)
	{
		FBatchGroup NewGroup;
		NewGroup.ModelData   = ModelData;
		NewGroup.RuntimeName = RuntimeName;
		NewGroup.NumJoints   = Core->GetNumJoints();
		NewGroup.JN          = Core->GetJN();

		// Get shared model from cache
		NewGroup.SharedModel = FWOCModelCache::GetOrCreate(ModelData, RuntimeName);
		if (!NewGroup.SharedModel.IsValid())
		{
			UE_LOG(LogWOCBatch, Warning,
			       TEXT("UWOCBatchSubsystem: Failed to get IModelCPU for '%s'."),
			       *ModelData->GetName());
			return INDEX_NONE;
		}

		Group = &BatchGroups.Add(Key, MoveTemp(NewGroup));
		UE_LOG(LogWOCBatch, Log,
		       TEXT("UWOCBatchSubsystem: Created batch group for '%s' (J=%d JN=%d)"),
		       *ModelData->GetName(), Group->NumJoints, Group->JN);
	}

	// Add slot to group
	const uint32 IndexInGroup = Group->Slots.Num();
	FBatchGroup::FSlotInfo& Slot = Group->Slots.AddDefaulted_GetRef();
	Slot.Identity = Core;
	Slot.Coefficients = Core->CoeffGPUBuffer;
	Slot.Rotations = Core->RglobalGPUBuffer;

	// Ensure input/output buffers are large enough (use int64 to detect overflow)
	const int64 InputNeeded64  = (int64)(IndexInGroup + 1) * Group->NumJoints * 9;
	const int64 OutputNeeded64 = (int64)(IndexInGroup + 1) * Group->JN;
	const int64 RGNeeded64     = (int64)(IndexInGroup + 1) * Group->NumJoints * 9;

	if (InputNeeded64 > (int64)MAX_int32 || OutputNeeded64 > (int64)MAX_int32 || RGNeeded64 > (int64)MAX_int32)
	{
		UE_LOG(LogWOCBatch, Error,
		       TEXT("UWOCBatchSubsystem: Buffer size overflow detected (Slots=%d J=%d JN=%d)."),
		       IndexInGroup + 1, Group->NumJoints, Group->JN);
		Group->Slots.Pop();
		return INDEX_NONE;
	}

	const int32 InputNeeded  = (int32)InputNeeded64;
	const int32 OutputNeeded = (int32)OutputNeeded64;
	const int32 RGNeeded     = (int32)RGNeeded64;

	if (Group->InputBuffer.Num() < InputNeeded)
	{
		Group->InputBuffer.SetNumZeroed(InputNeeded);
	}
	if (Group->OutputBuffer.Num() < OutputNeeded)
	{
		Group->OutputBuffer.SetNumZeroed(OutputNeeded);
	}
	if (Group->RGlobalBuffer.Num() < RGNeeded)
	{
		Group->RGlobalBuffer.SetNumZeroed(RGNeeded);
	}

	// Return global slot handle
	FSlotMapping Mapping;
	Mapping.GroupKey     = Key;
	Mapping.IndexInGroup = IndexInGroup;
	const int32 Handle = SlotMappings.Num();
	SlotMappings.Add(Mapping);

	return Handle;
}

// ===========================================================================
// WriteInput / WriteRGlobal — called from Evaluate (worker thread, lock-free)
// ===========================================================================

void UWOCBatchSubsystem::WriteInput(int32 SlotHandle, TConstArrayView<float> RLocalData)
{
	if (!SlotMappings.IsValidIndex(SlotHandle)) { return; }

	const FSlotMapping& Map = SlotMappings[SlotHandle];
	FBatchGroup* Group = BatchGroups.Find(Map.GroupKey);
	if (!Group) { return; }

	const uint32 Offset = Map.IndexInGroup * Group->NumJoints * 9;
	const uint32 Count  = Group->NumJoints * 9;
	if (RLocalData.Num() != (int32)Count) { return; }
	if (Offset + Count > (uint32)Group->InputBuffer.Num()) { return; }

	FMemory::Memcpy(
		Group->InputBuffer.GetData() + Offset,
		RLocalData.GetData(),
		Count * sizeof(float));
}

void UWOCBatchSubsystem::WriteRGlobal(int32 SlotHandle, TConstArrayView<float> RGlobalData)
{
	if (!SlotMappings.IsValidIndex(SlotHandle)) { return; }

	const FSlotMapping& Map = SlotMappings[SlotHandle];
	FBatchGroup* Group = BatchGroups.Find(Map.GroupKey);
	if (!Group) { return; }

	const uint32 Offset = Map.IndexInGroup * Group->NumJoints * 9;
	const uint32 Count  = Group->NumJoints * 9;
	if (RGlobalData.Num() != (int32)Count) { return; }
	if (Offset + Count > (uint32)Group->RGlobalBuffer.Num()) { return; }

	FMemory::Memcpy(
		Group->RGlobalBuffer.GetData() + Offset,
		RGlobalData.GetData(),
		Count * sizeof(float));
}

// ===========================================================================
// Tick — runs after all animation evaluation is complete
// ===========================================================================

void UWOCBatchSubsystem::Tick(float DeltaTime)
{
	SCOPE_CYCLE_COUNTER(STAT_WOCBatch_Tick);

	// Only process if we registered slots this frame
	if (CurrentFrameNumber != uint64(GFrameCounter))
	{
		ResetFrame();
		CurrentFrameNumber = GFrameCounter;
		return;
	}
	if (SlotMappings.IsEmpty()) { return; }

	// Run batch inference per group
	{
		SCOPE_CYCLE_COUNTER(STAT_WOCBatch_Infer);
		for (auto& Pair : BatchGroups)
		{
			FBatchGroup& Group = Pair.Value;
			const uint32 SlotCount = Group.Slots.Num();
			if (SlotCount == 0) { continue; }
			Group.RunBatchInference(SlotCount);
		}
	}

	// GPU upload — single render command for all slots across all groups
	{
		SCOPE_CYCLE_COUNTER(STAT_WOCBatch_Upload);
		FlushBatchResults();
	}
	ResetFrame();
}

void UWOCBatchSubsystem::FlushBatchResults()
{
	// Collect all upload tasks across all groups into a single ENQUEUE_RENDER_COMMAND
	struct FUploadEntry
	{
		TRefCountPtr<FRDGPooledBuffer> Buffer;
		TArray<float> Data;  // Owned copy (safe for async RT execution)
	};
	TArray<FUploadEntry> Uploads;

	for (auto& Pair : BatchGroups)
	{
		FBatchGroup& Group = Pair.Value;
		const uint32 SlotCount = Group.Slots.Num();
		if (SlotCount == 0) { continue; }

		for (uint32 i = 0; i < SlotCount; ++i)
		{
			const auto& Slot = Group.Slots[i];

			// Coefficients
			if (Slot.Coefficients.IsValid())
			{
				const uint32 CoeffOffset = i * Group.JN;
				FUploadEntry& E = Uploads.AddDefaulted_GetRef();
				E.Buffer = Slot.Coefficients;
				E.Data.SetNumUninitialized(Group.JN);
				FMemory::Memcpy(E.Data.GetData(),
				                Group.OutputBuffer.GetData() + CoeffOffset,
				                Group.JN * sizeof(float));
			}

			// R_global
			if (Slot.Rotations.IsValid())
			{
				const uint32 RGOffset = i * Group.NumJoints * 9;
				const uint32 RGCount  = Group.NumJoints * 9;
				FUploadEntry& E = Uploads.AddDefaulted_GetRef();
				E.Buffer = Slot.Rotations;
				E.Data.SetNumUninitialized(RGCount);
				FMemory::Memcpy(E.Data.GetData(),
				                Group.RGlobalBuffer.GetData() + RGOffset,
				                RGCount * sizeof(float));
			}
		}
	}

	if (Uploads.IsEmpty()) { return; }

	// Deduplicate: only keep the LAST upload for each unique buffer (defense in depth)
	{
		TMap<FRDGPooledBuffer*, int32> BufferToLastIndex;
		for (int32 i = 0; i < Uploads.Num(); ++i)
		{
			BufferToLastIndex.FindOrAdd(Uploads[i].Buffer.GetReference()) = i;
		}
		if (BufferToLastIndex.Num() < Uploads.Num())
		{
			TArray<FUploadEntry> Deduped;
			Deduped.Reserve(BufferToLastIndex.Num());
			for (auto& Pair : BufferToLastIndex)
			{
				Deduped.Add(MoveTemp(Uploads[Pair.Value]));
			}
			Uploads = MoveTemp(Deduped);
		}
	}

	// Single render command for all uploads
	ENQUEUE_RENDER_COMMAND(WOCBatch_FlushAll)
	(
		[Uploads = MoveTemp(Uploads)](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder Builder(RHICmdList);
			for (const FUploadEntry& U : Uploads)
			{
				if (!U.Buffer || U.Data.IsEmpty()) { continue; }
				FRDGBufferRef Buf = Builder.RegisterExternalBuffer(U.Buffer);
				Builder.QueueBufferUpload(Buf, U.Data.GetData(),
				                         U.Data.Num() * sizeof(float),
				                         ERDGInitialDataFlags::NoCopy);
			}
			Builder.Execute();
		}
	);
}

// ===========================================================================
// FBatchGroup — Batch model management & inference
// ===========================================================================

uint32 UWOCBatchSubsystem::FBatchGroup::QuantizeBatchSize(uint32 N)
{
	static const uint32 Buckets[] = { 1, 4, 8, 16, 32, 64, 96, 128, 192, 256, 384, 512 };
	for (uint32 B : Buckets)
	{
		if (N <= B) { return B; }
	}
	return ((N + 63) / 64) * 64;
}

void UWOCBatchSubsystem::FBatchGroup::EnsureBatchModel(uint32 BatchSize)
{
	if (BatchModelSize == BatchSize) { return; }
	if (!SharedModel.IsValid()) { return; }

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
	auto NewInstance = SharedModel->CreateModelInstanceCPU();
#else
	TSharedPtr<UE::NNE::IModelInstanceCPU> NewInstance = MakeShareable(SharedModel->CreateModelInstance().Release());
#endif
	if (!NewInstance.IsValid())
	{
		UE_LOG(LogWOCBatch, Warning,
		       TEXT("WOCBatch: CreateModelInstanceCPU failed (N=%d)"), BatchSize);
		return;
	}

	TArray<UE::NNE::FTensorShape> Shapes;
	Shapes.Add(UE::NNE::FTensorShape::Make({ (uint32)BatchSize, (uint32)NumJoints, 3, 3 }));
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
	if (NewInstance->SetInputTensorShapes(Shapes) != UE::NNE::EResultStatus::Ok)
#else
	if (NewInstance->SetInputTensorShapes(Shapes) != 0)
#endif
	{
		UE_LOG(LogWOCBatch, Warning,
		       TEXT("WOCBatch: SetInputTensorShapes(N=%d) failed. "
		            "Ensure ONNX model supports dynamic batch."), BatchSize);
		return;
	}

	BatchInstance   = MoveTemp(NewInstance);
	BatchModelSize  = BatchSize;
	UE_LOG(LogWOCBatch, Log,
	       TEXT("WOCBatch: Model ready (batch=%d J=%d JN=%d)"), BatchSize, NumJoints, JN);
}

void UWOCBatchSubsystem::FBatchGroup::RunBatchInference(uint32 SlotCount)
{
	if (SlotCount == 0 || !SharedModel.IsValid()) { return; }

	const uint32 Quantized = QuantizeBatchSize(SlotCount);
	EnsureBatchModel(Quantized);

	if (!BatchInstance.IsValid() || BatchModelSize == 0) { return; }

	const uint32 ActualBatch = FMath::Min(Quantized, BatchModelSize);

	// Use int64 for intermediate calculations to detect potential overflow
	const int64 InputTotal64  = (int64)ActualBatch * NumJoints * 9;
	const int64 OutputTotal64 = (int64)ActualBatch * JN;

	if (InputTotal64 > (int64)MAX_int32 || OutputTotal64 > (int64)MAX_int32)
	{
		UE_LOG(LogWOCBatch, Error,
		       TEXT("WOCBatch: RunBatchInference buffer overflow (Batch=%u J=%u JN=%u)."),
		       ActualBatch, NumJoints, JN);
		return;
	}

	const int32 InputTotal  = (int32)InputTotal64;
	const int32 OutputTotal = (int32)OutputTotal64;

	// Ensure buffer sizes (may need to grow for quantized padding)
	if (InputBuffer.Num() < InputTotal)
	{
		InputBuffer.SetNumZeroed(InputTotal);
	}
	if (OutputBuffer.Num() < OutputTotal)
	{
		OutputBuffer.SetNumZeroed(OutputTotal);
	}

	// Zero-fill padding slots beyond actual data
	if (ActualBatch > SlotCount)
	{
		const int64 PadStart64 = (int64)SlotCount * NumJoints * 9;
		const int64 PadBytes64 = (int64)(ActualBatch - SlotCount) * NumJoints * 9 * sizeof(float);
		FMemory::Memzero(InputBuffer.GetData() + PadStart64, (SIZE_T)PadBytes64);
	}

	// Run inference
	TArray<UE::NNE::FTensorBindingCPU> InBindings, OutBindings;
	{
		auto& B       = InBindings.AddDefaulted_GetRef();
		B.Data        = InputBuffer.GetData();
		B.SizeInBytes = (uint64)InputTotal * sizeof(float);
	}
	{
		auto& B       = OutBindings.AddDefaulted_GetRef();
		B.Data        = OutputBuffer.GetData();
		B.SizeInBytes = (uint64)OutputTotal * sizeof(float);
	}

	BatchInstance->RunSync(InBindings, OutBindings);
}
