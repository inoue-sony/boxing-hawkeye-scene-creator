#include "WOCDeformerDataInterface.h"
#include "WOCBasisAsset.h"
#include "AnimNode_WOCDeformer.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimClassInterface.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "ShaderParameterMetadataBuilder.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogWOCDI, Log, All);

BEGIN_SHADER_PARAMETER_STRUCT(FWOCDeformerShaderParams, )
	SHADER_PARAMETER(uint32, NumVertices)        // V
	SHADER_PARAMETER(uint32, NumJoints)          // J (total joints for coefficients)
	SHADER_PARAMETER(uint32, JN)                 // J*N
	SHADER_PARAMETER(uint32, MaxJointsPerVertex) // MAX_JOINT to compute WOC (compressed)
	SHADER_PARAMETER(uint32, N)                  // Num of basis per joint (0-12 controlled by quality slider)
	SHADER_PARAMETER(float,  OffsetScale)
	SHADER_PARAMETER(uint32, bValid)             // 0: pass-through (WOC data not ready), 1: normal inference
	SHADER_PARAMETER(uint32, NActive)            // actual number of bases to loop (quality slider value)
	SHADER_PARAMETER(uint32, HashTableSize)      // UV hash table size (0 = no hash table)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, CoeffBuffer)        // [J*N]
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, RglobalBuffer)      // [J*9]
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<int>,   SparseIndexBuffer)  // [V*K]
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, PackedValuesBuffer) // [NNZ*3]
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<int>,   JointTableBuffer)   // [V*MAX_JOINT]
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>,  HashKeysBuffer)     // [HashTableSize]
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<int>,   HashValuesBuffer)   // [HashTableSize]
END_SHADER_PARAMETER_STRUCT()

// ===========================================================================
// UWOCDeformerDataInterface
// ===========================================================================

TSubclassOf<UActorComponent> UWOCDeformerDataInterface::GetRequiredComponentClass() const
{
	return USkinnedMeshComponent::StaticClass();
}

void UWOCDeformerDataInterface::GetSupportedInputs(
	TArray<FShaderFunctionDefinition>& OutFunctions) const
{
	OutFunctions.AddDefaulted_GetRef()
		.SetName(TEXT("ReadNumVertices"))
		.AddReturnType(EShaderFundamentalType::Uint);

	OutFunctions.AddDefaulted_GetRef()
		.SetName(TEXT("ReadWOCOffset"))
		.AddReturnType(EShaderFundamentalType::Float, 3)
		.AddParam(EShaderFundamentalType::Uint);
}

void UWOCDeformerDataInterface::GetShaderParameters(
	TCHAR const*                          UID,
	FShaderParametersMetadataBuilder&     InOutBuilder,
	FShaderParametersMetadataAllocations& InOutAllocations) const
{
	InOutBuilder.AddNestedStruct<FWOCDeformerShaderParams>(UID);
}

