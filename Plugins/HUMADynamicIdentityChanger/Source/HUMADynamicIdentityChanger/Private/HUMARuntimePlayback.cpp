#include "HUMARuntimePlayback.h"

#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

UHUMARuntimePoseComponent::UHUMARuntimePoseComponent()
{
	bWantsInitializeComponent = true;
}

bool UHUMARuntimePoseComponent::Fail(const FString& Message)
{
	LastError = Message;
	UE_LOG(LogHUMAIdentity, Error, TEXT("%s: %s"), *GetPathName(), *Message);
	return false;
}

void UHUMARuntimePoseComponent::PreparePlayback(UHUMAIdentityData* Data, const FHUMARuntimeTrack& Track)
{
	InitialData = Data;
	InitialBetas = Track.Betas;
	TrackId = Track.TrackId;
	PlayerId = Track.PlayerId;
	TeamId = Track.TeamId;
	JerseyNumber = Track.JerseyNumber;
	RoleId = Track.RoleId;
}

void UHUMARuntimePoseComponent::InitializeComponent()
{
	Super::InitializeComponent();
	if (!InitialData) { return; }
	TInlineComponentArray<UHUMAIdentityComponent*> Identities(GetOwner());
	if (Identities.Num() != 1)
	{
		Fail(TEXT("Playback Actor BP must contain exactly one HUMA Identity component."));
		return;
	}
	Identity = Identities[0];
	if (Identity->IdentityData && Identity->IdentityData != InitialData)
	{
		Fail(TEXT("Playback Actor BP's Identity Data does not match the playback controller."));
		return;
	}
	Identity->IdentityData = InitialData;
	TargetMesh = Identity->TargetMesh;
	if (!TargetMesh)
	{
		TInlineComponentArray<USkeletalMeshComponent*> Meshes(GetOwner());
		for (USkeletalMeshComponent* Mesh : Meshes)
		{
			if (Mesh->GetSkeletalMeshAsset() != InitialData->ReferenceMesh) { continue; }
			if (TargetMesh)
			{
				Fail(TEXT("Playback Actor BP has multiple matching meshes; assign Identity.TargetMesh explicitly."));
				return;
			}
			TargetMesh = Mesh;
		}
	}
	if (!TargetMesh || TargetMesh->GetOwner() != GetOwner()
		|| TargetMesh->GetSkeletalMeshAsset() != InitialData->ReferenceMesh)
	{
		Fail(TEXT("Playback Actor BP must own the skeletal mesh referenced by Identity Data."));
		return;
	}
	Identity->TargetMesh = TargetMesh;
	if (!Identity->SetBetas(InitialBetas) || !Identity->ApplyIdentity())
	{
		Fail(Identity->LastError);
		return;
	}
	TargetMesh->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	TargetMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	bPlaybackInitialized = true;
	InitialBetas.Reset();
}

void UHUMARuntimePoseComponent::SetTrackVisible(bool bVisible)
{
	AActor* Owner = GetOwner();
	if (bVisible)
	{
		if (!bTrackHidden) { return; }
		Owner->SetActorHiddenInGame(bOwnerWasHidden);
		for (const TPair<TWeakObjectPtr<AActor>, bool>& Entry : HiddenAttachments)
		{
			if (AActor* Actor = Entry.Key.Get()) { Actor->SetActorHiddenInGame(Entry.Value); }
		}
		HiddenAttachments.Reset();
		bTrackHidden = false;
		return;
	}
	if (!bTrackHidden) { bOwnerWasHidden = Owner->IsHidden(); }
	Owner->SetActorHiddenInGame(true);
	bTrackHidden = true;
	TArray<AActor*> Attachments;
	Owner->GetAttachedActors(Attachments, true, true);
	for (AActor* Actor : Attachments)
	{
		if (!IsValid(Actor) || !Actor->IsOwnedBy(Owner)) { continue; }
		if (!HiddenAttachments.Contains(Actor)) { HiddenAttachments.Add(Actor, Actor->IsHidden()); }
		Actor->SetActorHiddenInGame(true);
	}
}

AHUMARuntimeCharacter::AHUMARuntimeCharacter()
{
	bUseRuntimeMotion = true;
	RuntimePose = CreateDefaultSubobject<UHUMARuntimePoseComponent>(TEXT("RuntimePose"));
	RuntimePose->TargetMesh = CharacterMesh;
}

UHUMARuntimePlaybackComponent::UHUMARuntimePlaybackComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	CharacterClass = AHUMARuntimeCharacter::StaticClass();
}

