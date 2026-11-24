#pragma once

#include "CoreMinimal.h"
#include "HUMAIdentity.h"
#include "HUMARuntimeData.h"
#include "HUMARuntimePlayback.generated.h"

struct FHUMARuntimePose
{
	TArray<FQuat> MeshLocalRotations;
	FVector HipDisplacement = FVector::ZeroVector;
};

UCLASS(ClassGroup = HUMA, meta = (BlueprintSpawnableComponent))
class HUMADYNAMICIDENTITYCHANGER_API UHUMARuntimePoseComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UHUMARuntimePoseComponent();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA|Runtime")
	TObjectPtr<USkeletalMeshComponent> TargetMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA|Runtime")
	TObjectPtr<UHUMAIdentityComponent> Identity;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA|Runtime")
	int32 TrackId = INDEX_NONE;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA|Runtime")
	FString PlayerId;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA|Runtime")
	int32 TeamId = INDEX_NONE;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA|Runtime")
	int32 JerseyNumber = INDEX_NONE;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA|Runtime")
	int32 RoleId = INDEX_NONE;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA|Runtime")
	double SampleTime = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA|Runtime")
	bool bPlaybackInitialized = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA|Runtime")
	FString LastError;

	void PreparePlayback(UHUMAIdentityData* Data, const FHUMARuntimeTrack& Track);
	TSharedPtr<const FHUMARuntimePose, ESPMode::ThreadSafe> GetPose() const { return Pose; }
	void SetPose(TSharedRef<const FHUMARuntimePose, ESPMode::ThreadSafe> InPose) { Pose = InPose; }
	void SetTrackVisible(bool bVisible);
	virtual void InitializeComponent() override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UHUMAIdentityData> InitialData;
	TArray<float> InitialBetas;
	TSharedPtr<const FHUMARuntimePose, ESPMode::ThreadSafe> Pose;
	bool bTrackHidden = false;
	bool bOwnerWasHidden = false;
	TMap<TWeakObjectPtr<AActor>, bool> HiddenAttachments;
	bool Fail(const FString& Message);
};

UCLASS(Blueprintable)
class HUMADYNAMICIDENTITYCHANGER_API AHUMARuntimeCharacter : public AHUMAIdentityCharacter
{
	GENERATED_BODY()
public:
	AHUMARuntimeCharacter();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HUMA|Runtime")
	TObjectPtr<UHUMARuntimePoseComponent> RuntimePose;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HUMA|Runtime")
	int32 TrackId = INDEX_NONE;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HUMA|Runtime")
	FString PlayerId;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HUMA|Runtime")
	int32 TeamId = INDEX_NONE;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HUMA|Runtime")
	int32 JerseyNumber = INDEX_NONE;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FHUMARuntimePlayerSpawned, AActor*, Player, UHUMARuntimePoseComponent*, RuntimePose);

UCLASS(ClassGroup = HUMA, meta = (BlueprintSpawnableComponent))
class HUMADYNAMICIDENTITYCHANGER_API UHUMARuntimePlaybackComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UHUMARuntimePlaybackComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Runtime")
	FString ManifestPath = TEXT("retarget_runtime/manifest.json");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Runtime")
	FString ExpectedModelId = TEXT("HUMA_FULLBODY_v1.0.0_MALE_LOD0");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Runtime")
	TObjectPtr<UHUMAIdentityData> IdentityData;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Runtime")
	TSubclassOf<AActor> CharacterClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Runtime")
	TArray<int32> ExcludedRoleIds;

	UPROPERTY(BlueprintAssignable, Category = "HUMA|Runtime")
	FHUMARuntimePlayerSpawned OnPlayerSpawned;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA|Runtime")
	double LastLoadMilliseconds = 0.0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA|Runtime")
	double LastLoadStartedSeconds = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Runtime")
	bool bLoop = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA|Runtime")
	bool bReady = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA|Runtime")
	bool bPlaying = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA|Runtime")
	double CurrentTime = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA|Runtime")
	double Duration = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA|Runtime")
	FString LastError;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA|Runtime")
	TArray<TObjectPtr<AActor>> Players;

	UFUNCTION(BlueprintCallable, Category = "HUMA|Runtime")
	bool LoadDataset();

	UFUNCTION(BlueprintCallable, Category = "HUMA|Runtime")
	bool LoadAndPlay();

	UFUNCTION(BlueprintCallable, Category = "HUMA|Runtime")
	bool Play();

	UFUNCTION(BlueprintCallable, Category = "HUMA|Runtime")
	void Pause();

	UFUNCTION(BlueprintCallable, Category = "HUMA|Runtime")
	bool Seek(double TimeSeconds);

	UFUNCTION(BlueprintCallable, Category = "HUMA|Runtime")
	bool Restart();

	UFUNCTION(BlueprintPure, Category = "HUMA|Runtime")
	FString GetPlaybackSummary() const;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	TSharedPtr<const FHUMARuntimeDataset> Dataset;
	TArray<int32> ActiveTrackIndices;
	bool Fail(const FString& Message);
	void ClearPlayers();
	void ApplyTime();
	TSharedPtr<const FHUMARuntimePose, ESPMode::ThreadSafe> SamplePose(const FHUMARuntimeTrack& Track, double Time) const;
};

UCLASS(Blueprintable)
class HUMADYNAMICIDENTITYCHANGER_API UHUMARuntimePlaybackWidget : public UUserWidget
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintReadWrite, Transient, Category = "HUMA|Runtime")
	TObjectPtr<UHUMARuntimePlaybackComponent> Playback;

	UFUNCTION(BlueprintCallable, Category = "HUMA|Runtime")
	void TogglePlayback();

	UFUNCTION(BlueprintCallable, Category = "HUMA|Runtime")
	void RestartPlayback();

	UFUNCTION(BlueprintCallable, Category = "HUMA|Runtime")
	void BeginScrub();

	UFUNCTION(BlueprintCallable, Category = "HUMA|Runtime")
	void EndScrub();

	UFUNCTION(BlueprintCallable, Category = "HUMA|Runtime")
	void SeekNormalized(float Value);

	UFUNCTION(BlueprintPure, Category = "HUMA|Runtime")
	float GetNormalizedTime() const;

	UFUNCTION(BlueprintPure, Category = "HUMA|Runtime")
	FText GetTimeLabel() const;

	UFUNCTION(BlueprintPure, Category = "HUMA|Runtime")
	FText GetPlayButtonLabel() const;

	UFUNCTION(BlueprintPure, Category = "HUMA|Runtime")
	FText GetStatusLabel() const;

	UFUNCTION(BlueprintPure, Category = "HUMA|Runtime")
	bool CanControlPlayback() const;

private:
	bool bResumeAfterScrub = false;
};

// This host supplies components/properties only. The project's Blueprint owns startup.
UCLASS(Blueprintable)
class HUMADYNAMICIDENTITYCHANGER_API AHUMARuntimePlaybackHost : public AActor
{
	GENERATED_BODY()
public:
	AHUMARuntimePlaybackHost();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HUMA|Runtime")
	TObjectPtr<UHUMARuntimePlaybackComponent> Playback;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Runtime")
	TSubclassOf<UHUMARuntimePlaybackWidget> ControlsClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Runtime")
	TObjectPtr<AActor> PlaybackCamera;

	UFUNCTION(BlueprintCallable, Category = "HUMA|Runtime")
	bool ShowPlaybackControls();

private:
	UPROPERTY(Transient)
	TObjectPtr<UHUMARuntimePlaybackWidget> Controls;
};
