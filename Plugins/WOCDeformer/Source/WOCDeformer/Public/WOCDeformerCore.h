// Copyright (c) 2025 WOCDeformer Plugin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NNE.h"
#include "NNEModelData.h"
#include "NNERuntimeCPU.h"
#include "RenderGraphResources.h"
#include "UObject/ObjectKey.h"

class USkeletalMeshComponent;
class UWOCBasisAsset;

// WOCDeformer stat group: referenced from multiple .cpp files
DECLARE_STATS_GROUP(TEXT("WOCDeformer"), STATGROUP_WOCDeformer, STATCAT_Advanced);

// ----------------------------------------------------------------
// FWOCModelCache — Global cache for IModel instances
// ----------------------------------------------------------------

class WOCDEFORMER_API FWOCModelCache
{
public:
	static TSharedPtr<UE::NNE::IModelCPU> GetOrCreate(
		UNNEModelData* ModelData, const FString& RuntimeName);
	static void Release(UNNEModelData* ModelData);
	static void ReleaseByKey(const TObjectKey<UNNEModelData>& ModelKey);

private:
	friend class FWOCBatchEmptyFramesTest;
	struct FCacheEntry
	{
		TSharedPtr<UE::NNE::IModelCPU> Model;
		uint32 RefCount = 0;
	};
	static TMap<TObjectKey<UNNEModelData>, FCacheEntry> Cache;
	static FCriticalSection                            Lock;
};

/**
 * Pure C++ core for WOC Deformer inference and GPU buffer management.
 * Owned by FAnimNode_WOCDeformer.
 * Not a UObject derivative, so cannot be a UPROPERTY (assumes external ownership of UNNEModelData/BasisAsset).
 */
struct WOCDEFORMER_API FWOCDeformerInferenceCore
{
	// Input (set by caller before each Tick)
	TWeakObjectPtr<UNNEModelData>   ModelData;
	TWeakObjectPtr<UWOCBasisAsset>  BasisAsset;
	FString RuntimeName  = TEXT("NNERuntimeORTCpu");
	float   OffsetScale  = 1.0f;
	bool    bLogLODChanges = true;

	// LOD result (referenced by DataInterface)
	uint32 ActiveLODForFrame = INDEX_NONE;

	// GPU buffers (referenced by DataInterface)
	TRefCountPtr<FRDGPooledBuffer> CoeffGPUBuffer;
	TRefCountPtr<FRDGPooledBuffer> RglobalGPUBuffer;
	bool bGPUBufferReady = false;

	/** Initialize model only (no inference). Call from game thread. */
	bool TryInit(USkeletalMeshComponent* SkelComp);

	/**
	 * Run inference with externally-provided R matrices and upload to GPU.
	 * Safe to call from anim worker thread (issues ENQUEUE_RENDER_COMMAND internally).
	 * RLocalData  : [J * 9] per-joint parent-space rotation (row-major)
	 * RGlobalData : [J * 9] per-joint component-space rotation (row-major)
	 * LODIndex    : LOD index resolved in PreUpdate
	 */
	void RunFrameWithRotations(TConstArrayView<float> RLocalData,
	                           TConstArrayView<float> RGlobalData,
	                           uint32                  LODIndex);

	bool           IsModelReady()   const { return bModelReady; }
	uint32          GetNumJoints()   const { return NumJoints; }
	uint32          GetJN()          const { return JN; }
	const TArray<FName>& GetJointNames() const { return JointNames; }
	uint32          ResolveEffectiveLOD(uint32 RequestedLOD) const;

	static bool ExtractModelMetadata(UNNEModelData* Model, TArray<FName>& Names, int32& Coefficients, FString& Error);

	/** Release ref to model cache */
	void Release();

	~FWOCDeformerInferenceCore();

private:
	bool  EnsureModelReady(USkeletalMeshComponent* SkelComp);
	void  RunInferenceCPU();
	void  UploadCoeffs();
	void  CreateGPUBuffers();

	static TMap<FString, FString> ParseONNXMetadata(TConstArrayView<uint8> Bytes);

	// Internal state
	TSharedPtr<UE::NNE::IModelInstanceCPU> ModelInstance;
	TArray<FName> JointNames;
	TArray<uint32> JointToRefBoneIndex;
	uint32 NumJoints = 0;
	uint32 NumCoeffs = 0;
	uint32 JN        = 0;
	bool  bModelReady = false;

	TArray<float> InputBuffer;
	TArray<float> OutputBuffer;

	TOptional<TObjectKey<UNNEModelData>> CachedModelKey;
};
