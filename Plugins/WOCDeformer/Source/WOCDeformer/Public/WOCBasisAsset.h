#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "RenderGraphResources.h"

#include "WOCBasisAsset.generated.h"

class UNNEModelData;

USTRUCT()
struct WOCDEFORMER_API FWOCModelMetadata
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "WOC Model Metadata")
	TObjectPtr<UNNEModelData> ModelData;
	UPROPERTY()
	FGuid SourceFileId;
	UPROPERTY(VisibleAnywhere, Category = "WOC Model Metadata")
	int32 Version = 1;
	UPROPERTY(VisibleAnywhere, Category = "WOC Model Metadata")
	TArray<FName> JointNames;
	UPROPERTY(VisibleAnywhere, Category = "WOC Model Metadata")
	int32 NumCoeffs = 0;
};

// ----------------------------------------------------------------
// FWOCBasisLOD — Data for a single LOD level
// ----------------------------------------------------------------

USTRUCT(BlueprintType)
struct WOCDEFORMER_API FWOCBasisLOD
{
	GENERATED_BODY()

	// Binary file path (used internally as default directory for file dialogs)
	UPROPERTY()
	FString BinaryFilePath;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "WOC Basis LOD")
	int32 NumVertices        = 0;  // V

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "WOC Basis LOD")
	int32 NumJoints          = 0;  // J

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "WOC Basis LOD")
	int32 NumCoeffs          = 0;  // N

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "WOC Basis LOD")
	int32 MaxJointsPerVertex = 0;  // MAX_JOINT

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "WOC Basis LOD")
	int32 NNZ                = 0;  // Number of non-zero sparse entries

	/** Number of WOC bases to use for this LOD. -1 = auto-use max from binary (default).
	    0 = no deformation, positive = use that many bases (clamped to max in binary). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WOC Basis LOD",
	          meta = (ClampMin = -1, UIMin = 0))
	int32 ActiveN = -1;

	// Sparse data (v2 format)
	UPROPERTY()
	TArray<int32> SparseIndexTable;  // [V * MAX_JOINT * N]  (-1=no value, else=index into PackedValues)

	UPROPERTY()
	TArray<float> PackedValues;      // [NNZ * 3]  (xyz interleaved)

	UPROPERTY()
	TArray<int32> JointTableData;    // [V, MAX_JOINT]

	// UV Hash Table (v3 format) — maps quantized UV key → original vertex ID
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "WOC Basis LOD")
	int32 HashTableSize = 0;

	UPROPERTY()
	TArray<uint32> HashKeys;         // [HashTableSize]  (0xFFFFFFFF = empty)

	UPROPERTY()
	TArray<int32> HashValues;        // [HashTableSize]  (original vertex ID)

	// Persistent GPU buffers
	TRefCountPtr<FRDGPooledBuffer> SparseIndexGPUBuffer;
	TRefCountPtr<FRDGPooledBuffer> PackedValuesGPUBuffer;
	TRefCountPtr<FRDGPooledBuffer> JointTableGPUBuffer;
	TRefCountPtr<FRDGPooledBuffer> HashKeysGPUBuffer;
	TRefCountPtr<FRDGPooledBuffer> HashValuesGPUBuffer;
	bool bGPUBufferReady = false;
};

// ----------------------------------------------------------------
// UWOCBasisAsset — Contains an array of LODs
// ----------------------------------------------------------------

UCLASS(BlueprintType)
class WOCDEFORMER_API UWOCBasisAsset : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WOC Basis")
	TArray<FWOCBasisLOD> LODs;

	UPROPERTY(VisibleAnywhere, Category = "WOC Basis|Model Metadata")
	TArray<FWOCModelMetadata> ModelMetadata;

	UFUNCTION(BlueprintCallable, Category = "WOC Basis|Model Metadata")
	bool CacheModelMetadata(UNNEModelData* Model);

	bool ResolveModelMetadata(UNNEModelData* Model, TArray<FName>& Names, int32& Coefficients, FString& Error) const;

	int32 GetNumLODs() const { return LODs.Num(); }
	FWOCBasisLOD*       GetLOD(int32 LODIndex)       { return LODs.IsValidIndex(LODIndex) ? &LODs[LODIndex] : nullptr; }
	const FWOCBasisLOD* GetLOD(int32 LODIndex) const { return LODs.IsValidIndex(LODIndex) ? &LODs[LODIndex] : nullptr; }

	/** Returns the effective number of bases to use for a given LOD. ActiveN=-1 uses the LOD's max N. */
	int32 GetEffectiveN(const FWOCBasisLOD& LOD) const
	{
		return (LOD.ActiveN < 0) ? LOD.NumCoeffs : FMath::Clamp(LOD.ActiveN, 0, LOD.NumCoeffs);
	}

	void EnsureGPUBuffers(int32 LODIndex);
	void EnsureAllGPUBuffers();

	// Opens a file dialog and adds as a new LOD (the button in Details panel is managed by WOCBasisAssetDetails)
	UFUNCTION(BlueprintCallable, Category = "WOC Basis")
	void AddLODFromFile();

	// Direct load (for Blueprint/C++)
	UFUNCTION(BlueprintCallable, Category = "WOC Basis")
	bool LoadLODFromBinaryFile(int32 LODIndex, const FString& FilePath);

	virtual void PostLoad() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
	virtual void PreSave(FObjectPreSaveContext SaveContext) override;
#endif

private:
	// Internal binary loading implementation
	bool LoadIntoLOD(FWOCBasisLOD& OutLOD, const FString& FilePath);
};