bool UHUMARuntimePlaybackComponent::Fail(const FString& Message)
{
	LastError = Message;
	UE_LOG(LogHUMAIdentity, Error, TEXT("%s: %s"), *GetPathName(), *Message);
	return false;
}

void UHUMARuntimePlaybackComponent::ClearPlayers()
{
	bPlaying = false;
	bReady = false;
	for (AActor* Player : Players)
	{
		if (!IsValid(Player)) { continue; }
		TArray<AActor*> Attachments;
		Player->GetAttachedActors(Attachments, true, true);
		for (int32 Index = Attachments.Num() - 1; Index >= 0; --Index)
		{
			AActor* Attached = Attachments[Index];
			if (IsValid(Attached) && Attached->IsOwnedBy(Player)) { Attached->Destroy(); }
		}
		Player->Destroy();
	}
	Players.Reset();
	ActiveTrackIndices.Reset();
	Dataset.Reset();
	CurrentTime = 0.0;
	Duration = 0.0;
}

bool UHUMARuntimePlaybackComponent::LoadDataset()
{
	check(IsInGameThread());
	const double LoadStart = FPlatformTime::Seconds();
	LastLoadStartedSeconds = LoadStart;
	ON_SCOPE_EXIT { LastLoadMilliseconds = (FPlatformTime::Seconds() - LoadStart) * 1000.0; };
	ClearPlayers();
	LastError.Reset();
	if (!GetOwner() || !GetWorld() || !GetWorld()->IsGameWorld() || !CharacterClass)
	{
		return Fail(TEXT("Runtime playback requires a game world, owner, and character class."));
	}
	FString Path = ManifestPath;
	if (FPaths::IsRelative(Path)) { Path = FPaths::Combine(FPaths::ProjectContentDir(), Path); }
	FHUMARuntimeDataset Loaded;
	FString Error;
	if (!LoadHUMARuntimeDataset(Path, IdentityData, ExpectedModelId, Loaded, Error)) { return Fail(Error); }
	Dataset = MakeShared<FHUMARuntimeDataset>(MoveTemp(Loaded));
	const FTransform Placement = GetOwner()->GetActorTransform();
	for (int32 TrackIndex = 0; TrackIndex < Dataset->Tracks.Num(); ++TrackIndex)
	{
		const FHUMARuntimeTrack& Track = Dataset->Tracks[TrackIndex];
		if (ExcludedRoleIds.Contains(Track.RoleId)) { continue; }
		ActiveTrackIndices.Add(TrackIndex);
		Duration = FMath::Max(Duration, Track.GetDuration());
		AActor* Player = GetWorld()->SpawnActorDeferred<AActor>(
			CharacterClass, Placement, GetOwner(), nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Player)
		{
			ClearPlayers();
			return Fail(TEXT("Could not spawn a runtime player."));
		}
		Players.Add(Player);
		if (AHUMARuntimeCharacter* Native = Cast<AHUMARuntimeCharacter>(Player))
		{
			Native->bUseRuntimeMotion = true;
			Native->Motion = nullptr;
			Native->TrackId = Track.TrackId;
			Native->PlayerId = Track.PlayerId;
			Native->TeamId = Track.TeamId;
			Native->JerseyNumber = Track.JerseyNumber;
			Native->DisplayName = FString::Printf(TEXT("Track %d / player %s / #%d"), Track.TrackId, *Track.PlayerId, Track.JerseyNumber);
			Native->CharacterMesh->SetSkeletalMeshAsset(IdentityData->ReferenceMesh);
		}
		UHUMARuntimePoseComponent* Runtime = Player->FindComponentByClass<UHUMARuntimePoseComponent>();
		const bool bAddRuntimeComponent = Runtime == nullptr;
		if (bAddRuntimeComponent)
		{
			Runtime = NewObject<UHUMARuntimePoseComponent>(Player, TEXT("HUMARuntimePose"));
			Player->AddInstanceComponent(Runtime);
		}
		Runtime->PreparePlayback(IdentityData, Track);
		if (const TSharedPtr<const FHUMARuntimePose, ESPMode::ThreadSafe> InitialPose = SamplePose(Track, 0.0))
		{
			Runtime->SetPose(InitialPose.ToSharedRef());
		}
		if (bAddRuntimeComponent) { Runtime->RegisterComponent(); }
		Player->FinishSpawning(Placement);
		if (!Runtime->bPlaybackInitialized || !Runtime->LastError.IsEmpty())
		{
			Error = Runtime->LastError.IsEmpty() ? TEXT("Runtime pose did not initialize on the spawned Actor BP.") : Runtime->LastError;
			ClearPlayers();
			return Fail(Error);
		}
		Runtime->TargetMesh->AddTickPrerequisiteComponent(this);
		OnPlayerSpawned.Broadcast(Player, Runtime);
#if WITH_EDITOR
		Player->SetActorLabel(FString::Printf(TEXT("HUMA_Track_%02d_Player_%s"), Track.TrackId, *Track.PlayerId));
#endif
	}
	if (Players.IsEmpty())
	{
		ClearPlayers();
		return Fail(TEXT("The spawn filter excluded every track in the dataset."));
	}
	bReady = true;
	ApplyTime();
	UE_LOG(LogHUMAIdentity, Log, TEXT("Loaded runtime dataset: %s; %d tracks, duration %.6f s."), *Path, Players.Num(), Duration);
	return true;
}

