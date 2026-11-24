// Copyright (c) 2025 WOCDeformer Plugin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNodeBase.h"
#include "NNEModelData.h"

#include "WOCDeformerCore.h"

#include "AnimNode_WOCDeformer.generated.h"

class UWOCBasisAsset;
class UWOCBatchSubsystem;
class USkeletalMeshComponent;

/**
 * Animation Blueprint node that performs WOC Deformer inference.
 *
 * Runs its own Core in PreUpdate to produce/update Coeff/Rglobal GPU buffers.
 * The Deformer Graph side (UWOCDeformerDataInterface) searches for this AnimNode
 * on the owning SkelMeshComponent's AnimInstances.
 */
USTRUCT(BlueprintInternalUseOnly)
struct WOCDEFORMER_API FAnimNode_WOCDeformer : public FAnimNode_Base
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Links)
	FPoseLink Source;

	/** ONNX model (shared across all LODs) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WOC Deformer", meta = (PinShownByDefault))
	TObjectPtr<UNNEModelData> ModelData = nullptr;

	/** Basis asset containing the LOD array */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WOC Deformer", meta = (PinShownByDefault))
	TObjectPtr<UWOCBasisAsset> BasisAsset = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WOC Deformer",
	          meta = (PinHiddenByDefault, ClampMin = "0.0", ClampMax = "2.0"))
	float OffsetScale = 1.0f;

	UPROPERTY(EditAnywhere, Category = "WOC Deformer", AdvancedDisplay)
	FString RuntimeName = TEXT("NNERuntimeORTCpu");

	UPROPERTY(EditAnywhere, Category = "WOC Deformer", AdvancedDisplay)
	bool bLogLODChanges = true;

	// FAnimNode_Base
	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override;
	virtual void CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) override;
	virtual void Update_AnyThread(const FAnimationUpdateContext& Context) override;
	virtual void Evaluate_AnyThread(FPoseContext& Output) override;
	virtual void GatherDebugData(FNodeDebugData& DebugData) override;
	virtual bool HasPreUpdate() const override { return true; }
	virtual void PreUpdate(const UAnimInstance* InAnimInstance) override;

	// Accessed from DataInterface
	const FWOCDeformerInferenceCore& GetCore() const { return Core; }
	UWOCBasisAsset*                  GetBasisAsset() const { return BasisAsset; }
	float                            GetOffsetScaleValue() const { return OffsetScale; }
	uint32                           GetActiveLOD() const { return Core.ActiveLODForFrame; }

private:
	FWOCDeformerInferenceCore Core;

	TWeakObjectPtr<USkeletalMeshComponent>  CachedSkelMesh;

	/** LOD index resolved in PreUpdate (passed to Evaluate_AnyThread) */
	uint32 PreparedLODIndex = INDEX_NONE;

	/** Batch subsystem slot handle for this frame (INDEX_NONE = fallback to per-node inference) */
	int32 BatchSlotHandle = INDEX_NONE;

	/** Cached subsystem pointer (set in PreUpdate GT, used in Evaluate WT) */
	UWOCBatchSubsystem* CachedBatchSubsystem = nullptr;

	/** JointNames -> compact pose bone index (int32) mapping. INDEX_NONE = no match */
	TArray<int32> CompactBoneIndices;

	/** For tight FK loop: parent index for compact index 0..MaxCSIdx (rebuilt on BoneContainer change) */
	TArray<int32> CachedParentIndices;

	/** For tight FK loop: CS rotation cache for compact index 0..MaxCSIdx (overwritten every frame) */
	TArray<FQuat> CachedCSRotations;

	/** Max compact index within CompactBoneIndices. Determines the end of the tight FK loop. */
	int32 MaxCSIdx = INDEX_NONE;

	/** Pre-allocated rotation buffers to avoid per-frame SetNumZeroed heap allocation */
	TArray<float> CachedRLocalData;
	TArray<float> CachedRGlobalData;
};
