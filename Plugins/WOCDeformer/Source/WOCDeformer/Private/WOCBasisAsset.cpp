#include "WOCBasisAsset.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHI.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "WOCDeformerCore.h"
#include "NNEModelData.h"
#include "UObject/ObjectSaveContext.h"

#if WITH_EDITOR
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogWOCBasis, Log, All);

bool UWOCBasisAsset::ResolveModelMetadata(UNNEModelData* Model, TArray<FName>& Names, int32& Coefficients, FString& Error) const
{
	const auto* Entry = ModelMetadata.FindByPredicate([Model](const FWOCModelMetadata& Item) { return Item.ModelData == Model; });
	if (!Model || !Entry || Entry->Version != 1 || Entry->JointNames.IsEmpty() || Entry->JointNames.Num() > 1024
		|| Entry->NumCoeffs < 1 || Entry->NumCoeffs > 512)
	{
		Error = TEXT("Missing/invalid cook-safe WOC metadata. Compile the AnimBP or call Cache Model Metadata in Editor and save the Basis asset before cooking.");
		return false;
	}
	TSet<FName> Unique;
	for (const FName Name : Entry->JointNames)
	{
		if (Name.IsNone() || Unique.Contains(Name)) { Error = TEXT("WOC metadata contains an empty/duplicate joint name."); return false; }
		Unique.Add(Name);
	}
	for (const FWOCBasisLOD& LOD : LODs)
	{
		if (LOD.NumJoints != Entry->JointNames.Num() || LOD.NumCoeffs > Entry->NumCoeffs)
		{ Error = TEXT("WOC model metadata does not match the Basis joint/coefficient dimensions."); return false; }
	}
	Names = Entry->JointNames;
	Coefficients = Entry->NumCoeffs;
	return true;
}

bool UWOCBasisAsset::CacheModelMetadata(UNNEModelData* Model)
{
#if WITH_EDITOR
	auto* Existing = ModelMetadata.FindByPredicate([Model](const FWOCModelMetadata& Item) { return Item.ModelData == Model; });
	TArray<FName> Names;
	int32 Coefficients = 0;
	FString Error;
	if (Model && Existing && Existing->SourceFileId == Model->GetFileId()
		&& ResolveModelMetadata(Model, Names, Coefficients, Error)) return true;
	if (!FWOCDeformerInferenceCore::ExtractModelMetadata(Model, Names, Coefficients, Error))
	{ UE_LOG(LogWOCBasis, Error, TEXT("%s: %s"), *GetPathName(), *Error); return false; }
	FWOCModelMetadata Entry;
	Entry.ModelData = Model;
	Entry.SourceFileId = Model->GetFileId();
	Entry.JointNames = MoveTemp(Names);
	Entry.NumCoeffs = Coefficients;
	for (const FWOCBasisLOD& LOD : LODs)
		if (LOD.NumJoints != Entry.JointNames.Num() || LOD.NumCoeffs > Entry.NumCoeffs)
		{ UE_LOG(LogWOCBasis, Error, TEXT("%s: ONNX metadata disagrees with Basis dimensions."), *GetPathName()); return false; }
	Modify();
	if (Existing) *Existing = MoveTemp(Entry);
	else ModelMetadata.Add(MoveTemp(Entry));
	MarkPackageDirty();
	return true;
#else
	UE_LOG(LogWOCBasis, Error, TEXT("WOC metadata must be captured in Editor before cooking."));
	return false;
#endif
}

#if WITH_EDITOR
void UWOCBasisAsset::PreSave(FObjectPreSaveContext SaveContext)
{
	for (const FWOCModelMetadata& Entry : ModelMetadata)
		if (!CacheModelMetadata(Entry.ModelData))
			UE_LOG(LogWOCBasis, Error, TEXT("Cannot save valid WOC metadata for %s."), *GetPathName());
	Super::PreSave(SaveContext);
}
#endif

// ===========================================================================
// Binary Loading
// ===========================================================================

