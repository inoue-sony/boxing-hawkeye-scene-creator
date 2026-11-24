#include "WOCDeformerCore.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "NNE.h"
#include "NNERuntime.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHI.h"
#include "Misc/ScopeExit.h"

#include "WOCBasisAsset.h"

DEFINE_LOG_CATEGORY_STATIC(LogWOCDeformerCore, Log, All);

// ===========================================================================
// FWOCModelCache
// ===========================================================================

TMap<TObjectKey<UNNEModelData>, FWOCModelCache::FCacheEntry> FWOCModelCache::Cache;
FCriticalSection                                   FWOCModelCache::Lock;

TSharedPtr<UE::NNE::IModelCPU> FWOCModelCache::GetOrCreate(
	UNNEModelData* ModelData, const FString& RuntimeName)
{
	FScopeLock ScopeLock(&Lock);

	if (FCacheEntry* Entry = Cache.Find(TObjectKey<UNNEModelData>(ModelData)))
	{
		Entry->RefCount++;
		return Entry->Model;
	}

	const TWeakInterfacePtr<INNERuntimeCPU> RuntimeCPU =
		UE::NNE::GetRuntime<INNERuntimeCPU>(RuntimeName);

	if (!RuntimeCPU.IsValid())
	{
		UE_LOG(LogWOCDeformerCore, Error,
		       TEXT("WOCModelCache: Cannot get CPU runtime '%s'."), *RuntimeName);
		return nullptr;
	}

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
	TSharedPtr<UE::NNE::IModelCPU> Model = RuntimeCPU->CreateModelCPU(ModelData);
#else
	TSharedPtr<UE::NNE::IModelCPU> Model = MakeShareable(RuntimeCPU->CreateModel(ModelData).Release());
#endif
	if (!Model.IsValid())
	{
		UE_LOG(LogWOCDeformerCore, Error, TEXT("WOCModelCache: CreateModelCPU failed."));
		return nullptr;
	}

	FCacheEntry& Entry = Cache.Add(TObjectKey<UNNEModelData>(ModelData));
	Entry.Model    = Model;
	Entry.RefCount = 1;
	return Model;
}

void FWOCModelCache::Release(UNNEModelData* ModelData)
{
	ReleaseByKey(TObjectKey<UNNEModelData>(ModelData));
}

void FWOCModelCache::ReleaseByKey(const TObjectKey<UNNEModelData>& ModelKey)
{
	FScopeLock ScopeLock(&Lock);
	if (FCacheEntry* Entry = Cache.Find(ModelKey))
	{
		if (Entry->RefCount <= 0)
		{
			UE_LOG(LogWOCDeformerCore, Warning, TEXT("WOCModelCache::Release called with RefCount already <= 0. Possible double-release."));
			return;
		}
		if (--Entry->RefCount <= 0) { Cache.Remove(ModelKey); }
	}
}

// ===========================================================================
// Public entry
// ===========================================================================

void FWOCDeformerInferenceCore::Release()
{
	if (CachedModelKey.IsSet())
	{
		FWOCModelCache::ReleaseByKey(CachedModelKey.GetValue());
	}
	ModelInstance.Reset();
	bModelReady = false;
	CachedModelKey.Reset();
	CoeffGPUBuffer.SafeRelease();
	RglobalGPUBuffer.SafeRelease();
	bGPUBufferReady = false;
	ActiveLODForFrame = INDEX_NONE;
}

FWOCDeformerInferenceCore::~FWOCDeformerInferenceCore()
{
	Release();
}

bool FWOCDeformerInferenceCore::TryInit(USkeletalMeshComponent* SkelComp)
{
	return EnsureModelReady(SkelComp);
}

