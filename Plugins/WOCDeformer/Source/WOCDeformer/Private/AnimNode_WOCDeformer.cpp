#include "AnimNode_WOCDeformer.h"

#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"

#include "WOCBasisAsset.h"
#include "WOCBatchSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogWOCAnimNode, Log, All);

// STATGROUP_WOCDeformer is declared in WOCDeformerCore.h
DECLARE_CYCLE_STAT(TEXT("WOC SourceEval"),       STAT_WOC_SourceEval,      STATGROUP_WOCDeformer);
DECLARE_CYCLE_STAT(TEXT("WOC RLocal"),            STAT_WOC_RLocal,          STATGROUP_WOCDeformer);
DECLARE_CYCLE_STAT(TEXT("WOC RGlobal Total"),     STAT_WOC_RGlobal_Total,   STATGROUP_WOCDeformer);
DECLARE_CYCLE_STAT(TEXT("WOC RGlobal TightFK"),   STAT_WOC_TightFK,         STATGROUP_WOCDeformer);
DECLARE_CYCLE_STAT(TEXT("WOC RGlobal WriteRot"),  STAT_WOC_RGlobal_WriteRot,STATGROUP_WOCDeformer);
DECLARE_CYCLE_STAT(TEXT("WOC RunFrame"),          STAT_WOC_RunFrame,        STATGROUP_WOCDeformer);
DECLARE_CYCLE_STAT(TEXT("WOC PreUpdate"),         STAT_WOC_PreUpdate,       STATGROUP_WOCDeformer);
DECLARE_CYCLE_STAT(TEXT("WOC PreUpdate GPU"),     STAT_WOC_EnsureGPU,       STATGROUP_WOCDeformer);

void FAnimNode_WOCDeformer::Initialize_AnyThread(const FAnimationInitializeContext& Context)
{
	FAnimNode_Base::Initialize_AnyThread(Context);
	Source.Initialize(Context);

	CachedSkelMesh.Reset();
	PreparedLODIndex    = INDEX_NONE;
	CompactBoneIndices.Reset();
	CachedParentIndices.Reset();
	CachedCSRotations.Reset();
	MaxCSIdx = INDEX_NONE;
	CachedRLocalData.Reset();
	CachedRGlobalData.Reset();
}

void FAnimNode_WOCDeformer::CacheBones_AnyThread(const FAnimationCacheBonesContext& Context)
{
	Source.CacheBones(Context);
}

void FAnimNode_WOCDeformer::Update_AnyThread(const FAnimationUpdateContext& Context)
{
	GetEvaluateGraphExposedInputs().Execute(Context);
	Source.Update(Context);
}