void UWOCDeformerDataInterface::GetHLSL(
	FString& OutHLSL, FString const& InDataInterfaceName) const
{
	const FString& Dn = InDataInterfaceName;

	// Variable declarations
	OutHLSL += FString::Printf(TEXT(
		"uint   %s_NumVertices;\n"
		"uint   %s_NumJoints;\n"
		"uint   %s_JN;\n"
		"uint   %s_MaxJointsPerVertex;\n"
		"uint   %s_N;\n"
		"float  %s_OffsetScale;\n"
		"uint   %s_bValid;\n"
		"uint   %s_NActive;\n"
		"uint   %s_HashTableSize;\n"
		"Buffer<float> %s_CoeffBuffer;\n"
		"Buffer<float> %s_RglobalBuffer;\n"
		"Buffer<int>   %s_SparseIndexBuffer;\n"
		"Buffer<float> %s_PackedValuesBuffer;\n"
		"Buffer<int>   %s_JointTableBuffer;\n"
		"Buffer<uint>  %s_HashKeysBuffer;\n"
		"Buffer<int>   %s_HashValuesBuffer;\n"
		"\n"
		"uint ReadNumVertices_%s() { return %s_NumVertices; }\n"
		),
		*Dn,*Dn,*Dn,*Dn,*Dn,*Dn,*Dn,*Dn,*Dn,  // scalars (9)
		*Dn,*Dn,*Dn,*Dn,*Dn,*Dn,*Dn,            // buffers (7)
		*Dn,*Dn);                                  // ReadNumVertices

	// ReadWOCOffset — Sparse version, input = mesh vertex ID
	// (vertex ID is read from UV2.y by kernel)
	OutHLSL += FString::Printf(TEXT(
		"\n"
		"float3 ReadWOCOffset_%s(uint v)\n"
		"{\n"
		"    if (!%s_bValid)          { return float3(0,0,0); }\n"
		"    if (v >= %s_NumVertices) { return float3(0,0,0); }\n"
		"    float3 offset = float3(0,0,0);\n"
		"    uint K = %s_MaxJointsPerVertex * %s_N;\n"
		"    for (uint slot = 0u; slot < %s_MaxJointsPerVertex; ++slot)\n"
		"    {\n"
		"        int j = %s_JointTableBuffer[v * %s_MaxJointsPerVertex + slot];\n"
		"        if (j < 0) { continue; }\n"
		"        float3 localDeform = float3(0,0,0);\n"
		"        uint cBase = (uint)j * %s_N;\n"
		"        uint tBase = v * K + slot * %s_N;\n"
		"        for (uint i = 0u; i < %s_NActive; ++i)\n"
		"        {\n"
		"            int sparseIdx = %s_SparseIndexBuffer[tBase + i];\n"
		"            if (sparseIdx < 0) { continue; }\n"
		"            float c = %s_CoeffBuffer[cBase + i];\n"
		"            if (abs(c) < 1e-7) { continue; }\n"
		"            uint pIdx = (uint)sparseIdx * 3u;\n"
		"            localDeform.x += c * %s_PackedValuesBuffer[pIdx];\n"
		"            localDeform.y += c * %s_PackedValuesBuffer[pIdx + 1u];\n"
		"            localDeform.z += c * %s_PackedValuesBuffer[pIdx + 2u];\n"
		"        }\n"
		"        uint rBase = (uint)j * 9u;\n"
		"        float3x3 Rg = float3x3(\n"
		"            %s_RglobalBuffer[rBase+0u],%s_RglobalBuffer[rBase+1u],%s_RglobalBuffer[rBase+2u],\n"
		"            %s_RglobalBuffer[rBase+3u],%s_RglobalBuffer[rBase+4u],%s_RglobalBuffer[rBase+5u],\n"
		"            %s_RglobalBuffer[rBase+6u],%s_RglobalBuffer[rBase+7u],%s_RglobalBuffer[rBase+8u]\n"
		"        );\n"
		"        offset += mul(Rg, localDeform);\n"
		"    }\n"
		"    return offset * %s_OffsetScale;\n"
		"}\n\n"
		),
		*Dn,*Dn,                               // function name, bValid
		*Dn,                                   // NumVertices
		*Dn,*Dn,*Dn,                           // K calc (MaxJointsPerVertex, N, MaxJointsPerVertex)
		*Dn,*Dn,                               // JointTable (JointTableBuffer, MaxJointsPerVertex)
		*Dn,*Dn,*Dn,                           // cBase(N), tBase(N), NActive loop
		*Dn,                                   // SparseIndexBuffer
		*Dn,                                   // CoeffBuffer
		*Dn,*Dn,*Dn,                           // PackedValues x,y,z
		*Dn,*Dn,*Dn,*Dn,*Dn,*Dn,*Dn,*Dn,*Dn, // Rg (9)
		*Dn);                                   // OffsetScale
}



UComputeDataProvider* UWOCDeformerDataInterface::CreateDataProvider(
	TObjectPtr<UObject> InBinding, uint64 InInputMask, uint64 InOutputMask) const
{
	UWOCDeformerDataProvider* Provider = NewObject<UWOCDeformerDataProvider>();

	if (USkeletalMeshComponent* SkelComp = Cast<USkeletalMeshComponent>(InBinding))
	{
		// Only store SkelComp. AnimInstance/AnimNode are resolved dynamically each frame.
		Provider->BoundSkelMesh = SkelComp;
	}

	return Provider;
}

// ===========================================================================
// UWOCDeformerDataProvider
// ===========================================================================

namespace
{
	struct FWOCResolvedSource
	{
		const FWOCDeformerInferenceCore* Core = nullptr;
		TObjectPtr<UWOCBasisAsset>       Basis = nullptr;
		float                            OffsetScale = 1.0f;
		uint32                           ActiveLOD = INDEX_NONE;
	};