bool UWOCBasisAsset::LoadIntoLOD(FWOCBasisLOD& OutLOD, const FString& FilePath)
{
	TArray<uint8> Raw;
	if (!FFileHelper::LoadFileToArray(Raw, *FilePath))
	{
		UE_LOG(LogWOCBasis, Error, TEXT("WOCBasisAsset: Cannot open file '%s'."), *FilePath);
		return false;
	}
	if (Raw.Num() < 24)
	{
		UE_LOG(LogWOCBasis, Error, TEXT("WOCBasisAsset: File size too small."));
		return false;
	}

	// Use Memcpy to avoid alignment issues with reinterpret_cast on byte arrays
	int32 H[6];
	FMemory::Memcpy(H, Raw.GetData(), 24);
	OutLOD.NumVertices        = H[0];
	OutLOD.NumJoints          = H[1];
	OutLOD.NumCoeffs          = H[2];
	OutLOD.MaxJointsPerVertex = H[3];
	OutLOD.NNZ                = H[4];
	OutLOD.HashTableSize      = H[5];

	// Validate header values: reject negative or excessively large values
	if (OutLOD.NumVertices < 0 || OutLOD.NumVertices > 10000000 ||
	    OutLOD.NumJoints < 0 || OutLOD.NumJoints > 4096 ||
	    OutLOD.NumCoeffs < 0 || OutLOD.NumCoeffs > 1024 ||
	    OutLOD.MaxJointsPerVertex < 0 || OutLOD.MaxJointsPerVertex > 256 ||
	    OutLOD.NNZ < 0 || OutLOD.NNZ > 100000000 ||
	    OutLOD.HashTableSize < 0 || OutLOD.HashTableSize > 100000000)
	{
		UE_LOG(LogWOCBasis, Error,
		       TEXT("WOCBasisAsset: Invalid header values (negative or out of range). V=%d J=%d N=%d K=%d NNZ=%d HT=%d"),
		       OutLOD.NumVertices, OutLOD.NumJoints, OutLOD.NumCoeffs,
		       OutLOD.MaxJointsPerVertex, OutLOD.NNZ, OutLOD.HashTableSize);
		return false;
	}

	const int64 K          = (int64)OutLOD.MaxJointsPerVertex * OutLOD.NumCoeffs;
	const int64 TableCount = (int64)OutLOD.NumVertices * K;             // SparseIndexTable
	const int64 ValCount   = (int64)OutLOD.NNZ * 3;                    // PackedValues
	const int64 JTCount    = (int64)OutLOD.NumVertices * OutLOD.MaxJointsPerVertex; // JointTable
	const int64 HashCount  = (int64)OutLOD.HashTableSize;              // HashKeys + HashValues
	const int64 Expected   = 24 + TableCount * 4 + ValCount * 4 + JTCount * 4 + HashCount * 8;

	if (Raw.Num() < Expected)
	{
		UE_LOG(LogWOCBasis, Error,
		       TEXT("WOCBasisAsset: Size mismatch (expected %lld, actual %d). V=%d J=%d N=%d MAX_JOINT=%d NNZ=%d HT=%d"),
		       Expected, Raw.Num(),
		       OutLOD.NumVertices, OutLOD.NumJoints, OutLOD.NumCoeffs,
		       OutLOD.MaxJointsPerVertex, OutLOD.NNZ, OutLOD.HashTableSize);
		return false;
	}

	const uint8* Ptr = Raw.GetData() + 24;

	// SparseIndexTable [V * K]
	OutLOD.SparseIndexTable.SetNumUninitialized(TableCount);
	FMemory::Memcpy(OutLOD.SparseIndexTable.GetData(), Ptr, TableCount * 4);
	Ptr += TableCount * 4;

	// PackedValues [NNZ * 3]
	OutLOD.PackedValues.SetNumUninitialized(ValCount);
	FMemory::Memcpy(OutLOD.PackedValues.GetData(), Ptr, ValCount * 4);
	Ptr += ValCount * 4;

	// JointTable [V * MAX_JOINT]
	OutLOD.JointTableData.SetNumUninitialized(JTCount);
	FMemory::Memcpy(OutLOD.JointTableData.GetData(), Ptr, JTCount * 4);
	Ptr += JTCount * 4;

	// UV Hash Table (optional)
	if (OutLOD.HashTableSize > 0)
	{
		OutLOD.HashKeys.SetNumUninitialized(HashCount);
		FMemory::Memcpy(OutLOD.HashKeys.GetData(), Ptr, HashCount * 4);
		Ptr += HashCount * 4;

		OutLOD.HashValues.SetNumUninitialized(HashCount);
		FMemory::Memcpy(OutLOD.HashValues.GetData(), Ptr, HashCount * 4);
	}

	OutLOD.BinaryFilePath = FilePath;
	OutLOD.SparseIndexGPUBuffer.SafeRelease();
	OutLOD.PackedValuesGPUBuffer.SafeRelease();
	OutLOD.JointTableGPUBuffer.SafeRelease();
	OutLOD.HashKeysGPUBuffer.SafeRelease();
	OutLOD.HashValuesGPUBuffer.SafeRelease();
	OutLOD.bGPUBufferReady = false;

	UE_LOG(LogWOCBasis, Log,
	       TEXT("WOCBasisAsset: Loaded LOD V=%d J=%d N=%d MAX_JOINT=%d NNZ=%d HT=%d from '%s'"),
	       OutLOD.NumVertices, OutLOD.NumJoints, OutLOD.NumCoeffs,
	       OutLOD.MaxJointsPerVertex, OutLOD.NNZ, OutLOD.HashTableSize, *FilePath);

	return true;
}