void FWOCDeformerInferenceCore::RunFrameWithRotations(
	TConstArrayView<float> RLocalData,
	TConstArrayView<float> RGlobalData,
	uint32                  LODIndex)
{
	if (!bModelReady || !bGPUBufferReady) { return; }
	if (RLocalData.Num() != NumJoints * 9) { return; }

	ActiveLODForFrame = LODIndex;

	// Per-node inference (same-frame, no latency)
	FMemory::Memcpy(InputBuffer.GetData(), RLocalData.GetData(),
	                RLocalData.Num() * sizeof(float));
	RunInferenceCPU();

	UploadCoeffs();

	// Upload R_global
	if (RGlobalData.Num() == NumJoints * 9)
	{
		// Copy data into lambda-owned buffer to guarantee pointer validity on render thread
		TArray<float> RglobalCopy;
		RglobalCopy.SetNumUninitialized(RGlobalData.Num());
		FMemory::Memcpy(RglobalCopy.GetData(), RGlobalData.GetData(),
		                RGlobalData.Num() * sizeof(float));

		TRefCountPtr<FRDGPooledBuffer> BufRef = RglobalGPUBuffer;

		ENQUEUE_RENDER_COMMAND(WOCDeformer_UploadRglobal_AnimNode)
		(
			[BufRef, RglobalCopy = MoveTemp(RglobalCopy)]
			(FRHICommandListImmediate& RHICmdList)
			{
				FRDGBuilder Builder(RHICmdList);
				FRDGBufferRef Buf = Builder.RegisterExternalBuffer(BufRef);
				Builder.QueueBufferUpload(Buf, RglobalCopy.GetData(),
				                         RglobalCopy.Num() * sizeof(float),
				                         ERDGInitialDataFlags::NoCopy);
				Builder.Execute();
			}
		);
	}
}

// ===========================================================================
// LOD
// ===========================================================================

uint32 FWOCDeformerInferenceCore::ResolveEffectiveLOD(uint32 RequestedLOD) const
{
	TObjectPtr<UWOCBasisAsset> Basis = BasisAsset.Get();
	if (!Basis || Basis->GetNumLODs() == 0) { return INDEX_NONE; }
	return FMath::Clamp(RequestedLOD, 0, Basis->GetNumLODs() - 1);
}

// ===========================================================================
// Model init
// ===========================================================================