	// Find FAnimNode_WOCDeformer from a single AnimInstance
	static bool TryResolveFromAnimInstance(UAnimInstance* AI, FWOCResolvedSource& Out)
	{
		if (!AI) { return false; }

		IAnimClassInterface* AnimClass = IAnimClassInterface::GetFromClass(AI->GetClass());
		if (!AnimClass) { return false; }

		const TArray<FStructProperty*>& NodeProps = AnimClass->GetAnimNodeProperties();
		for (FStructProperty* Prop : NodeProps)
		{
			if (Prop->Struct == FAnimNode_WOCDeformer::StaticStruct())
			{
				FAnimNode_WOCDeformer* Node =
					Prop->ContainerPtrToValuePtr<FAnimNode_WOCDeformer>(AI);
				if (Node)
				{
					Out.Core        = &Node->GetCore();
					Out.Basis       = Node->GetBasisAsset();
					Out.OffsetScale = Node->GetOffsetScaleValue();
					Out.ActiveLOD   = Node->GetActiveLOD();
					return true;
				}
			}
		}
		return false;
	}

	static FWOCResolvedSource ResolveSource(UWOCDeformerDataProvider* Self)
	{
		FWOCResolvedSource Out;

		USkeletalMeshComponent* SkelMesh = Self->BoundSkelMesh.Get();
		if (!SkelMesh)
		{
			UE_LOG(LogWOCDI, Warning, TEXT("[WOCDI] ResolveSource: BoundSkelMesh is null"));
			return Out;
		}

		// Search all AnimInstances (Main / PostProcess / Linked)
		// Order: PostProcess -> Main -> Linked
		if (UAnimInstance* PostAI = SkelMesh->GetPostProcessInstance())
		{
			if (TryResolveFromAnimInstance(PostAI, Out)) { return Out; }
		}

		if (UAnimInstance* MainAI = SkelMesh->GetAnimInstance())
		{
			if (TryResolveFromAnimInstance(MainAI, Out)) { return Out; }
		}

		// Linked Anim Instances: access via const pointer to use the const overload
		const USkeletalMeshComponent* ConstSkelMesh = SkelMesh;
		for (UAnimInstance* Linked : ConstSkelMesh->GetLinkedAnimInstances())
		{
			if (TryResolveFromAnimInstance(Linked, Out)) { return Out; }
		}

		// --- Diagnostic log when not found (re-output approximately every 5 seconds) ---
		// Using GFrameCounter instead of a static bool ensures logs are emitted
		// reliably across PIE sessions and after respawns
		static uint64 LastDumpFrame = UINT64_MAX;
		const uint64 CurrentFrame = (uint64)GFrameCounter;
		if (CurrentFrame - LastDumpFrame > 300)
		{
			LastDumpFrame = CurrentFrame;
			UAnimInstance* MainAI = SkelMesh->GetAnimInstance();
			UAnimInstance* PostAI = SkelMesh->GetPostProcessInstance();
			const int32 LinkedCount = ConstSkelMesh->GetLinkedAnimInstances().Num();
			UE_LOG(LogWOCDI, Warning,
				TEXT("[WOCDI] ResolveSource DUMP: SkelMesh=%s Main=%s Post=%s LinkedCount=%d"),
				*SkelMesh->GetName(),
				MainAI ? *MainAI->GetClass()->GetName() : TEXT("null"),
				PostAI ? *PostAI->GetClass()->GetName() : TEXT("null"),
				LinkedCount);

			auto DumpProps = [](UAnimInstance* AI, const TCHAR* Tag)
			{
				if (!AI) return;
				IAnimClassInterface* AC = IAnimClassInterface::GetFromClass(AI->GetClass());
				if (!AC)
				{
					UE_LOG(LogWOCDI, Warning, TEXT("[WOCDI]   %s %s: no IAnimClassInterface"),
						Tag, *AI->GetClass()->GetName());
					return;
				}
				const auto& Props = AC->GetAnimNodeProperties();
				UE_LOG(LogWOCDI, Warning, TEXT("[WOCDI]   %s %s: %d AnimNode props"),
					Tag, *AI->GetClass()->GetName(), Props.Num());
				for (FStructProperty* P : Props)
				{
					UE_LOG(LogWOCDI, Warning, TEXT("[WOCDI]     - %s (%s)"),
						*P->GetName(), *P->Struct->GetName());
				}
			};
			DumpProps(MainAI, TEXT("Main"));
			DumpProps(PostAI, TEXT("Post"));
			for (UAnimInstance* L : ConstSkelMesh->GetLinkedAnimInstances())
			{
				DumpProps(L, TEXT("Linked"));
			}
		}

		return Out;
	}
}