void FAnimNode_WOCDeformer::Evaluate_AnyThread(FPoseContext& Output)
{
	// Always evaluate upstream pose (pass-through — does not modify pose)
	{
		SCOPE_CYCLE_COUNTER(STAT_WOC_SourceEval);
		Source.Evaluate(Output);
	}

	if (!Core.IsModelReady())
	{
		UE_LOG(LogWOCAnimNode, Warning, TEXT("[WOCAnimNode] Evaluate: Model not ready. ModelData=%s BasisAsset=%s"),
			ModelData ? *ModelData->GetName() : TEXT("None"),
			BasisAsset ? *BasisAsset->GetName() : TEXT("None"));
		return;
	}
	if (!Core.bGPUBufferReady)
	{
		UE_LOG(LogWOCAnimNode, Warning, TEXT("[WOCAnimNode] Evaluate: GPU buffers not ready"));
		return;
	}
	if (PreparedLODIndex == (uint32)INDEX_NONE)
	{
		UE_LOG(LogWOCAnimNode, Warning, TEXT("[WOCAnimNode] Evaluate: PreparedLODIndex==INDEX_NONE (BasisAsset LODs=%d)"),
			BasisAsset ? BasisAsset->GetNumLODs() : -1);
		return;
	}

	const uint32 J = Core.GetNumJoints();
	if (J == 0)
	{
		UE_LOG(LogWOCAnimNode, Warning, TEXT("[WOCAnimNode] Evaluate: NumJoints==0"));
		return;
	}

	const FBoneContainer&   BoneContainer = Output.Pose.GetBoneContainer();
	const TArray<FName>&    JointNames    = Core.GetJointNames();

	// --- (Re)build CompactPoseBoneIndex ---
	if (CompactBoneIndices.Num() != JointNames.Num())
	{
		CompactBoneIndices.Reset(JointNames.Num());
		uint32 Mapped = 0;
		for (int32 j = 0; j < JointNames.Num(); ++j)
		{
			const int32 RefIdx =
				BoneContainer.GetReferenceSkeleton().FindBoneIndex(JointNames[j]);
			const FCompactPoseBoneIndex Compact = (RefIdx != INDEX_NONE)
				? BoneContainer.MakeCompactPoseIndex(FMeshPoseBoneIndex(RefIdx))
				: FCompactPoseBoneIndex(INDEX_NONE);
			CompactBoneIndices.Add(Compact.GetInt());
			if (Compact.IsValid()) { ++Mapped; }
			else
			{
				UE_LOG(LogWOCAnimNode, Warning,
					TEXT("[WOCAnimNode] BoneIndex: joint '%s' not found in skeleton"),
					*JointNames[j].ToString());
			}
		}
		UE_LOG(LogWOCAnimNode, Verbose, TEXT("[WOCAnimNode] BoneIndex map built: %u/%d joints mapped"),
			Mapped, JointNames.Num());

		// Pre-allocate rotation buffers to match joint count.
		// Subsequent frames only need Memzero, eliminating per-frame malloc/free.
		CachedRLocalData.SetNumUninitialized((int32)(J * 9));
		CachedRGlobalData.SetNumUninitialized((int32)(J * 9));

		// For tight FK: find the max compact index in CompactBoneIndices,
		// then pre-cache parent indices for 0..MaxCSIdx.
		// Updated together with CompactBoneIndices when BoneContainer changes.
		MaxCSIdx = INDEX_NONE;
		for (int32 j = 0; j < CompactBoneIndices.Num(); ++j)
		{
			if (CompactBoneIndices[j] != INDEX_NONE)
				MaxCSIdx = FMath::Max(MaxCSIdx, CompactBoneIndices[j]);
		}
		if (MaxCSIdx != INDEX_NONE)
		{
			CachedParentIndices.SetNumUninitialized(MaxCSIdx + 1);
			for (int32 i = 0; i <= MaxCSIdx; ++i)
				CachedParentIndices[i] =
					BoneContainer.GetParentBoneIndex(FCompactPoseBoneIndex(i)).GetInt();
			CachedCSRotations.SetNumUninitialized(MaxCSIdx + 1);
			UE_LOG(LogWOCAnimNode, Log, TEXT("[WOCAnimNode] MaxCSIdx=%d (TightFK loop size)"), MaxCSIdx);
		}
	}

	// --- Rotation matrix helper: UE coordinate -> model coordinate conversion (same sign convention as Component) ---
	auto WriteRot = [](float* Dst, const FQuat& Q)
	{
		const FMatrix M = Q.ToMatrix();
		Dst[0] =  (float)M.M[0][0]; Dst[1] = -(float)M.M[1][0]; Dst[2] =  (float)M.M[2][0];
		Dst[3] = -(float)M.M[0][1]; Dst[4] =  (float)M.M[1][1]; Dst[5] = -(float)M.M[2][1];
		Dst[6] =  (float)M.M[0][2]; Dst[7] = -(float)M.M[1][2]; Dst[8] =  (float)M.M[2][2];
	};

	// --- R_local: compact pose is already in local (parent-space), use directly ---
	FMemory::Memzero(CachedRLocalData.GetData(), J * 9 * sizeof(float));
	{
		SCOPE_CYCLE_COUNTER(STAT_WOC_RLocal);
		for (int32 j = 0; j < (int32)J && j < CompactBoneIndices.Num(); ++j)
		{
			const FCompactPoseBoneIndex Idx(CompactBoneIndices[j]);
			if (!Idx.IsValid()) { continue; }
			WriteRot(CachedRLocalData.GetData() + j * 9, Output.Pose[Idx].GetRotation());
		}
	}

	// --- R_global: tight FK loop ---
	// Eliminates FCSPose lazy flags and recursion; linearly accumulates compact index 0..MaxCSIdx.
	// CachedParentIndices is rebuilt once when BoneContainer changes.
	FMemory::Memzero(CachedRGlobalData.GetData(), J * 9 * sizeof(float));
	if (MaxCSIdx != INDEX_NONE)
	{
		SCOPE_CYCLE_COUNTER(STAT_WOC_RGlobal_Total);

		{
			SCOPE_CYCLE_COUNTER(STAT_WOC_RGlobal_WriteRot);

			// Step 1: compact index 0 -> MaxCSIdx, accumulate CS rotation in order
			//    compact pose is always parent < child order, so parent is guaranteed to be resolved during loop
			{
				SCOPE_CYCLE_COUNTER(STAT_WOC_TightFK);
				const int32 N = MaxCSIdx + 1;
				for (int32 i = 0; i < N; ++i)
				{
					const FQuat LocalRot = Output.Pose[FCompactPoseBoneIndex(i)].GetRotation();
					const int32 ParentInt = CachedParentIndices[i];
					CachedCSRotations[i] = (ParentInt >= 0)
						? CachedCSRotations[ParentInt] * LocalRot
						: LocalRot;
				}
			}

			// Step 2: Write only the relevant joints to R_global buffer
			for (int32 j = 0; j < (int32)J && j < CompactBoneIndices.Num(); ++j)
			{
				const int32 RawIdx = CompactBoneIndices[j];
				if (RawIdx < 0) { continue; }
				// Deformer Graph consumes offsets in component space. ComponentToWorld
				// applies the actor/component rotation after deformation.
				WriteRot(CachedRGlobalData.GetData() + j * 9, CachedCSRotations[RawIdx]);
			}
		}
	}

	// --- Inference or batch write ---
	if (BatchSlotHandle != INDEX_NONE && CachedBatchSubsystem)
	{
		// Batch path: write rotations to subsystem buffer (inference runs later in Tick)
		SCOPE_CYCLE_COUNTER(STAT_WOC_RunFrame);
		CachedBatchSubsystem->WriteInput(BatchSlotHandle, CachedRLocalData);
		CachedBatchSubsystem->WriteRGlobal(BatchSlotHandle, CachedRGlobalData);
	}
	else
	{
		// Fallback: per-node inference + immediate GPU upload
		SCOPE_CYCLE_COUNTER(STAT_WOC_RunFrame);
		Core.RunFrameWithRotations(CachedRLocalData, CachedRGlobalData, PreparedLODIndex);
	}
}