bool UHUMARuntimePlaybackComponent::LoadAndPlay()
{
	return LoadDataset() && Play();
}

bool UHUMARuntimePlaybackComponent::Play()
{
	if (!bReady) { return Fail(TEXT("Load a valid dataset before starting playback.")); }
	if (CurrentTime >= Duration) { CurrentTime = 0.0; ApplyTime(); }
	LastError.Reset();
	bPlaying = true;
	return true;
}

void UHUMARuntimePlaybackComponent::Pause()
{
	bPlaying = false;
}

bool UHUMARuntimePlaybackComponent::Seek(double TimeSeconds)
{
	if (!bReady || !FMath::IsFinite(TimeSeconds) || TimeSeconds < 0 || TimeSeconds > Duration)
	{
		return Fail(TEXT("Seek requires loaded data and a finite time within the playback duration."));
	}
	CurrentTime = TimeSeconds;
	if (CurrentTime >= Duration) { bPlaying = false; }
	LastError.Reset();
	ApplyTime();
	return true;
}

bool UHUMARuntimePlaybackComponent::Restart()
{
	return Seek(0.0) && Play();
}

void UHUMARuntimePlaybackComponent::ApplyTime()
{
	for (int32 Index = 0; Index < Players.Num(); ++Index)
	{
		AActor* Player = Players[Index];
		if (!IsValid(Player))
		{
			bPlaying = false;
			Fail(TEXT("A runtime player was removed during playback; reload the dataset."));
			continue;
		}
		UHUMARuntimePoseComponent* Runtime = Player->FindComponentByClass<UHUMARuntimePoseComponent>();
		if (!Runtime || !Runtime->bPlaybackInitialized)
		{
			bPlaying = false;
			Fail(TEXT("A runtime pose component was removed or invalidated; reload the dataset."));
			continue;
		}
		const TSharedPtr<const FHUMARuntimePose, ESPMode::ThreadSafe> Pose = SamplePose(Dataset->Tracks[ActiveTrackIndices[Index]], CurrentTime);
		Runtime->SetTrackVisible(Pose.IsValid());
		if (!Pose) { continue; }
		Runtime->SampleTime = CurrentTime;
		Runtime->SetPose(Pose.ToSharedRef());
	}
}

TSharedPtr<const FHUMARuntimePose, ESPMode::ThreadSafe> UHUMARuntimePlaybackComponent::SamplePose(
	const FHUMARuntimeTrack& Track, double Time) const
{
	FHUMARuntimeSample Sample;
	if (!Track.Sample(Time, Sample)) { return nullptr; }
	const FReferenceSkeleton& Reference = IdentityData->ReferenceMesh->GetRefSkeleton();
	TSharedRef<FHUMARuntimePose, ESPMode::ThreadSafe> Pose = MakeShared<FHUMARuntimePose, ESPMode::ThreadSafe>();
	Pose->MeshLocalRotations.Reserve(Reference.GetNum());
	for (const FTransform& Bone : Reference.GetRefBonePose()) { Pose->MeshLocalRotations.Add(Bone.GetRotation()); }
	for (int32 Joint = 0; Joint < Dataset->JointToMeshBone.Num(); ++Joint)
	{
		Pose->MeshLocalRotations[Dataset->JointToMeshBone[Joint]] =
			Dataset->MeshRootRotation.Inverse() * Sample.LocalRotations[Joint] * Dataset->MeshRootRotation;
	}
	Pose->HipDisplacement = Dataset->MeshRootRotation.UnrotateVector(Sample.HipDisplacement);
	return Pose;
}

void UHUMARuntimePlaybackComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction)
{
	Super::TickComponent(DeltaTime, TickType, TickFunction);
	if (!bPlaying || !bReady) { return; }
	double NextTime = CurrentTime + DeltaTime;
	if (NextTime >= Duration)
	{
		if (bLoop) { NextTime = FMath::Fmod(NextTime, Duration); }
		else { NextTime = Duration; bPlaying = false; }
	}
	CurrentTime = NextTime;
	ApplyTime();
}