bool UWOCBasisAsset::LoadLODFromBinaryFile(int32 LODIndex, const FString& FilePath)
{
	while (!LODs.IsValidIndex(LODIndex)) { LODs.AddDefaulted(); }
	const bool bOK = LoadIntoLOD(LODs[LODIndex], FilePath);
	if (bOK) { MarkPackageDirty(); }
	return bOK;
}

// ===========================================================================
// Editor Tools
// ===========================================================================

void UWOCBasisAsset::AddLODFromFile()
{
#if WITH_EDITOR
	IDesktopPlatform* DP = FDesktopPlatformModule::Get();
	if (!DP) { return; }

	void* ParentWindowHandle = nullptr;
	if (FSlateApplication::IsInitialized())
	{
		TSharedPtr<SWindow> MainWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
		if (MainWindow.IsValid() && MainWindow->GetNativeWindow().IsValid())
		{
			ParentWindowHandle = MainWindow->GetNativeWindow()->GetOSWindowHandle();
		}
	}

	const FString DefaultPath = LODs.Num() > 0 && !LODs.Last().BinaryFilePath.IsEmpty()
		? FPaths::GetPath(LODs.Last().BinaryFilePath)
		: FPaths::ProjectDir();

	TArray<FString> OutFiles;
	const bool bPicked = DP->OpenFileDialog(
		ParentWindowHandle,
		TEXT("Select WOC Basis binary (add as new LOD)"),
		DefaultPath,
		TEXT(""),
		TEXT("WOC Basis Binary (*.bin)|*.bin|All Files (*.*)|*.*"),
		EFileDialogFlags::Multiple,
		OutFiles);

	if (!bPicked) { return; }

	for (const FString& F : OutFiles)
	{
		FWOCBasisLOD NewLOD;
		NewLOD.BinaryFilePath = F;
		if (LoadIntoLOD(NewLOD, F))
		{
			LODs.Add(MoveTemp(NewLOD));
		}
	}
	MarkPackageDirty();
#else
	UE_LOG(LogWOCBasis, Warning, TEXT("AddLODFromFile is editor-only."));
#endif
}

// ===========================================================================
// PostLoad / GPU buffer init
// ===========================================================================

void UWOCBasisAsset::PostLoad()
{
	Super::PostLoad();
	EnsureAllGPUBuffers();
}

void UWOCBasisAsset::EnsureAllGPUBuffers()
{
	for (int32 i = 0; i < LODs.Num(); ++i) { EnsureGPUBuffers(i); }
}