FComputeDataProviderRenderProxy* UWOCDeformerDataProvider::GetRenderProxy()
{
	// When WOC data is unresolved, dispatch kernel with the last valid vertex count to write zero offsets.
	// (Prevents output buffer from going stale during initial frames or while waiting for bGPUBufferReady)
	auto MakePassThrough = [this]() -> FComputeDataProviderRenderProxy*
	{
		return new FWOCDeformerDataProviderProxy(
			nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
			CachedNumVertices, 0, 0, 0,
			0, /*NActive=*/0, 1.0f, /*HashTableSize=*/0,
			/*bValid=*/false);
	};

	const FWOCResolvedSource Src = ResolveSource(this);

	const FWOCBasisLOD* LOD =
		(Src.Core && Src.Basis) ? Src.Basis->GetLOD(Src.ActiveLOD) : nullptr;

	if (!Src.Core)
	{
		UE_LOG(LogWOCDI, Warning, TEXT("[WOCDataInterface] GetRenderProxy: Core is null (no Component and no AnimNode found)"));
		return MakePassThrough();
	}
	if (!Src.Basis)
	{
		UE_LOG(LogWOCDI, Warning, TEXT("[WOCDataInterface] GetRenderProxy: BasisAsset is null"));
		return MakePassThrough();
	}
	if (Src.ActiveLOD == INDEX_NONE || !LOD)
	{
		UE_LOG(LogWOCDI, Warning, TEXT("[WOCDataInterface] GetRenderProxy: ActiveLOD=%d invalid (BasisAsset LODs=%d)"),
			Src.ActiveLOD, Src.Basis->GetNumLODs());
		return MakePassThrough();
	}
	if (!LOD->bGPUBufferReady)
	{
		UE_LOG(LogWOCDI, Warning, TEXT("[WOCDataInterface] GetRenderProxy: LOD[%d].bGPUBufferReady==false"), Src.ActiveLOD);
		return MakePassThrough();
	}
	if (!Src.Core->bGPUBufferReady)
	{
		UE_LOG(LogWOCDI, Warning, TEXT("[WOCDataInterface] GetRenderProxy: Core GPU buffers not ready"));
		return MakePassThrough();
	}

	UE_LOG(LogWOCDI, Verbose, TEXT("[WOCDataInterface] GetRenderProxy: OK LOD=%d V=%d J=%d N=%d"),
		Src.ActiveLOD, LOD->NumVertices, LOD->NumJoints, LOD->NumCoeffs);

	CachedNumVertices = LOD->NumVertices;  // Update cache for pass-through on subsequent frames
	const uint32 BasisEffectiveN = Src.Basis->GetEffectiveN(*LOD);

	// Clamp NActive to the ONNX model's actual per-joint coefficient count to prevent
	// HLSL from reading past the end of CoeffBuffer when BasisAsset N > ONNX N.
	const uint32 CoreNumCoeffs = (Src.Core->GetNumJoints() > 0)
		? (Src.Core->GetJN() / Src.Core->GetNumJoints())
		: BasisEffectiveN;
	const uint32 EffectiveN = FMath::Min(BasisEffectiveN, CoreNumCoeffs);

	return new FWOCDeformerDataProviderProxy(
		Src.Core->CoeffGPUBuffer,
		Src.Core->RglobalGPUBuffer,
		LOD->SparseIndexGPUBuffer,
		LOD->PackedValuesGPUBuffer,
		LOD->JointTableGPUBuffer,
		LOD->HashKeysGPUBuffer,
		LOD->HashValuesGPUBuffer,
		LOD->NumVertices,
		LOD->NumJoints,
		LOD->NumJoints * LOD->NumCoeffs,
		LOD->MaxJointsPerVertex,
		LOD->NumCoeffs,       // N: buffer stride (full N, does not change)
		EffectiveN,           // NActive: loop upper bound controlled by slider
		Src.OffsetScale,
		LOD->HashTableSize,
		/*bValid=*/true);
}

// ===========================================================================
// FWOCDeformerDataProviderProxy
// ===========================================================================

FWOCDeformerDataProviderProxy::FWOCDeformerDataProviderProxy(
	TRefCountPtr<FRDGPooledBuffer> InCoeffBuffer,
	TRefCountPtr<FRDGPooledBuffer> InRglobalBuffer,
	TRefCountPtr<FRDGPooledBuffer> InSparseIndexBuffer,
	TRefCountPtr<FRDGPooledBuffer> InPackedValuesBuffer,
	TRefCountPtr<FRDGPooledBuffer> InJointTableBuffer,
	TRefCountPtr<FRDGPooledBuffer> InHashKeysBuffer,
	TRefCountPtr<FRDGPooledBuffer> InHashValuesBuffer,
	uint32 InNumVertices, uint32 InNumJoints,
	uint32 InJN, uint32 InMaxJointsPerVertex,
	uint32 InN, uint32 InNActive, float InOffsetScale,
	uint32 InHashTableSize,
	bool bInValid)
	: CoeffBuffer(InCoeffBuffer)
	, RglobalBuffer(InRglobalBuffer)
	, SparseIndexBuffer(InSparseIndexBuffer)
	, PackedValuesBuffer(InPackedValuesBuffer)
	, JointTableBuffer(InJointTableBuffer)
	, HashKeysBuffer(InHashKeysBuffer)
	, HashValuesBuffer(InHashValuesBuffer)
	, NumVertices(InNumVertices)
	, NumJoints(InNumJoints)
	, JN(InJN)
	, MaxJointsPerVertex(InMaxJointsPerVertex)
	, N(InN)
	, NActive(InNActive)
	, OffsetScale(InOffsetScale)
	, HashTableSize(InHashTableSize)
	, bValid(bInValid)
{
}

