#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/ActorComponent.h"
#include "Engine/DataAsset.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/HUD.h"
#include "HUMAIdentity.generated.h"

class UAnimSequence;
class USkeletalMesh;
class USkeletalMeshComponent;

HUMADYNAMICIDENTITYCHANGER_API DECLARE_LOG_CATEGORY_EXTERN(LogHUMAIdentity, Log, All);

UCLASS(BlueprintType)
class HUMADYNAMICIDENTITYCHANGER_API UHUMAIdentityData : public UDataAsset
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUMA")
	TObjectPtr<USkeletalMesh> ReferenceMesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUMA")
	TArray<FName> BetaNames;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUMA")
	TArray<FName> JointNames;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUMA")
	TArray<FVector> JointTemplates;

	// Joint-major, beta-minor, converted into mesh component coordinates.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUMA")
	TArray<FVector> JointCoefficients;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUMA")
	FName HipBone = TEXT("hip");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUMA")
	FString SourceDescription;

	bool Validate(USkeletalMesh* Mesh, FString& Error) const;
};

struct FHUMAIdentityPose
{
	TArray<FTransform> ReferencePose;
	TArray<FTransform> ShapedPose;
	TArray<FName> BetaNames;
	TArray<float> Betas;
	int32 HipIndex = INDEX_NONE;
};

UCLASS(ClassGroup = HUMA, meta = (BlueprintSpawnableComponent))
class HUMADYNAMICIDENTITYCHANGER_API UHUMAIdentityComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UHUMAIdentityComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA")
	TObjectPtr<USkeletalMeshComponent> TargetMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA")
	TObjectPtr<UHUMAIdentityData> IdentityData;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUMA")
	TArray<float> Betas;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA")
	FString LastError;

	UFUNCTION(BlueprintCallable, Category = "HUMA")
	bool SetBeta(int32 Index, float Value);

	UFUNCTION(BlueprintCallable, Category = "HUMA")
	bool SetBetas(const TArray<float>& Values);

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "HUMA")
	void ResetIdentity();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "HUMA")
	bool ApplyIdentity();

	UFUNCTION(BlueprintPure, Category = "HUMA")
	TArray<FVector> GetAppliedBonePositions() const;

	UFUNCTION(BlueprintCallable, Category = "HUMA")
	TArray<FTransform> GetEvaluatedLocalPose();

	UFUNCTION(BlueprintPure, Category = "HUMA")
	static TArray<FVector> GetReferenceBonePositions(USkeletalMesh* Mesh);

	UFUNCTION(BlueprintPure, Category = "HUMA")
	static TArray<FName> GetReferenceBoneNames(USkeletalMesh* Mesh);

	TSharedPtr<const FHUMAIdentityPose, ESPMode::ThreadSafe> GetIdentityPose() const { return Pose; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction) override;
	virtual void PostLoad() override;
	virtual void OnRegister() override;
	virtual void InitializeComponent() override;
	virtual void OnUnregister() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif

private:
	bool bDirty = true;
	TWeakObjectPtr<USkeletalMeshComponent> AppliedMesh;
	TSharedPtr<const FHUMAIdentityPose, ESPMode::ThreadSafe> Pose;
	bool Fail(const FString& Message);
	void InitializeBetaArrayIfEmpty();
	USkeletalMeshComponent* FindMatchingMesh() const;
	void ReleaseOverride();
};

UCLASS(Blueprintable)
class HUMADYNAMICIDENTITYCHANGER_API AHUMAIdentityCharacter : public AActor
{
	GENERATED_BODY()
public:
	AHUMAIdentityCharacter();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HUMA")
	TObjectPtr<USkeletalMeshComponent> CharacterMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HUMA")
	TObjectPtr<UHUMAIdentityComponent> Identity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA")
	TObjectPtr<UAnimSequence> Motion;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA")
	bool bUseRuntimeMotion = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA")
	FString DisplayName = TEXT("HUMA Character");

	virtual void BeginPlay() override;
};

UCLASS()
class HUMADYNAMICIDENTITYCHANGER_API UHUMAIdentityPanel : public UUserWidget
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA", meta = (ExposeOnSpawn = true))
	TObjectPtr<UHUMAIdentityComponent> TargetIdentity;

	UFUNCTION(BlueprintPure, Category = "HUMA")
	TArray<UHUMAIdentityComponent*> GetAvailableIdentities() const;

	UFUNCTION(BlueprintCallable, Category = "HUMA")
	bool SetSelectedBeta(int32 Index, float Value);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
};

UCLASS()
class HUMADYNAMICIDENTITYCHANGER_API AHUMAIdentityHUD : public AHUD
{
	GENERATED_BODY()
protected:
	virtual void BeginPlay() override;
	UPROPERTY(Transient)
	TObjectPtr<UHUMAIdentityPanel> Panel;
};

UCLASS()
class HUMADYNAMICIDENTITYCHANGER_API AHUMAIdentityGameMode : public AGameModeBase
{
	GENERATED_BODY()
public:
	AHUMAIdentityGameMode();
};