void FAnimNode_WOCDeformer::GatherDebugData(FNodeDebugData& DebugData)
{
	const FString Line = FString::Printf(
		TEXT("WOC Deformer (AnimNode): ModelData=%s BasisAsset=%s"),
		ModelData ? *ModelData->GetName() : TEXT("None"),
		BasisAsset ? *BasisAsset->GetName() : TEXT("None"));
	DebugData.AddDebugItem(Line);
	Source.GatherDebugData(DebugData);
}

void FAnimNode_WOCDeformer::PreUpdate(const UAnimInstance* InAnimInstance)
{
	SCOPE_CYCLE_COUNTER(STAT_WOC_PreUpdate);

	if (!InAnimInstance) { return; }

	TObjectPtr<USkeletalMeshComponent> SkelMesh = InAnimInstance->GetSkelMeshComponent();
	if (!SkelMesh) { return; }
	CachedSkelMesh = SkelMesh;

	// Set Core inputs and initialize model (no inference)
	Core.ModelData      = ModelData;
	Core.BasisAsset     = BasisAsset;
	Core.RuntimeName    = RuntimeName;
	Core.OffsetScale    = OffsetScale;
	Core.bLogLODChanges = bLogLODChanges;

	// init only (executed on game thread to read skeleton)
	const bool bInitOK = Core.TryInit(SkelMesh);
	if (!bInitOK)
	{
		UE_LOG(LogWOCAnimNode, Warning, TEXT("[WOCAnimNode] PreUpdate: TryInit FAILED ModelData=%s BasisAsset=%s"),
			ModelData ? *ModelData->GetName() : TEXT("None"),
			BasisAsset ? *BasisAsset->GetName() : TEXT("None"));
	}

	if (!Core.IsModelReady()) { PreparedLODIndex = INDEX_NONE; return; }

	// Resolve LOD and allocate basis buffer on GPU (must be on game thread)
	PreparedLODIndex = Core.ResolveEffectiveLOD(SkelMesh->GetPredictedLODLevel());
	if (PreparedLODIndex != (uint32)INDEX_NONE)
	{
		if (UWOCBasisAsset* Basis = BasisAsset.Get())
		{
			{
				SCOPE_CYCLE_COUNTER(STAT_WOC_EnsureGPU);
				Basis->EnsureGPUBuffers((int32)PreparedLODIndex);
			}
			const FWOCBasisLOD* LOD = Basis->GetLOD((int32)PreparedLODIndex);
			if (PreparedLODIndex != Core.ActiveLODForFrame)
			{
				// Log only when LOD changes
				UE_LOG(LogWOCAnimNode, Verbose,
					TEXT("[WOCAnimNode] LOD changed %u -> %u  V=%d J=%d N=%d GPUReady=%s"),
					Core.ActiveLODForFrame, PreparedLODIndex,
					LOD ? LOD->NumVertices : -1,
					LOD ? LOD->NumJoints   : -1,
					LOD ? LOD->NumCoeffs   : -1,
					(LOD && LOD->bGPUBufferReady) ? TEXT("Yes") : TEXT("No"));
			}
		}
		else
		{
			UE_LOG(LogWOCAnimNode, Warning, TEXT("[WOCAnimNode] PreUpdate: BasisAsset is null!"));
		}
	}
	else
	{
		UE_LOG(LogWOCAnimNode, Warning,
			TEXT("[WOCAnimNode] PreUpdate: ResolveEffectiveLOD returned INDEX_NONE (BasisAsset LODs=%d)"),
			BasisAsset ? BasisAsset->GetNumLODs() : -1);
	}

	// Register with batch subsystem for batched inference (same-frame, no latency)
	BatchSlotHandle = INDEX_NONE;
	CachedBatchSubsystem = nullptr;
	if (Core.bGPUBufferReady && PreparedLODIndex != (uint32)INDEX_NONE)
	{
		if (UWorld* World = SkelMesh->GetWorld())
		{
			CachedBatchSubsystem = World->GetSubsystem<UWOCBatchSubsystem>();
			if (CachedBatchSubsystem)
			{
				BatchSlotHandle = CachedBatchSubsystem->RegisterSlot(
					ModelData.Get(), RuntimeName, &Core);
			}
		}
		// Set ActiveLODForFrame so DataInterface can resolve the correct LOD
		Core.ActiveLODForFrame = PreparedLODIndex;
	}
}