bool FWOCDeformerInferenceCore::EnsureModelReady(USkeletalMeshComponent* SkelComp)
{
	if (bModelReady) { return true; }

	TObjectPtr<UNNEModelData> Md = ModelData.Get();
	if (!Md)
	{
		UE_LOG(LogWOCDeformerCore, Warning, TEXT("WOCDeformer: ModelData is not set."));
		return false;
	}

	TObjectPtr<UWOCBasisAsset> Basis = BasisAsset.Get();
	if (!Basis)
	{
		UE_LOG(LogWOCDeformerCore, Error, TEXT("WOCDeformer: BasisAsset is not set."));
		return false;
	}
#if WITH_EDITOR
	if (!Basis->CacheModelMetadata(Md)) return false;
#endif
	{
		int32 Coefficients = 0;
		FString Error;
		if (!Basis->ResolveModelMetadata(Md, JointNames, Coefficients, Error))
		{
			UE_LOG(LogWOCDeformerCore, Error, TEXT("WOCDeformer: %s"), *Error);
			return false;
		}
		NumJoints = JointNames.Num();
		NumCoeffs = Coefficients;
	}

	if (NumJoints == 0) { NumJoints = JointNames.Num(); }
	if (NumJoints == 0)
	{
		UE_LOG(LogWOCDeformerCore, Error, TEXT("WOCDeformer: ONNX metadata 'max_joints' not found."));
		return false;
	}

	// Overflow check: NumJoints * NumCoeffs must fit in uint32
	const uint64 JN64 = (uint64)NumJoints * (uint64)NumCoeffs;
	if (JN64 > (uint64)MAX_uint32)
	{
		UE_LOG(LogWOCDeformerCore, Error, TEXT("WOCDeformer: JN overflow (%u * %u = %llu exceeds uint32 max)."), NumJoints, NumCoeffs, JN64);
		return false;
	}
	JN = (uint32)JN64;

	// JointName -> RefSkelIndex mapping
	{
		const FReferenceSkeleton& RefSkel =
			SkelComp->GetSkeletalMeshAsset()->GetRefSkeleton();
		JointToRefBoneIndex.Init(INDEX_NONE, JointNames.Num());
		uint32 Found = 0;
		for (uint32 j = 0; j < (uint32)JointNames.Num(); ++j)
		{
			const int32 Idx = RefSkel.FindBoneIndex(JointNames[j]);
			if (Idx != INDEX_NONE) { JointToRefBoneIndex[j] = Idx; ++Found; }
		}
		UE_LOG(LogWOCDeformerCore, Log, TEXT("WOCDeformer: joint mapping %u/%d"),
		       Found, JointNames.Num());
	}

	InputBuffer.SetNumZeroed((int32)(NumJoints * 9));
	OutputBuffer.SetNumZeroed((int32)JN);

	const TSharedPtr<UE::NNE::IModelCPU> Model = FWOCModelCache::GetOrCreate(Md, RuntimeName);
	if (!Model.IsValid()) { return false; }
	ON_SCOPE_EXIT
	{
		if (!bModelReady)
		{
			ModelInstance.Reset();
			FWOCModelCache::Release(Md);
		}
	};

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
	ModelInstance = Model->CreateModelInstanceCPU();
#else
	ModelInstance = MakeShareable(Model->CreateModelInstance().Release());
#endif
	if (!ModelInstance.IsValid()) { return false; }

	TArray<UE::NNE::FTensorShape> InputShapes;
	InputShapes.Add(UE::NNE::FTensorShape::Make({ 1, NumJoints, 3, 3 }));
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
	if (ModelInstance->SetInputTensorShapes(InputShapes) != UE::NNE::EResultStatus::Ok)
#else
	if (ModelInstance->SetInputTensorShapes(InputShapes) != 0)
#endif
	{
		UE_LOG(LogWOCDeformerCore, Error, TEXT("WOCDeformer: SetInputTensorShapes failed."));
		return false;
	}

	{
		const TConstArrayView<UE::NNE::FTensorDesc> OutDescs = ModelInstance->GetOutputTensorDescs();
		if (!OutDescs.IsEmpty())
		{
			const UE::NNE::FTensorShape OutShape =
				UE::NNE::FTensorShape::MakeFromSymbolic(OutDescs[0].GetShape());
			const int32 OutEl = OutShape.Volume();
			if (OutEl > 0 && OutEl != (int32)JN)
			{
				UE_LOG(LogWOCDeformerCore, Error,
				       TEXT("WOCDeformer: ONNX output size mismatch. expected=%u got=%d"),
				       JN, OutEl);
				return false;
			}
		}
	}

	CreateGPUBuffers();
	CachedModelKey.Emplace(Md.Get());
	bModelReady = true;

	UE_LOG(LogWOCDeformerCore, Log, TEXT("WOCDeformer: Model ready. J=%u N=%u JN=%u"),
	       NumJoints, NumCoeffs, JN);
	return true;
}

// ===========================================================================
// GPU buffer creation
// ===========================================================================

void FWOCDeformerInferenceCore::CreateGPUBuffers()
{
	if (!IsInGameThread())
	{
		UE_LOG(LogWOCDeformerCore, Error, TEXT("WOCDeformer: CreateGPUBuffers must be called from the game thread."));
		return;
	}
	if (!GIsRHIInitialized)
	{
		UE_LOG(LogWOCDeformerCore, Error, TEXT("WOCDeformer: RHI not initialized. Cannot create GPU buffers."));
		return;
	}

	const uint32 InJN = JN;
	const uint32 J9   = NumJoints * 9;

	// Struct to return results from RT to GT (eliminates raw pointer captures)
	struct FGPUCoreResult
	{
		TRefCountPtr<FRDGPooledBuffer> CoeffBuf;
		TRefCountPtr<FRDGPooledBuffer> RglobalBuf;
	};
	TSharedRef<FGPUCoreResult> Result = MakeShared<FGPUCoreResult>();

	ENQUEUE_RENDER_COMMAND(WOCDeformer_CreateBuffers)
	(
		[InJN, J9, Result]
		(FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder Builder(RHICmdList);

			auto MakeBuf = [&](uint32 Count, const TCHAR* Name)
				-> TRefCountPtr<FRDGPooledBuffer>
			{
				FRDGBufferDesc Desc =
					FRDGBufferDesc::CreateBufferDesc(sizeof(float), Count);
				Desc.Usage = EBufferUsageFlags::ShaderResource
				           | EBufferUsageFlags(BUF_SourceCopy);
				FRDGBufferRef Buf = Builder.CreateBuffer(Desc, Name);
				return Builder.ConvertToExternalBuffer(Buf);
			};

			Result->CoeffBuf   = MakeBuf(InJN, TEXT("WOCDeformer.Coeff"));
			Result->RglobalBuf = MakeBuf(J9,   TEXT("WOCDeformer.Rglobal"));

			Builder.Execute();
		}
	);

	FRenderCommandFence Fence;
	Fence.BeginFence();
	Fence.Wait();

	// Write back to GT members after Fence.Wait() completes
	CoeffGPUBuffer   = MoveTemp(Result->CoeffBuf);
	RglobalGPUBuffer = MoveTemp(Result->RglobalBuf);
	bGPUBufferReady  = true;
}

