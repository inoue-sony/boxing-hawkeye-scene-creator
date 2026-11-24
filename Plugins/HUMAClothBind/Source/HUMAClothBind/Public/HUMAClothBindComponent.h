#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "HUMAClothBindComponent.generated.h"

class UHUMAClothBindAsset;
class UProceduralMeshComponent;
class USkeletalMeshComponent;
class USkeletalMesh;
class UMeshDeformer;
class UHUMAClothGpuMeshComponent;
struct FHUMAClothReadback;
class UMaterialInterface;

USTRUCT(BlueprintType)
struct HUMACLOTHBIND_API FHUMAClothSpawnFailure
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="HUMA Cloth Bind", meta=(ToolTip="Zero-based input array index. -1 indicates an empty input array."))
    int32 InputIndex = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly, Category="HUMA Cloth Bind")
    TObjectPtr<UHUMAClothBindAsset> Asset;
    UPROPERTY(BlueprintReadOnly, Category="HUMA Cloth Bind")
    FString Error;
};

UCLASS(ClassGroup=(HUMA), meta=(BlueprintSpawnableComponent))
class HUMACLOTHBIND_API UHUMAClothBindComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UHUMAClothBindComponent();
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUMA Cloth Bind")
    TObjectPtr<UHUMAClothBindAsset> BindAsset;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUMA Cloth Bind")
    TObjectPtr<AActor> SourceBodyActor;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUMA Cloth Bind", meta=(ToolTip="Optional explicit source component. Takes precedence over Source Body Actor."))
    TObjectPtr<USkeletalMeshComponent> SourceBodyComponent;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUMA Cloth Bind")
    bool bPreviewEnabled = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUMA Cloth Bind")
    bool bUseGPU = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUMA Cloth Bind|Lifetime")
    bool bDestroyOwnerWithSource = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="HUMA Cloth Bind|Source", meta=(ToolTip="When the body has no active deformer, temporarily use UE's standard skinning/Morph/Cloth graph. Existing deformers are never replaced."))
    bool bEnableStandardSkinningAdapter = true;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="HUMA Cloth Bind|Source")
    TSoftObjectPtr<UMeshDeformer> StandardSkinningDeformer;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="HUMA Cloth Bind|Source")
    bool bUsingStandardSkinningAdapter = false;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="HUMA Cloth Bind")
    FString PreviewStatus;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="HUMA Cloth Bind")
    int64 CapturedFrames = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="HUMA Cloth Bind")
    int32 ActiveBodyLOD = INDEX_NONE;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="HUMA Cloth Bind")
    int32 ActiveBindBodyLOD = INDEX_NONE;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="HUMA Cloth Bind", meta=(Units="cm"))
    float SourceLODProjectionMaxErrorCm = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="HUMA Cloth Bind")
    bool bUsingCompatibleBodyMesh = false;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="HUMA Cloth Bind")
    int64 LODTransitions = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, DuplicateTransient, Category="HUMA Cloth Bind|Sharing")
    int64 BodyNormalPasses = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, DuplicateTransient, Category="HUMA Cloth Bind|Sharing")
    int64 BodyNormalReuses = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, DuplicateTransient, Category="HUMA Cloth Bind|Sharing")
    int32 BodyNormalGroup = INDEX_NONE;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, DuplicateTransient, Category="HUMA Cloth Bind|Sharing")
    int64 BodyNormalEvaluation = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="HUMA Cloth Bind")
    float EvaluationMilliseconds = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, DuplicateTransient, Category="HUMA Cloth Bind")
    int32 InitializationCount = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, DuplicateTransient, Category="HUMA Cloth Bind", meta=(Units="ms"))
    float InitializationMilliseconds = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="HUMA Cloth Bind", meta=(Units="cm"))
    float LastFrameMotionCm = 0;
    UFUNCTION(CallInEditor, BlueprintCallable, Category="HUMA Cloth Bind")
    void RebuildPreview();
    UFUNCTION(CallInEditor, BlueprintCallable, Category="HUMA Cloth Bind")
    void ClearPreview();
    UFUNCTION(BlueprintCallable, Category="HUMA Cloth Bind")
    bool InitializeBinding(UHUMAClothBindAsset* Asset, USkeletalMeshComponent* Source);
    UFUNCTION(BlueprintPure, Category="HUMA Cloth Bind")
    bool IsBindingInitialized() const;
    int32 ApplyMaterialSlots(const TMap<FName, TObjectPtr<UMaterialInterface>>& Materials);
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="HUMA Cloth Bind|Appearance")
    int32 AppliedMaterialSlots = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category="HUMA Cloth Bind|Appearance")
    TMap<FName, TObjectPtr<UMaterialInterface>> MaterialOverrides;
    UFUNCTION(BlueprintCallable, Category="HUMA Cloth Bind|Validation")
    bool RequestGPUSnapshot(const FString& Path);
    UFUNCTION(BlueprintCallable, Category="HUMA Cloth Bind|Validation", meta=(DevelopmentOnly))
    bool SetSourcePoseForValidation(float Position, int32 BodyLOD);
    virtual void OnRegister() override;
    virtual void OnUnregister() override;
    virtual void BeginPlay() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction) override;
    USkeletalMeshComponent* GetSourceBodyComponent() const { return BodyComponent; }