void UHUMARuntimePlaybackComponent::EndPlay(const EEndPlayReason::Type Reason)
{
	ClearPlayers();
	Super::EndPlay(Reason);
}

FString UHUMARuntimePlaybackComponent::GetPlaybackSummary() const
{
	if (!LastError.IsEmpty()) { return TEXT("ERROR: ") + LastError; }
	if (!bReady) { return TEXT("Runtime dataset not loaded."); }
	TArray<FString> Rates;
	for (int32 Index : ActiveTrackIndices)
	{
		const FHUMARuntimeTrack& Track = Dataset->Tracks[Index];
		Rates.AddUnique(FString::Printf(TEXT("%g"), Track.FPS));
	}
	return FString::Printf(TEXT("%d players | %s fps | %s"), Players.Num(), *FString::Join(Rates, TEXT(", ")),
		bPlaying ? TEXT("Playing") : TEXT("Paused"));
}

void UHUMARuntimePlaybackWidget::TogglePlayback()
{
	if (!Playback) { UE_LOG(LogHUMAIdentity, Error, TEXT("Playback widget has no playback component.")); return; }
	if (Playback->bPlaying) { Playback->Pause(); }
	else { Playback->Play(); }
}

void UHUMARuntimePlaybackWidget::RestartPlayback()
{
	if (Playback) { Playback->Restart(); }
}

void UHUMARuntimePlaybackWidget::BeginScrub()
{
	if (!Playback) { return; }
	bResumeAfterScrub = Playback->bPlaying;
	Playback->Pause();
}

void UHUMARuntimePlaybackWidget::EndScrub()
{
	if (Playback && bResumeAfterScrub && Playback->CurrentTime < Playback->Duration) { Playback->Play(); }
	bResumeAfterScrub = false;
}

void UHUMARuntimePlaybackWidget::SeekNormalized(float Value)
{
	if (Playback && Playback->bReady && FMath::IsFinite(Value))
	{
		Playback->Seek(FMath::Clamp(static_cast<double>(Value), 0.0, 1.0) * Playback->Duration);
	}
}

float UHUMARuntimePlaybackWidget::GetNormalizedTime() const
{
	return Playback && Playback->Duration > 0 ? static_cast<float>(Playback->CurrentTime / Playback->Duration) : 0.0f;
}

FText UHUMARuntimePlaybackWidget::GetTimeLabel() const
{
	return FText::FromString(FString::Printf(TEXT("%.3f / %.3f s"), Playback ? Playback->CurrentTime : 0.0, Playback ? Playback->Duration : 0.0));
}

FText UHUMARuntimePlaybackWidget::GetPlayButtonLabel() const
{
	return Playback && Playback->bPlaying ? NSLOCTEXT("HUMA", "Pause", "Pause") : NSLOCTEXT("HUMA", "Play", "Play");
}

FText UHUMARuntimePlaybackWidget::GetStatusLabel() const
{
	return FText::FromString(Playback ? Playback->GetPlaybackSummary() : TEXT("Waiting for playback controller."));
}

bool UHUMARuntimePlaybackWidget::CanControlPlayback() const
{
	return Playback && Playback->bReady;
}

AHUMARuntimePlaybackHost::AHUMARuntimePlaybackHost()
{
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Scene")));
	Playback = CreateDefaultSubobject<UHUMARuntimePlaybackComponent>(TEXT("Playback"));
}

bool AHUMARuntimePlaybackHost::ShowPlaybackControls()
{
	APlayerController* Controller = UGameplayStatics::GetPlayerController(this, 0);
	if (!Controller || !ControlsClass)
	{
		UE_LOG(LogHUMAIdentity, Error, TEXT("Assign a player controller and a Widget Blueprint class to show playback controls."));
		return false;
	}
	if (Controls) { Controls->RemoveFromParent(); }
	Controls = CreateWidget<UHUMARuntimePlaybackWidget>(Controller, ControlsClass);
	if (!Controls) { UE_LOG(LogHUMAIdentity, Error, TEXT("Could not create the playback Widget Blueprint.")); return false; }
	Controls->Playback = Playback;
	Controls->AddToViewport(10);
	Controller->bShowMouseCursor = true;
	FInputModeGameAndUI Input;
	Input.SetHideCursorDuringCapture(false);
	Controller->SetInputMode(Input);
	if (PlaybackCamera) { Controller->SetViewTarget(PlaybackCamera); }
	return true;
}