// ===========================================================================
// Per-frame processing
// ===========================================================================

void FWOCDeformerInferenceCore::RunInferenceCPU()
{
	if (!ModelInstance.IsValid()) { return; }

	TArray<UE::NNE::FTensorBindingCPU> InputBindings;
	{
		UE::NNE::FTensorBindingCPU& B = InputBindings.AddDefaulted_GetRef();
		B.Data        = InputBuffer.GetData();
		B.SizeInBytes = InputBuffer.Num() * sizeof(float);
	}
	TArray<UE::NNE::FTensorBindingCPU> OutputBindings;
	{
		UE::NNE::FTensorBindingCPU& B = OutputBindings.AddDefaulted_GetRef();
		B.Data        = OutputBuffer.GetData();
		B.SizeInBytes = OutputBuffer.Num() * sizeof(float);
	}
	ModelInstance->RunSync(InputBindings, OutputBindings);
}

void FWOCDeformerInferenceCore::UploadCoeffs()
{
	if (!bGPUBufferReady) { return; }

	// Copy data into lambda-owned buffer to guarantee pointer validity on render thread
	TArray<float> CoeffCopy(OutputBuffer);
	TRefCountPtr<FRDGPooledBuffer> BufRef = CoeffGPUBuffer;

	ENQUEUE_RENDER_COMMAND(WOCDeformer_UploadCoeffs)
	(
		[BufRef, CoeffCopy = MoveTemp(CoeffCopy)]
		(FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder Builder(RHICmdList);
			FRDGBufferRef Buf = Builder.RegisterExternalBuffer(BufRef);
			Builder.QueueBufferUpload(Buf, CoeffCopy.GetData(),
			                         CoeffCopy.Num() * sizeof(float),
			                         ERDGInitialDataFlags::NoCopy);
			Builder.Execute();
		}
	);
}

// ===========================================================================
// ONNX metadata parser (minimal protobuf reader for field 14 = metadata_props)
// ===========================================================================

bool FWOCDeformerInferenceCore::ExtractModelMetadata(UNNEModelData* Model, TArray<FName>& Names, int32& Coefficients, FString& Error)
{
	Names.Reset();
	Coefficients = 0;
	if (!Model || Model->GetFileData().IsEmpty() || Model->GetFileData().Num() > MAX_int32)
	{ Error = TEXT("Raw ONNX data is unavailable for metadata capture; migrate the original model in Editor."); return false; }
	const auto Bytes = Model->GetFileData();
	const auto Meta = ParseONNXMetadata(TConstArrayView<uint8>(Bytes.GetData(), static_cast<int32>(Bytes.Num())));
	const FString* JointCountText = Meta.Find(TEXT("max_joints"));
	const FString* NamesText = Meta.Find(TEXT("joint_names"));
	const FString* CoefficientsText = Meta.Find(TEXT("num_coeffs_per_joint"));
	int32 JointCount = 0;
	auto ParseCount = [](const FString& Text, int32& Value)
	{
		if (Text.IsEmpty()) return false;
		for (const TCHAR Character : Text) if (Character < TEXT('0') || Character > TEXT('9')) return false;
		return LexTryParseString(Value, *Text);
	};
	if (!CoefficientsText)
	{
		Coefficients = 12;
		UE_LOG(LogWOCDeformerCore, Warning, TEXT("WOC legacy ONNX omits num_coeffs_per_joint; retaining SDK default 12, validated against Basis dimensions and model output."));
	}
	if (!JointCountText || !NamesText
		|| !ParseCount(*JointCountText, JointCount) || (CoefficientsText && !ParseCount(*CoefficientsText, Coefficients))
		|| JointCount < 1 || JointCount > 1024 || Coefficients < 1 || Coefficients > 512)
	{ Error = TEXT("ONNX requires valid joint_names/max_joints and a valid coefficient count."); return false; }
	TArray<FString> Strings;
	NamesText->ParseIntoArray(Strings, TEXT(","), false);
	TSet<FName> Unique;
	for (const FString& Text : Strings)
	{
		const FName Name(*Text.TrimStartAndEnd());
		if (Name.IsNone() || Unique.Contains(Name)) { Error = TEXT("ONNX joint names must be nonempty and unique."); return false; }
		Names.Add(Name);
		Unique.Add(Name);
	}
	if (Names.Num() != JointCount) { Error = TEXT("ONNX joint_names length does not match max_joints."); return false; }
	return true;
}