#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif
private:
    TWeakObjectPtr<AActor> LifetimeSource;
    UFUNCTION()
    void OnSourceEndPlay(AActor* Source, EEndPlayReason::Type Reason);
    UFUNCTION()
    void OnSourceDestroyed(AActor* Source);
    USkeletalMeshComponent* ResolveSourceBody() const;
    UPROPERTY(Transient)
    TObjectPtr<UProceduralMeshComponent> PreviewMesh;
    UPROPERTY(Transient)
    TObjectPtr<UHUMAClothGpuMeshComponent> GPUMesh;
    UPROPERTY(Transient)
    TObjectPtr<USkeletalMeshComponent> BodyComponent;
    TWeakObjectPtr<UHUMAClothBindAsset> InitializedBindAsset;
    TWeakObjectPtr<USkeletalMesh> InitializedBodyMesh;
    TArray<int32> SourceMapping;
    TWeakObjectPtr<USkeletalMeshComponent> AdaptedBody;
    TWeakObjectPtr<USkeletalMeshComponent> LOD0Body;
    TSharedPtr<FHUMAClothReadback, ESPMode::ThreadSafe> State;
    TArray<FVector> LastClothPositions;
    uint64 AppliedRevision = 0;
};

UCLASS()
class HUMACLOTHBIND_API AHUMAClothPreviewActor : public AActor
{
    GENERATED_BODY()
public:
    AHUMAClothPreviewActor();
    UFUNCTION(BlueprintCallable, Category="HUMA Cloth Bind", meta=(DisplayName="Spawn Bound Garment"))
    static AHUMAClothPreviewActor* SpawnBoundGarment(UHUMAClothBindAsset* Asset, USkeletalMeshComponent* Source);
    UFUNCTION(BlueprintCallable, Category="HUMA Cloth Bind", meta=(DisplayName="Spawn Bound Garments", ToolTip="Spawn one garment per input entry. Returns successful actors in input order; failures include their input index and reason. Successful actors remain if another entry fails."))
    static TArray<AHUMAClothPreviewActor*> SpawnBoundGarments(const TArray<UHUMAClothBindAsset*>& Assets,
        USkeletalMeshComponent* Source, TArray<FHUMAClothSpawnFailure>& Failures);
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="HUMA Cloth Bind")
    TObjectPtr<UHUMAClothBindComponent> ClothBind;
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
private:
    static AHUMAClothPreviewActor* SpawnBoundGarmentInternal(UHUMAClothBindAsset* Asset,
        USkeletalMeshComponent* Source, FString& Error);
};