int32 FWOCDeformerDataProviderProxy::GetDispatchThreadCount(
	TArray<FIntVector>& ThreadCounts) const
{
	for (FIntVector& C : ThreadCounts) { C.X = (int32)NumVertices; }
	return (int32)NumVertices;
}

bool FWOCDeformerDataProviderProxy::IsValid(FValidationData const& InValidationData) const
{
	// Even when bValid=false (pass-through), dispatch the kernel if NumVertices > 0.
	// HLSL checks bValid==0 and writes zero offsets, preventing stale output buffers.
	return InValidationData.ParameterStructSize == sizeof(FWOCDeformerShaderParams)
	    && NumVertices > 0;
}

void FWOCDeformerDataProviderProxy::AllocateResources(
	FRDGBuilder& GraphBuilder, FAllocationData const& InAllocationData)
{
	auto RegOrDummy = [&](TRefCountPtr<FRDGPooledBuffer>& Pool, const TCHAR* Name) -> FRDGBufferRef
	{
		if (Pool.IsValid())
		{
			return GraphBuilder.RegisterExternalBuffer(Pool);
		}
		FRDGBufferRef Dummy = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(float), 1), Name);
		AddClearUAVPass(GraphBuilder,
			GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Dummy, PF_R32_FLOAT)), 0u);
		return Dummy;
	};

	CoeffSRV        = GraphBuilder.CreateSRV(RegOrDummy(CoeffBuffer,        TEXT("WOC_DummyCoeff")),        PF_R32_FLOAT);
	RglobalSRV      = GraphBuilder.CreateSRV(RegOrDummy(RglobalBuffer,      TEXT("WOC_DummyRglobal")),      PF_R32_FLOAT);
	SparseIndexSRV  = GraphBuilder.CreateSRV(RegOrDummy(SparseIndexBuffer,  TEXT("WOC_DummySparseIdx")),    PF_R32_SINT);
	PackedValuesSRV = GraphBuilder.CreateSRV(RegOrDummy(PackedValuesBuffer, TEXT("WOC_DummyPackedVals")),   PF_R32_FLOAT);
	JointTableSRV   = GraphBuilder.CreateSRV(RegOrDummy(JointTableBuffer,   TEXT("WOC_DummyJointTable")),   PF_R32_SINT);
	HashKeysSRV     = GraphBuilder.CreateSRV(RegOrDummy(HashKeysBuffer,     TEXT("WOC_DummyHashKeys")),     PF_R32_UINT);
	HashValuesSRV   = GraphBuilder.CreateSRV(RegOrDummy(HashValuesBuffer,   TEXT("WOC_DummyHashValues")),   PF_R32_SINT);
}

void FWOCDeformerDataProviderProxy::GatherDispatchData(FDispatchData const& InDispatchData)
{
	const TStridedView<FWOCDeformerShaderParams> ParamArray =
		MakeStridedParameterView<FWOCDeformerShaderParams>(InDispatchData);

	for (int32 i = 0; i < ParamArray.Num(); ++i)
	{
		FWOCDeformerShaderParams& P = ParamArray[i];
		P.NumVertices        = NumVertices;
		P.NumJoints          = NumJoints;
		P.JN                 = JN;
		P.MaxJointsPerVertex = MaxJointsPerVertex;
		P.N                  = N;
		P.OffsetScale        = OffsetScale;
		P.bValid             = bValid ? 1u : 0u;
		P.NActive            = NActive;
		P.HashTableSize      = HashTableSize;
		P.CoeffBuffer        = CoeffSRV;
		P.RglobalBuffer      = RglobalSRV;
		P.SparseIndexBuffer  = SparseIndexSRV;
		P.PackedValuesBuffer = PackedValuesSRV;
		P.JointTableBuffer   = JointTableSRV;
		P.HashKeysBuffer     = HashKeysSRV;
		P.HashValuesBuffer   = HashValuesSRV;
	}
}
