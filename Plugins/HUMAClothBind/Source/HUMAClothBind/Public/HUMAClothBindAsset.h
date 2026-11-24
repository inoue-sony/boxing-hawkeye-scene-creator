#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "HUMAClothSourceMapping.h"
#include "HUMAClothBindAsset.generated.h"

class USkeletalMesh;

UENUM(BlueprintType)
enum class EHUMAClothGender : uint8 { Female, Male };

UENUM(BlueprintType)
enum class EHUMAClothBindMethod : uint8
{
    BodyField UMETA(DisplayName="body_field"),
    ClothGraph UMETA(DisplayName="cloth_graph")
};

UCLASS(BlueprintType)
class HUMACLOTHBIND_API UHUMAClothBindAsset : public UDataAsset
{
    GENERATED_BODY()
public:
    // Retained for legacy source signatures only. New Bakes use shared canonical topology.
    UPROPERTY(BlueprintReadOnly, Category="Legacy")
    EHUMAClothGender Gender = EHUMAClothGender::Female;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Bake")
    bool bCanonicalBodyTopology = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Source")
    TObjectPtr<USkeletalMesh> BodyMesh;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Source", meta=(AllowedClasses="/Script/Engine.StaticMesh,/Script/Engine.SkeletalMesh"))
    TObjectPtr<UObject> GarmentMesh;
    UPROPERTY(EditAnywhere, Category="Source")
    FTransform GarmentToBody;
    UPROPERTY(EditAnywhere, Category="SoftBind", meta=(ClampMin="0.1", Units="cm"))
    float SigmaMin = 0.5f;
    UPROPERTY(EditAnywhere, Category="SoftBind", meta=(ClampMin="0.0"))
    float DistanceScale = 1.0f;
    UPROPERTY(EditAnywhere, Category="Bake")
    bool bBakeAllBodyLODs = true;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Bake", meta=(ToolTip="Select a method, then click Bake to rebuild the tables."))
    EHUMAClothBindMethod BakeMethod = EHUMAClothBindMethod::BodyField;
    UPROPERTY()
    bool bBakeMethodInitialized = false;
    UPROPERTY(VisibleAnywhere, Category="Bake")
    int32 BodyLODIndex = 0;
    UPROPERTY(VisibleAnywhere, Instanced, Category="Bake")
    TArray<TObjectPtr<UHUMAClothBindAsset>> AdditionalBodyLODs;
    UPROPERTY(VisibleAnywhere, Category="Bake")
    int32 FormatVersion = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Bake")
    FString BindMethod = TEXT("cloth_graph");
    UPROPERTY(VisibleAnywhere, Category="Bake")
    FString BakeSignature;
    UPROPERTY()
    FString SourceFingerprint;
    UPROPERTY(VisibleAnywhere, Category="Bake")
    FString BakeStatus = TEXT("Not baked");
    UPROPERTY(VisibleAnywhere, Category="Bake", meta=(Units="cm"))
    float MaxRestErrorCm = 0;
    UPROPERTY(VisibleAnywhere, Category="Bake", meta=(Units="cm"))
    float MaxRigidErrorCm = 0;
    UPROPERTY()
    TArray<FVector> RestBody;
    UPROPERTY()
    TArray<int32> CanonicalToRender;
    UPROPERTY()
    TArray<int32> BodyQuads;
    UPROPERTY()
    TArray<FIntVector> AnchorTriangles;
    UPROPERTY()
    TArray<FVector> AnchorBarycentrics;
    UPROPERTY()
    TArray<FVector> AnchorRestPositions;
    UPROPERTY()
    TArray<FVector> AnchorRestTangents;
    UPROPERTY()
    TArray<FVector> AnchorRestBitangents;
    UPROPERTY()
    TArray<FVector> AnchorRestNormals;
    UPROPERTY()
    TArray<FVector> ClothRestPositions;
    UPROPERTY()
    TArray<int32> InfluenceAnchors;
    UPROPERTY()
    TArray<float> InfluenceWeights;
    UPROPERTY()
    int32 Slots = 24;
    UPROPERTY()
    TArray<int32> ClothTriangles;
    UPROPERTY()
    TArray<FVector2D> ClothUVs;
    UPROPERTY()
    TArray<int32> SectionIndexCounts;
    UPROPERTY()
    TArray<int32> SectionMaterials;

    virtual void Serialize(FArchive& Ar) override;
    virtual void PostLoad() override;
#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif
    FString GetRequestedBindMethod() const;
    void NormalizeBindMethod();
    bool Validate(FString& Error) const;
    bool Evaluate(TConstArrayView<FVector3f> Body, TArray<FVector>& Cloth, FString& Error) const;
    UFUNCTION(CallInEditor, BlueprintCallable, Category="Bake")
    void Bake();
    UFUNCTION(BlueprintCallable, Category="HUMA Cloth Bind")
    bool ExportSnapshot(const FString& Path);
    UFUNCTION(BlueprintCallable, Category="HUMA Cloth Bind|Validation")
    bool ExportBakeData(const FString& Path) const;
    UFUNCTION(BlueprintCallable, Category="HUMA Cloth Bind")
    bool ImportBake(const FString& Path);
    UFUNCTION(CallInEditor, BlueprintCallable, Category="Bake")
    bool RunReferenceChecks();
    bool IsBakeCurrent(FString& Error) const;
    UFUNCTION(BlueprintCallable, Category="HUMA Cloth Bind", meta=(ToolTip="Returns an empty string for a compatible mesh, or the validation error."))
    FString GetBodyMeshCompatibilityError(USkeletalMesh* Mesh) const;
    bool BuildSourceMapping(USkeletalMesh* Mesh, TArray<int32>& Mapping, FString& Error) const;
    bool BuildSourceLODs(USkeletalMesh* Mesh, FHUMAClothSourceLODs& LODs, FString& Error) const;
    const UHUMAClothBindAsset* GetBodyLODBind(int32 LODIndex) const;
private:
    mutable FString CachedSourceFingerprint;
    mutable FString CachedBakeSignature;
};