namespace
{
	// Maximum length-delimited field size (256 MB) to reject corrupt data early
	static constexpr int64 MaxFieldLength = 256 * 1024 * 1024;

	static bool ReadVarint(TConstArrayView<uint8> B, int32& P, uint64& Out)
	{
		Out=0; int32 S=0;
		while(P<B.Num()){
			uint8 b=B[P++]; Out|=(uint64)(b&0x7F)<<S;
			if(!(b&0x80))return true; S+=7; if(S>=64)return false;
		} return false;
	}
	static bool ReadTag(TConstArrayView<uint8> B, int32& P, uint32& F, uint32& W)
	{ uint64 T; if(!ReadVarint(B,P,T))return false; F=(uint32)(T>>3); W=(uint32)(T&7); return true; }
	static bool ReadLD(TConstArrayView<uint8> B, int32& P, TConstArrayView<uint8>& Out)
	{
		uint64 L; if(!ReadVarint(B,P,L))return false;
		if((int64)L > MaxFieldLength) return false;  // reject absurdly large fields
		if(P+(int64)L>B.Num())return false;
		Out=TConstArrayView<uint8>(B.GetData()+P,(int32)L); P+=(int32)L; return true;
	}
	static bool SkipF(TConstArrayView<uint8> B, int32& P, uint32 W)
	{ switch(W){
	  case 0:{uint64 V;return ReadVarint(B,P,V);}
	  case 1:{if(P+8>B.Num())return false;P+=8;return true;}
	  case 2:{TConstArrayView<uint8> S;return ReadLD(B,P,S);}
	  case 5:{if(P+4>B.Num())return false;P+=4;return true;}
	  default:return false;} }
}

TMap<FString, FString> FWOCDeformerInferenceCore::ParseONNXMetadata(
	TConstArrayView<uint8> Bytes)
{
	TMap<FString, FString> R;

	if (Bytes.Num() == 0)
	{
		UE_LOG(LogWOCDeformerCore, Warning, TEXT("WOCDeformer: ParseONNXMetadata called with empty data."));
		return R;
	}

	int32 P=0;
	int32 ParsedCount = 0;
	while(P<Bytes.Num()){
		uint32 F,W; if(!ReadTag(Bytes,P,F,W))break;
		if(F==14&&W==2){
			TConstArrayView<uint8> E; if(!ReadLD(Bytes,P,E))break;
			FString K,V; int32 EP=0;
			while(EP<E.Num()){
				uint32 EF,EW; if(!ReadTag(E,EP,EF,EW))break;
				if(EW==2){ TConstArrayView<uint8> S; if(!ReadLD(E,EP,S))break;
				  FString Str=FString(S.Num(),(const UTF8CHAR*)S.GetData());
				  if(EF==1)K=MoveTemp(Str); else if(EF==2)V=MoveTemp(Str); }
				else{ if(!SkipF(E,EP,EW))break; }
			}
			if(!K.IsEmpty()){ R.Add(K,V); ++ParsedCount; }
		} else{ if(!SkipF(Bytes,P,W))break; }
	}

	if (ParsedCount == 0)
	{
		UE_LOG(LogWOCDeformerCore, Warning, TEXT("WOCDeformer: No metadata_props found in ONNX file (%d bytes). Model may be corrupt or missing custom metadata."), Bytes.Num());
	}

	return R;
}