void UWOCBasisAsset::EnsureGPUBuffers(int32 LODIndex)
{
	if (!LODs.IsValidIndex(LODIndex)) { return; }
	FWOCBasisLOD& LOD = LODs[LODIndex];
	if (LOD.bGPUBufferReady || LOD.SparseIndexTable.IsEmpty() || LOD.PackedValues.IsEmpty()) { return; }

	if (!IsInGameThread())
	{
		UE_LOG(LogWOCBasis, Error, TEXT("WOCBasisAsset: EnsureGPUBuffers must be called from the game thread."));
		return;
	}
	if (!GIsRHIInitialized)
	{
		UE_LOG(LogWOCBasis, Error, TEXT("WOCBasisAsset: RHI not initialized. Cannot create GPU buffers."));
		return;
	}

	TArray<int32> IndexCopy = LOD.SparseIndexTable;
	TArray<float> ValuesCopy = LOD.PackedValues;
	TArray<int32> TableCopy = LOD.JointTableData;
	TArray<uint32> HKeysCopy = LOD.HashKeys;
	TArray<int32> HValsCopy = LOD.HashValues;

	struct FGPUBasisResult
	{
		TRefCountPtr<FRDGPooledBuffer> IndexBuf;
		TRefCountPtr<FRDGPooledBuffer> ValuesBuf;
		TRefCountPtr<FRDGPooledBuffer> TableBuf;
		TRefCountPtr<FRDGPooledBuffer> HKeysBuf;
		TRefCountPtr<FRDGPooledBuffer> HValsBuf;
	};
	TSharedRef<FGPUBasisResult> Result = MakeShared<FGPUBasisResult>();

	ENQUEUE_RENDER_COMMAND(WOCBasis_Upload)
	(
		[IndexCopy = MoveTemp(IndexCopy), ValuesCopy = MoveTemp(ValuesCopy),
		 TableCopy = MoveTemp(TableCopy), HKeysCopy = MoveTemp(HKeysCopy),
		 HValsCopy = MoveTemp(HValsCopy), Result]
		(FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder Builder(RHICmdList);

			auto MakeSRVBuf = [&](auto& Data, const TCHAR* Name)
				-> TRefCountPtr<FRDGPooledBuffer>
			{
				if (Data.IsEmpty()) { return nullptr; }
				FRDGBufferDesc Desc = FRDGBufferDesc::CreateBufferDesc(
					sizeof(decltype(Data[0])), Data.Num());
				Desc.Usage = EBufferUsageFlags::ShaderResource;
				FRDGBufferRef Buf = Builder.CreateBuffer(Desc, Name);
				Builder.QueueBufferUpload(Buf, Data.GetData(),
				                         Data.Num() * sizeof(decltype(Data[0])),
				                         ERDGInitialDataFlags::None);
				return Builder.ConvertToExternalBuffer(Buf);
			};

			Result->IndexBuf  = MakeSRVBuf(IndexCopy,  TEXT("WOCSparseIndex"));
			Result->ValuesBuf = MakeSRVBuf(ValuesCopy, TEXT("WOCPackedValues"));
			Result->TableBuf  = MakeSRVBuf(TableCopy,  TEXT("WOCJointTable"));
			Result->HKeysBuf  = MakeSRVBuf(HKeysCopy,  TEXT("WOCHashKeys"));
			Result->HValsBuf  = MakeSRVBuf(HValsCopy,  TEXT("WOCHashValues"));

			Builder.Execute();
		}
	);

	FRenderCommandFence Fence;
	Fence.BeginFence();
	Fence.Wait();

	LOD.SparseIndexGPUBuffer  = MoveTemp(Result->IndexBuf);
	LOD.PackedValuesGPUBuffer = MoveTemp(Result->ValuesBuf);
	LOD.JointTableGPUBuffer   = MoveTemp(Result->TableBuf);
	LOD.HashKeysGPUBuffer     = MoveTemp(Result->HKeysBuf);
	LOD.HashValuesGPUBuffer   = MoveTemp(Result->HValsBuf);
	LOD.bGPUBufferReady       = true;

	UE_LOG(LogWOCBasis, Log, TEXT("WOCBasisAsset: GPU buffers ready (LOD%d)."), LODIndex);
}

#if WITH_EDITOR
void UWOCBasisAsset::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);

	// ActiveN changes do not require GPU buffer rebuild
	const FName PropName = Event.GetPropertyName();
	if (PropName == GET_MEMBER_NAME_CHECKED(FWOCBasisLOD, ActiveN))
	{
		return;
	}

	for (FWOCBasisLOD& LOD : LODs)
	{
		LOD.SparseIndexGPUBuffer.SafeRelease();
		LOD.PackedValuesGPUBuffer.SafeRelease();
		LOD.JointTableGPUBuffer.SafeRelease();
		LOD.HashKeysGPUBuffer.SafeRelease();
		LOD.HashValuesGPUBuffer.SafeRelease();
		LOD.bGPUBufferReady = false;
	}
}
#endif
