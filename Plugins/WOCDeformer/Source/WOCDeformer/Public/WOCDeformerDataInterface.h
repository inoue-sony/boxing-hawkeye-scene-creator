#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"
#include "OptimusComputeDataInterface.h"
#include "ComputeFramework/ComputeDataProvider.h"
#include "ComputeFramework/ShaderParamTypeDefinition.h"
#include "RenderGraphFwd.h"
#include "RenderGraphResources.h"
#include "ShaderParameterMetadataBuilder.h"

#include "WOCDeformerDataInterface.generated.h"

class UWOCBasisAsset;
class UAnimInstance;

UCLASS(Category = "WOC Deformer")
class WOCDEFORMER_API UWOCDeformerDataInterface : public UOptimusComputeDataInterface
{
	GENERATED_BODY()

public:
	virtual FString GetDisplayName() const override { return TEXT("WOC Deformer Offsets"); }
	TCHAR const* GetClassName() const override { return TEXT("WOCDeformerData"); }
	virtual bool CanSupportUnifiedDispatch() const override { return true; }

	virtual TArray<FOptimusCDIPinDefinition> GetPinDefinitions() const override
	{
		TArray<FOptimusCDIPinDefinition> Defs;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 4)
		Defs.Add({"WOCOffset", "ReadWOCOffset", "Vertex", "ReadNumVertices", false});
#else
		Defs.Add({"WOCOffset", "ReadWOCOffset", "Vertex", "ReadNumVertices"});
#endif
		return Defs;
	}

	virtual TSubclassOf<UActorComponent> GetRequiredComponentClass() const override;
	virtual void GetSupportedInputs(TArray<FShaderFunctionDefinition>& OutFunctions) const override;
	virtual void GetShaderParameters(TCHAR const* UID,
		FShaderParametersMetadataBuilder& InOutBuilder,
		FShaderParametersMetadataAllocations& InOutAllocations) const override;
	virtual void GetHLSL(FString& OutHLSL, FString const& InDataInterfaceName) const override;
	virtual UComputeDataProvider* CreateDataProvider(
		TObjectPtr<UObject> InBinding, uint64 InInputMask, uint64 InOutputMask) const override;
};


UCLASS()
class WOCDEFORMER_API UWOCDeformerDataProvider : public UComputeDataProvider
{
	GENERATED_BODY()

public:
	/** Anchor for dynamically searching Component/AnimNode each frame */
	UPROPERTY()
	TWeakObjectPtr<USkeletalMeshComponent> BoundSkelMesh;

	/** Vertex count from the last valid LOD. Used to dispatch kernel with zero offsets when WOC data is unresolved */
	uint32 CachedNumVertices = 0;

	virtual FComputeDataProviderRenderProxy* GetRenderProxy() override;
};


class FWOCDeformerDataProviderProxy : public FComputeDataProviderRenderProxy
{
public:
	FWOCDeformerDataProviderProxy(
		TRefCountPtr<FRDGPooledBuffer> InCoeffBuffer,        // [J*N]
		TRefCountPtr<FRDGPooledBuffer> InRglobalBuffer,      // [J*9]
		TRefCountPtr<FRDGPooledBuffer> InSparseIndexBuffer,  // [V*K] int32
		TRefCountPtr<FRDGPooledBuffer> InPackedValuesBuffer, // [NNZ*3] float
		TRefCountPtr<FRDGPooledBuffer> InJointTableBuffer,   // [V*MAX_JOINT]
		TRefCountPtr<FRDGPooledBuffer> InHashKeysBuffer,     // [HashTableSize] uint32
		TRefCountPtr<FRDGPooledBuffer> InHashValuesBuffer,   // [HashTableSize] int32
		uint32 InNumVertices, uint32 InNumJoints,
		uint32 InJN, uint32 InMaxJointsPerVertex,
		uint32 InN, uint32 InNActive, float InOffsetScale,
		uint32 InHashTableSize,
		bool bInValid = true);

	virtual int32 GetDispatchThreadCount(TArray<FIntVector>& ThreadCounts) const override;
	virtual bool  IsValid(FValidationData const& InValidationData) const override;
	virtual void  AllocateResources(FRDGBuilder& GraphBuilder,
	                                FAllocationData const& InAllocationData) override;
	virtual void  GatherDispatchData(FDispatchData const& InDispatchData) override;

private:
	TRefCountPtr<FRDGPooledBuffer> CoeffBuffer;
	TRefCountPtr<FRDGPooledBuffer> RglobalBuffer;
	TRefCountPtr<FRDGPooledBuffer> SparseIndexBuffer;
	TRefCountPtr<FRDGPooledBuffer> PackedValuesBuffer;
	TRefCountPtr<FRDGPooledBuffer> JointTableBuffer;
	TRefCountPtr<FRDGPooledBuffer> HashKeysBuffer;
	TRefCountPtr<FRDGPooledBuffer> HashValuesBuffer;
	uint32 NumVertices        = 0;
	uint32 NumJoints          = 0;
	uint32 JN                 = 0;
	uint32 MaxJointsPerVertex = 0;
	uint32 N                  = 0;
	uint32 NActive            = 0;
	float OffsetScale        = 1.0f;
	uint32 HashTableSize      = 0;

	bool bValid = true;

	FRDGBufferSRVRef CoeffSRV        = nullptr;
	FRDGBufferSRVRef RglobalSRV      = nullptr;
	FRDGBufferSRVRef SparseIndexSRV  = nullptr;
	FRDGBufferSRVRef PackedValuesSRV = nullptr;
	FRDGBufferSRVRef JointTableSRV   = nullptr;
	FRDGBufferSRVRef HashKeysSRV     = nullptr;
	FRDGBufferSRVRef HashValuesSRV   = nullptr;
};
