#include "HUMALiveHawkEyeComponent.h"

#include "HUMALiveWorker.h"
#include "HUMAIdentity.h"
#include "HUMARuntimeData.h"
#include "HUMARuntimePlayback.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NNEModelData.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY(LogHUMALive);

UHUMALiveHawkEyeComponent::UHUMALiveHawkEyeComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	FramesFolder.Path = TEXT("Data/HawkEye");
}

bool UHUMALiveHawkEyeComponent::Fail(const FString& Message)
{
	LastError = Message;
	UE_LOG(LogHUMALive, Error, TEXT("%s"), *Message);
	return false;
}

void UHUMALiveHawkEyeComponent::BeginPlay()
{
	Super::BeginPlay();
	bTriedAutoStart = false;
}

void UHUMALiveHawkEyeComponent::EndPlay(const EEndPlayReason::Type Reason)
{
	if (Worker) { Worker->Shutdown(); Worker.Reset(); }
	bRunning = false;
	Super::EndPlay(Reason);
}

UHUMARuntimePlaybackComponent* UHUMALiveHawkEyeComponent::ResolvePlayback()
{
	if (Playback) { return Playback; }
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (UHUMARuntimePlaybackComponent* Found = It->FindComponentByClass<UHUMARuntimePlaybackComponent>())
		{
			Playback = Found;
			return Found;
		}
	}
	return nullptr;
}

namespace
{
	/** 相対パスはプロジェクトフォルダ（.uproject のあるフォルダ）基準で絶対パスにする。 */
	FString MakeProjectAbsolute(const FString& InPath)
	{
		FString Path = InPath;
		if (FPaths::IsRelative(Path)) { Path = FPaths::Combine(FPaths::ProjectDir(), Path); }
		FPaths::CollapseRelativeDirectories(Path);
		return FPaths::ConvertRelativePathToFull(Path);
	}
}

FString UHUMALiveHawkEyeComponent::ResolveModelPath() const
{
	// 1) 明示指定（相対パスはプロジェクト基準）
	if (!IkModelFile.FilePath.IsEmpty()) { return MakeProjectAbsolute(IkModelFile.FilePath); }
	// 2) プラグイン同梱のモデル
	const FString Bundled = MakeProjectAbsolute(TEXT("Plugins/HUMALiveHawkEye/Resources/ik_huma_causal_w9.onnx"));
	if (FPaths::FileExists(Bundled)) { return Bundled; }
	// 3) 旧構成（HUMA_v1.0.0 配下の npb_huma_ik）
	return MakeProjectAbsolute(TEXT("../../../npb_huma_ik-1.0.0/npb_huma_ik/assets/ik_huma_causal_w9.onnx"));
}

FString UHUMALiveHawkEyeComponent::ResolveFramesFolder() const
{
	// 1) 指定フォルダ（相対パスはプロジェクト基準）が存在すればそれを使う
	if (!FramesFolder.Path.IsEmpty())
	{
		const FString Configured = MakeProjectAbsolute(FramesFolder.Path);
		if (FPaths::DirectoryExists(Configured)) { return Configured; }
	}
	// 2) 既定の置き場所 <Project>/Data/HawkEye
	return MakeProjectAbsolute(TEXT("Data/HawkEye"));
}

bool UHUMALiveHawkEyeComponent::StartLive()
{
	check(IsInGameThread());
	if (bRunning) { return true; }
	LastError.Reset();
	UHUMARuntimePlaybackComponent* Source = ResolvePlayback();
	if (!Source || !Source->bReady || !Source->IdentityData)
	{
		return Fail(TEXT("HUMA Live: the runtime playback component is not ready (no players spawned yet)."));
	}

	// 体型・チーム・関節の対応は、再生コンポーネントが読んだのと同じデータセットから取る（体型は事前に固定）
	FString Manifest = Source->ManifestPath;
	if (FPaths::IsRelative(Manifest)) { Manifest = FPaths::Combine(FPaths::ProjectContentDir(), Manifest); }
	FHUMARuntimeDataset Dataset;
	FString Error;
	if (!LoadHUMARuntimeDataset(Manifest, Source->IdentityData, Source->ExpectedModelId, Dataset, Error))
	{
		return Fail(TEXT("HUMA Live: could not read the profile dataset: ") + Error);
	}
	if (Dataset.JointToMeshBone.Num() != hawkeye_live::kHumaJoints)
	{
		return Fail(FString::Printf(TEXT("HUMA Live: the dataset has %d joints (expected %d)."), Dataset.JointToMeshBone.Num(), hawkeye_live::kHumaJoints));
	}
	JointToMeshBone = Dataset.JointToMeshBone;
	MeshRootRotation = Dataset.MeshRootRotation;
	RefRotations.Reset();
	for (const FTransform& Bone : Source->IdentityData->ReferenceMesh->GetRefSkeleton().GetRefBonePose())
	{
		RefRotations.Add(Bone.GetRotation());
	}

	const UHUMAIdentityData* Identity = Source->IdentityData;
	const int32 HipJoint = Identity->JointNames.Find(Identity->HipBone);
	const int32 BetaCount = Identity->BetaNames.Num();
	if (HipJoint == INDEX_NONE || Identity->JointCoefficients.Num() < (HipJoint + 1) * BetaCount)
	{
		return Fail(TEXT("HUMA Live: the identity data has no hip joint regression."));
	}
	FHUMALiveWorkerSettings Settings;
	Settings.Folder = ResolveFramesFolder();
	Settings.bLoop = bLoop;
	Settings.bWatch = bWatchForNewFiles;
	Settings.MaxGapFrames = MaxGapFrames;
	Settings.bTimeline = !bWatchForNewFiles;
	for (const FHUMARuntimeTrack& Track : Dataset.Tracks)
	{
		if (Source->ExcludedRoleIds.Contains(Track.RoleId)) { continue; }  // レフェリーなど、出していない人物は計算しない
		hawkeye_live::Profile Profile;
		Profile.track_id = Track.TrackId;
		Profile.team_id = Track.TeamId;
		Profile.role_id = Track.RoleId;
		Profile.jersey_number = Track.JerseyNumber;
		Profile.betas.assign(Track.Betas.GetData(), Track.Betas.GetData() + Track.Betas.Num());
		// 腰の静止位置 J_hip(β) = テンプレート + Σ 係数 × β（メッシュのコンポーネント座標、cm）
		FVector Hip = Identity->JointTemplates[HipJoint];
		for (int32 Beta = 0; Beta < BetaCount && Beta < Track.Betas.Num(); ++Beta)
		{
			Hip += Identity->JointCoefficients[HipJoint * BetaCount + Beta] * Track.Betas[Beta];
		}
		Profile.hip_rest_ue_cm = {static_cast<float>(Hip.X), static_cast<float>(Hip.Y), static_cast<float>(Hip.Z)};
		Settings.Profiles[Track.TrackId] = MoveTemp(Profile);
	}
	if (Settings.Profiles.empty()) { return Fail(TEXT("HUMA Live: the dataset has no players to drive.")); }
	if (!FPaths::DirectoryExists(Settings.Folder)) { return Fail(TEXT("HUMA Live: frames folder not found: ") + Settings.Folder); }

	// 再生バーに出す長さのため、フォルダの先頭と最後のフレームの時刻を調べる
	int32 ProbeCount = 0;
	if (Settings.bTimeline)
	{
		FString ProbeError;
		if (!FHUMALiveWorker::Probe(Settings.Folder, LiveFirstUs, LiveLastUs, ProbeCount, ProbeError))
		{
			return Fail(TEXT("HUMA Live: could not read the frames folder: ") + ProbeError);
		}
	}
	if (Settings.bTimeline) { LoadRounds(); }

	// IK モデル（ONNX）を読み込む
	const FString ModelPath = ResolveModelPath();
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *ModelPath)) { return Fail(TEXT("HUMA Live: could not read the IK model: ") + ModelPath); }
	ModelData = NewObject<UNNEModelData>(this);
	ModelData->Init(TEXT("onnx"), Bytes);
	TUniquePtr<FHUMANneIkModel> Model = MakeUnique<FHUMANneIkModel>();
	if (!Model->Initialize(ModelData, Error)) { return Fail(TEXT("HUMA Live: ") + Error); }

	// 再生コンポーネントのモーションを止めて、こちらの姿勢に切り替える
	bResumePlaybackOnStop = Source->bPlaying;
	LiveManifest = Source->ManifestPath;
	Buffers.Reset();
	bClockStarted = false;
	ClockUs = 0.0;
	bTimeline = Settings.bTimeline;
	if (bTimeline)
	{
		// 再生バー（シーク・再生/一時停止・先頭へ）をフォルダの長さに合わせる。
		// 再生コンポーネント自身の進行は止め（元のモーションを当てないように）、時刻と状態だけこちらで持つ。
		OriginalDuration = Source->Duration;
		Source->Duration = static_cast<double>(LiveLastUs - LiveFirstUs) * 1.0e-6 + 0.02;
		Source->CurrentTime = 0.0;
		Source->bPlaying = true;
		Source->SetComponentTickEnabled(false);
		LastWrittenTime = 0.0;
		ClockUs = static_cast<double>(LiveFirstUs);
		HeldClockUs = ClockUs;
		bClockStarted = true;
		bBuffering = true;
		bPendingReset = false;
		ExpectedGeneration = 0;
		UE_LOG(LogHUMALive, Log, TEXT("Timeline: %d files, %.3f s (seek bar length)"), ProbeCount, Source->Duration);
	}
	else
	{
		Source->Pause();
	}
	NewestUs = 0;
	FramesProcessed = 0;
	AverageIkMilliseconds = 0.f;
	Worker = MakeShared<FHUMALiveWorker>(MoveTemp(Settings), MoveTemp(Model));
	if (bTimeline) { Worker->ConsumerClockUs = LiveFirstUs; }
	Worker->Start();
	bRunning = true;
	UE_LOG(LogHUMALive, Log, TEXT("HUMA Live started: %s (model %s)"), *ResolveFramesFolder(), *ModelPath);
	return true;
}

void UHUMALiveHawkEyeComponent::StopLiveInternal(bool bRestorePlayback)
{
	if (Worker) { Worker->Shutdown(); Worker.Reset(); }
	const bool bWasRunning = bRunning;
	bRunning = false;
	Buffers.Reset();
	if (bTimeline)
	{
		bTimeline = false;
		if (Playback)
		{
			Playback->SetComponentTickEnabled(true);
			if (bRestorePlayback && Playback->bReady)
			{
				Playback->Duration = OriginalDuration;  // 別のデータセットに切り替わったときは、そちらの長さをそのまま使う
				Playback->CurrentTime = 0.0;
				Playback->bPlaying = bResumePlaybackOnStop;
			}
		}
	}
	if (bWasRunning && bRestorePlayback && Playback && Playback->bReady)
	{
		for (AActor* Player : Playback->Players)
		{
			if (UHUMARuntimePoseComponent* Runtime = IsValid(Player) ? Player->FindComponentByClass<UHUMARuntimePoseComponent>() : nullptr)
			{
				Runtime->SetTrackVisible(true);
			}
		}
		Playback->Seek(FMath::Clamp(Playback->CurrentTime, 0.0, Playback->Duration));  // 元のモーションの姿勢に戻す
		if (bResumePlaybackOnStop) { Playback->Play(); }
	}
}

void UHUMALiveHawkEyeComponent::ToggleLive()
{
	if (bRunning) { StopLive(); }
	else { StartLive(); }
}

void UHUMALiveHawkEyeComponent::DrainResults()
{
	TSharedPtr<FHUMALiveResult> Result;
	while (Worker && Worker->Results.Dequeue(Result))
	{
		if (bTimeline && Result->Generation != ExpectedGeneration) { continue; }  // シーク前の古い結果
		if (!Result->Error.IsEmpty())
		{
			LastError = Result->Error;
			UE_LOG(LogHUMALive, Warning, TEXT("%s"), *Result->Error);
		}
		if (Result->TimeUs == 0 && Result->Poses.empty()) { continue; }
		if (bPendingReset)
		{
			Buffers.Reset();  // シーク後の最初の結果が届いた。それまでは元の姿勢を保持していた
			bPendingReset = false;
		}
		++FramesProcessed;
		AverageIkMilliseconds = FMath::Lerp(AverageIkMilliseconds, static_cast<float>(Result->IkMilliseconds), FramesProcessed == 1 ? 1.f : 0.05f);
		NewestUs = FMath::Max(NewestUs, Result->TimeUs);
		if (!bClockStarted)
		{
			ClockUs = static_cast<double>(Result->TimeUs) - DelaySeconds * 1.0e6;
			bClockStarted = true;
		}
		for (const hawkeye_live::Pose& Pose : Result->Poses)
		{
			FTimedPose Timed;
			Timed.TimeUs = Pose.time_us;
			Timed.Rotations.SetNumUninitialized(hawkeye_live::kHumaJoints);
			for (int32 Joint = 0; Joint < hawkeye_live::kHumaJoints; ++Joint)
			{
				const float* Q = Pose.quat_xyzw.data() + Joint * 4;
				Timed.Rotations[Joint] = FQuat(Q[0], Q[1], Q[2], Q[3]);
			}
			Timed.Hip = FVector(Pose.hip_displacement_cm[0], Pose.hip_displacement_cm[1], Pose.hip_displacement_cm[2]);
			// 重心: フィールド（Z-up、m）→ UE ワールド cm。腰の変位と同じ (x, −y, z) × 100
			Timed.Com = FVector(Pose.com_m[0] * 100.0, -Pose.com_m[1] * 100.0, Pose.com_m[2] * 100.0);
			Timed.bHasCom = Pose.has_com;
			TArray<FTimedPose>& Buffer = Buffers.FindOrAdd(Pose.track_id);
			if (Buffer.Num() > 0 && Buffer.Last().TimeUs >= Timed.TimeUs) { Buffer.Reset(); }
			Buffer.Add(MoveTemp(Timed));
		}
	}
}

bool UHUMALiveHawkEyeComponent::SampleTrack(int32 TrackId, double Clock, TArray<FQuat>& OutRotations, FVector& OutHip) const
{
	const TArray<FTimedPose>* Buffer = Buffers.Find(TrackId);
	if (!Buffer || Buffer->Num() == 0) { return false; }
	const FTimedPose& Last = Buffer->Last();
	if (Clock > static_cast<double>(Last.TimeUs) + HideAfterSeconds * 1.0e6) { return false; }  // しばらく届いていない
	int32 Upper = 0;
	while (Upper < Buffer->Num() && static_cast<double>((*Buffer)[Upper].TimeUs) < Clock) { ++Upper; }
	if (Upper == 0)
	{
		const FTimedPose& FirstPose = (*Buffer)[0];
		OutRotations = FirstPose.Rotations;
		OutHip = FirstPose.Hip;
		return Clock >= static_cast<double>(FirstPose.TimeUs) - HideAfterSeconds * 1.0e6;
	}
	if (Upper >= Buffer->Num())
	{
		OutRotations = Last.Rotations;  // 次のフレームがまだ無い: 最新を保持
		OutHip = Last.Hip;
		return true;
	}
	const FTimedPose& A = (*Buffer)[Upper - 1];
	const FTimedPose& B = (*Buffer)[Upper];
	const double Span = static_cast<double>(B.TimeUs - A.TimeUs);
	const float Alpha = Span > 0 ? static_cast<float>((Clock - static_cast<double>(A.TimeUs)) / Span) : 0.f;
	OutRotations.SetNumUninitialized(A.Rotations.Num());
	for (int32 Joint = 0; Joint < A.Rotations.Num(); ++Joint)
	{
		OutRotations[Joint] = FQuat::Slerp(A.Rotations[Joint], B.Rotations[Joint], Alpha).GetNormalized();
	}
	OutHip = FMath::Lerp(A.Hip, B.Hip, Alpha);
	return true;
}

// 選手を手袋（取り付けられたアクター）と服（ClothBind の元の体がこの選手のもの）ごと隠す・戻す
void UHUMALiveHawkEyeComponent::SetPlayerHidden(AActor* Player, bool bHidden)
{
	bool* Prev = HiddenState.Find(Player);
	const bool bChanged = !Prev || *Prev != bHidden;
	// 服やグローブは後から作られることがあるので、隠しているあいだは 0.25 秒ごとに隠し直す
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	if (!bChanged && (!bHidden || Now - LastHideScanSeconds < 0.25)) { return; }
	if (bHidden) { LastHideScanSeconds = Now; }
	HiddenState.Add(Player, bHidden);
	Player->SetActorHiddenInGame(bHidden);
	TArray<AActor*> Attached;
	Player->GetAttachedActors(Attached, true, true);
	for (AActor* Child : Attached) { if (Child) { Child->SetActorHiddenInGame(bHidden); } }
	// HUMAClothBind の服: コンポーネントの GetSourceBodyActor() がこの選手なら隠す（モジュールに依存しないよう名前で呼ぶ）
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		for (UActorComponent* Comp : It->GetComponents())
		{
			if (!Comp || !Comp->GetClass()->GetName().Contains(TEXT("ClothBind"))) { continue; }
			UFunction* Getter = Comp->FindFunction(TEXT("GetSourceBodyActor"));
			if (!Getter || Getter->ParmsSize != sizeof(AActor*)) { continue; }
			struct { AActor* ReturnValue = nullptr; } Params;
			Comp->ProcessEvent(Getter, &Params);
			if (Params.ReturnValue == Player) { It->SetActorHiddenInGame(bHidden); }
		}
	}
}

void UHUMALiveHawkEyeComponent::ApplyPoses(double Clock)
{
	if (!Playback) { return; }
	TArray<FQuat> Rotations;
	FVector Hip;
	for (AActor* Player : Playback->Players)
	{
		if (!IsValid(Player)) { continue; }
		UHUMARuntimePoseComponent* Runtime = Player->FindComponentByClass<UHUMARuntimePoseComponent>();
		if (!Runtime || !Runtime->bPlaybackInitialized) { continue; }
		if (Runtime->TargetMesh && !Prerequisites.Contains(Runtime))
		{
			Runtime->TargetMesh->AddTickPrerequisiteComponent(this);  // 骨の更新より先に姿勢を入れる
			Prerequisites.Add(Runtime);
		}
		const bool bHave = SampleTrack(Runtime->TrackId, Clock, Rotations, Hip);
		const bool bHideThis = HiddenTrackId != 0 && Runtime->TrackId == HiddenTrackId;
		SetPlayerHidden(Player, bHideThis);
		Runtime->SetTrackVisible(bHave && !bHideThis);
		if (!bHave) { continue; }
		// UHUMARuntimePlaybackComponent::SamplePose と同じ変換（FBX のルート回転で共役をとる）
		TSharedRef<FHUMARuntimePose, ESPMode::ThreadSafe> Pose = MakeShared<FHUMARuntimePose, ESPMode::ThreadSafe>();
		Pose->MeshLocalRotations = RefRotations;
		for (int32 Joint = 0; Joint < JointToMeshBone.Num() && Joint < Rotations.Num(); ++Joint)
		{
			Pose->MeshLocalRotations[JointToMeshBone[Joint]] = MeshRootRotation.Inverse() * Rotations[Joint] * MeshRootRotation;
		}
		Pose->HipDisplacement = MeshRootRotation.UnrotateVector(Hip);
		Runtime->SampleTime = Clock * 1.0e-6;
		Runtime->SetPose(Pose);
	}
}

void UHUMALiveHawkEyeComponent::SeekLive(double TimeSeconds)
{
	if (!Worker) { return; }
	const double Duration = static_cast<double>(LiveLastUs - LiveFirstUs) * 1.0e-6;
	const double Clamped = FMath::Clamp(TimeSeconds, 0.0, Duration);
	if (!bBuffering) { HeldClockUs = ClockUs; }  // 新しい姿勢が届くまでは、直前の姿勢を出し続ける
	ClockUs = static_cast<double>(LiveFirstUs) + Clamped * 1.0e6;
	bBuffering = true;
	bPendingReset = true;
	NewestUs = 0;
	SinceSeekSeconds = 0.0;
	Worker->ConsumerClockUs = static_cast<int64>(ClockUs);
	ExpectedGeneration = Worker->SeekTo(static_cast<int64>(ClockUs));
	LastWrittenTime = Clamped;
}

void UHUMALiveHawkEyeComponent::TickTimeline(float DeltaTime)
{
	if (SequenceDataTime >= 0.f) { if (TickSequenceDriven()) { return; } }
	else { bSequenceDriven = false; }
	UHUMARuntimePlaybackComponent* Source = Playback;
	const double Duration = static_cast<double>(LiveLastUs - LiveFirstUs) * 1.0e-6;

	// 再生バーのシークや「先頭へ」は再生コンポーネントの CurrentTime を書き換える。こちらが前に書いた値と違えばシーク
	SinceSeekSeconds += DeltaTime;
	bool bSeekDeferred = false;
	if (FMath::Abs(Source->CurrentTime - LastWrittenTime) > 1.0e-6)
	{
		// つまみをドラッグ中は毎フレーム値が変わる。読み直しが間に合うよう、シークの間隔を少しあける
		if (!bBuffering || SinceSeekSeconds >= 0.08) { SeekLive(Source->CurrentTime); }
		else { bSeekDeferred = true; }
	}
	DrainResults();
	if (bBuffering)
	{
		const double Wait = Source->bPlaying ? DelaySeconds : 0.0;  // 止まっているときは、届き次第すぐ見せる
		const int64 Need = FMath::Min(static_cast<int64>(ClockUs + Wait * 1.0e6), LiveLastUs);
		if (!bPendingReset && NewestUs >= Need) { bBuffering = false; }
	}
	else if (Source->bPlaying)
	{
		ClockUs += static_cast<double>(DeltaTime) * 1.0e6;  // ワールドの時間（スロー再生込み）
		if (ClockUs >= static_cast<double>(LiveLastUs))
		{
			if (bLoop) { SeekLive(0.0); }
			else
			{
				ClockUs = static_cast<double>(LiveLastUs);
				Source->bPlaying = false;
			}
		}
	}
	if (Worker) { Worker->ConsumerClockUs = static_cast<int64>(ClockUs); }
	if (!bBuffering)
	{
		// 古いフレームを捨てる（補間に要る 1 つ前は残す）
		for (TPair<int32, TArray<FTimedPose>>& Pair : Buffers)
		{
			TArray<FTimedPose>& Buffer = Pair.Value;
			int32 Drop = 0;
			while (Drop + 1 < Buffer.Num() && static_cast<double>(Buffer[Drop + 1].TimeUs) < ClockUs) { ++Drop; }
			if (Drop > 0) { Buffer.RemoveAt(0, Drop, EAllowShrinking::No); }
		}
	}
	if (bBuffering && Buffers.Num() == 0) { return; }  // 最初の姿勢が届くまでは何もしない
	ApplyPoses(bBuffering ? HeldClockUs : ClockUs);
	if (bSeekDeferred) { return; }  // ユーザーが動かしている値を上書きしない

	// 再生バーに現在位置を出す
	double Shown = FMath::Clamp((ClockUs - static_cast<double>(LiveFirstUs)) * 1.0e-6, 0.0, Duration);
	if (!bBuffering && !Source->bPlaying && ClockUs >= static_cast<double>(LiveLastUs)) { Shown = Source->Duration; }  // 「再生」で先頭に戻るように
	Source->CurrentTime = Shown;
	LastWrittenTime = Shown;
}

void UHUMALiveHawkEyeComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction)
{
	Super::TickComponent(DeltaTime, TickType, TickFunction);
	if (const AHUMALiveHawkEyeActor* OwnerActor = Cast<AHUMALiveHawkEyeActor>(GetOwner()))
	{
		SequenceDataTime = OwnerActor->SequenceDataTime;  // Sequencer はアクター側の値にキーを打つ
		HiddenTrackId = OwnerActor->HiddenTrackId;
	}
	if (!bRunning)
	{
		if (bAutoStart && !bTriedAutoStart)
		{
			UHUMARuntimePlaybackComponent* Source = ResolvePlayback();
			// 最初に始めたのと同じデータセット（試合シーン）のときだけ自動で始める
			if (Source && Source->bReady && (LiveManifest.IsEmpty() || Source->ManifestPath == LiveManifest))
			{
				bTriedAutoStart = true;
				StartLive();
			}
		}
	}
	else
	{
		// シーン切り替え（別のデータセットの読み直し）があったら、ライブを止めてそのシーンの再生に任せる
		if (!Playback || !Playback->bReady || Playback->ManifestPath != LiveManifest)
		{
			StopLiveInternal(false);
			bTriedAutoStart = false;  // 元のシーンに戻ったら、また自動で始める
		}
		else if (bTimeline)
		{
			TickTimeline(DeltaTime);
		}
		else
		{
			if (Playback->bPlaying) { Playback->Pause(); }  // 再生の UI などで再開されても、ライブ中は止めておく
			DrainResults();
			if (bClockStarted)
			{
				ClockUs += static_cast<double>(DeltaTime) * 1.0e6;  // ワールドの時間（スロー再生込み）
				// フォルダを監視しているときは、最新のデータから遅れすぎないように追いつく
				const double Target = static_cast<double>(NewestUs) - DelaySeconds * 1.0e6;
				if (bWatchForNewFiles && ClockUs < Target - 0.5e6) { ClockUs = Target; }
				if (Worker) { Worker->ConsumerClockUs = static_cast<int64>(ClockUs); }
				// 古いフレームを捨てる（補間に要る 1 つ前は残す）
				for (TPair<int32, TArray<FTimedPose>>& Pair : Buffers)
				{
					TArray<FTimedPose>& Buffer = Pair.Value;
					int32 Drop = 0;
					while (Drop + 1 < Buffer.Num() && static_cast<double>(Buffer[Drop + 1].TimeUs) < ClockUs) { ++Drop; }
					if (Drop > 0) { Buffer.RemoveAt(0, Drop, EAllowShrinking::No); }
				}
				ApplyPoses(ClockUs);
			}
			if (Worker && Worker->bFinished && Buffers.Num() > 0)
			{
				bool bAllPast = true;
				for (const TPair<int32, TArray<FTimedPose>>& Pair : Buffers)
				{
					if (Pair.Value.Num() > 0 && static_cast<double>(Pair.Value.Last().TimeUs) > ClockUs) { bAllPast = false; }
				}
				if (bAllPast) { LastError = TEXT("Reached the end of the frames folder."); }
			}
		}
	}
	if (bRunning)
	{
		// 動作確認用: 2 秒ごとに状態をログへ
		LogAccumulator += DeltaTime;
		if (LogAccumulator >= 2.f)
		{
			LogAccumulator = 0.f;
			int32 Ready = 0;
			if (Playback) { for (AActor* P : Playback->Players) { if (IsValid(P)) { const UHUMARuntimePoseComponent* R = P->FindComponentByClass<UHUMARuntimePoseComponent>(); if (R && R->bPlaybackInitialized) { ++Ready; } } } }
			UE_LOG(LogHUMALive, Log, TEXT("Status: %s | players ready %d / %d | clockStarted %d | err '%s'"), *GetStatusText(), Ready, Playback ? Playback->Players.Num() : 0, bClockStarted ? 1 : 0, *LastError);
		}
	}
	if (bShowStatus && GEngine)
	{
		GEngine->AddOnScreenDebugMessage(static_cast<uint64>(GetUniqueID()), 0.f, bRunning ? FColor::Cyan : FColor(128, 128, 128),
			GetStatusText());
	}
}

FString UHUMALiveHawkEyeComponent::GetStatusText() const
{
	if (!bRunning)
	{
		return LastError.IsEmpty() ? TEXT("HUMA Live: off (ke * LiveToggle)") : TEXT("HUMA Live: off - ") + LastError;
	}
	const int32 Files = Worker ? Worker->FileCount.load() : 0;
	FString Position;
	if (bTimeline)
	{
		// 再生位置（表示中の時刻）をフォルダ内のフレーム番号に直す。ファイルはほぼ等間隔（50fps）に並んでいる
		const double Duration = static_cast<double>(LiveLastUs - LiveFirstUs) * 1.0e-6;
		const double Shown = FMath::Clamp(((bBuffering ? HeldClockUs : ClockUs) - static_cast<double>(LiveFirstUs)) * 1.0e-6, 0.0, Duration);
		const int32 Frame = (Duration > 0.0 && Files > 1) ? FMath::RoundToInt32(Shown / Duration * (Files - 1)) + 1 : 1;
		Position = FString::Printf(TEXT("%.2f s%s | frame %d / %d"), Shown, *DescribeRoundTime(Shown), Frame, Files);
	}
	else
	{
		Position = FString::Printf(TEXT("%d files"), Files);
	}
	int32 Visible = 0;  // 今の時刻に姿勢がある人数
	{
		TArray<FQuat> Unused;
		FVector UnusedHip;
		const double Clock = bBuffering ? HeldClockUs : ClockUs;
		for (const TPair<int32, TArray<FTimedPose>>& Pair : Buffers)
		{
			if (SampleTrack(Pair.Key, Clock, Unused, UnusedHip)) { ++Visible; }
		}
	}
	FString Text = FString::Printf(TEXT("HUMA Live: %s | read %d | IK %.1f ms/frame | %d tracks | delay %.2f s%s"),
		*Position, FramesProcessed, AverageIkMilliseconds, Visible, DelaySeconds,
		bWatchForNewFiles ? TEXT(" | watching") : (bLoop ? TEXT(" | loop") : TEXT("")));
	if (!LastError.IsEmpty()) { Text += TEXT(" | ") + LastError; }
	return Text;
}

// ---------------------------------------------------------------- Sequencer から時刻を指定する（動画書き出し用）
bool UHUMALiveHawkEyeComponent::TickSequenceDriven()
{
	UHUMARuntimePlaybackComponent* Source = Playback;
	if (!Source || !Worker) { return false; }
	const double Duration = static_cast<double>(LiveLastUs - LiveFirstUs) * 1.0e-6;
	const double Target = FMath::Clamp(static_cast<double>(SequenceDataTime), 0.0, Duration);
	const double TargetUs = static_cast<double>(LiveFirstUs) + Target * 1.0e6;
	Source->bPlaying = false;  // 時刻はこちらで決めるので、再生バー側では進めない

	// 初回・巻き戻し・先読みより先へ飛んだときだけ読み直す。少しずつ進むあいだは時計を動かすだけ
	const bool bNeedSeek = !bSequenceDriven || TargetUs < ClockUs - 1.0 || TargetUs > static_cast<double>(NewestUs) + 0.25e6;
	if (bNeedSeek)
	{
		SeekLive(Target);
		bSequenceDriven = true;
	}
	ClockUs = TargetUs;
	Worker->ConsumerClockUs = static_cast<int64>(ClockUs);

	// その時刻の姿勢がそろうまで待つ（書き出しで姿勢が遅れないように）
	const double Deadline = FPlatformTime::Seconds() + (bWaitForPosesInSequence ? 3.0 : 0.0);
	do
	{
		DrainResults();
		if (!bPendingReset && static_cast<double>(NewestUs) >= FMath::Min(ClockUs, static_cast<double>(LiveLastUs))) { break; }
		if (!bWaitForPosesInSequence) { break; }
		FPlatformProcess::Sleep(0.001f);
	} while (FPlatformTime::Seconds() < Deadline);
	if (!bPendingReset) { bBuffering = false; }

	// 古いフレームを捨てる（補間に要る 1 つ前は残す）
	for (TPair<int32, TArray<FTimedPose>>& Pair : Buffers)
	{
		TArray<FTimedPose>& Buffer = Pair.Value;
		int32 Drop = 0;
		while (Drop + 1 < Buffer.Num() && static_cast<double>(Buffer[Drop + 1].TimeUs) < ClockUs) { ++Drop; }
		if (Drop > 0) { Buffer.RemoveAt(0, Drop, EAllowShrinking::No); }
	}
	if (Buffers.Num() > 0) { ApplyPoses(bBuffering ? HeldClockUs : ClockUs); }
	Source->CurrentTime = Target;
	LastWrittenTime = Target;
	return true;
}

double UHUMALiveHawkEyeComponent::GetDisplayTimeSeconds() const
{
	return ((bBuffering ? HeldClockUs : ClockUs) - static_cast<double>(LiveFirstUs)) * 1.0e-6;
}

bool UHUMALiveHawkEyeComponent::GetTrackCenterOfMass(int32 TrackId, FVector& OutLocation) const
{
	const TArray<FTimedPose>* Buffer = Buffers.Find(TrackId);
	if (!Buffer || Buffer->Num() == 0) { return false; }
	const double Clock = bBuffering ? HeldClockUs : ClockUs;
	int32 Upper = 0;
	while (Upper < Buffer->Num() && static_cast<double>((*Buffer)[Upper].TimeUs) < Clock) { ++Upper; }
	if (Upper == 0 || Upper >= Buffer->Num())
	{
		const FTimedPose& P = (*Buffer)[FMath::Clamp(Upper, 0, Buffer->Num() - 1)];
		OutLocation = P.Com;
		return P.bHasCom;
	}
	const FTimedPose& A = (*Buffer)[Upper - 1];
	const FTimedPose& B = (*Buffer)[Upper];
	const double Span = static_cast<double>(B.TimeUs - A.TimeUs);
	const double Alpha = Span > 0 ? (Clock - static_cast<double>(A.TimeUs)) / Span : 0.0;
	OutLocation = FMath::Lerp(A.Com, B.Com, Alpha);
	return A.bHasCom && B.bHasCom;
}

// ---------------------------------------------------------------- デバッグ: ラウンドとシーンへのジャンプ
namespace
{
	// 自由視点コンテンツ案（2026-11-16 資料 P9〜P12）の候補シーン。F9（ベン vs プログレイス）のデータから探した時刻
	struct FScenePreset { int32 Id; int32 Round; double SecondsInRound; const TCHAR* Label; };
	const FScenePreset GScenePresets[] = {
		{11, 1, 158.60, TEXT("Idea1 A: Benn one-two (right 0.16 s)")},
		{12, 1, 179.00, TEXT("Idea1 B: one-two just before the bell")},
		{21, 3,  96.70, TEXT("Idea2 A: Prograis slips a left hook on the ropes")},
		{31, 4, 141.97, TEXT("Idea3 A: Prograis step-in 164cm -> 105cm")},
		{32, 4, 102.12, TEXT("Idea3 B: Prograis step-in 163cm -> 103cm")},
		{33, 4, 156.45, TEXT("Idea3 C: Prograis step-in 101cm -> 34cm")},
		{41, 5,  86.40, TEXT("Idea4 A: Benn left hook (hip -> shoulder -> fist)")},
		{42, 5, 133.10, TEXT("Idea4 B: Benn left that reached the head")},
	};
	// rounds.json が見つからないときに使う F9 の値（再生バー上の秒）
	const double GF9RoundStart[] = {212.07, 452.38, 692.23, 933.09, 1173.18, 1413.53, 1653.92, 1893.27, 2133.50, 2382.66};
	const double GF9RoundEnd[]   = {391.86, 632.37, 872.96, 1113.25, 1353.93, 1594.12, 1834.35, 2074.02, 2323.31, 2564.45};
}

void UHUMALiveHawkEyeComponent::Notify(const FString& Message, const FColor& Color) const
{
	UE_LOG(LogHUMALive, Log, TEXT("%s"), *Message);
	if (GEngine) { GEngine->AddOnScreenDebugMessage(-1, 6.f, Color, Message); }
}

void UHUMALiveHawkEyeComponent::LoadRounds()
{
	Rounds.Reset();
	RoundsSource.Reset();
	TArray<FString> Candidates;
	if (!RoundsFile.FilePath.IsEmpty()) { Candidates.Add(MakeProjectAbsolute(RoundsFile.FilePath)); }
	const FString Folder = ResolveFramesFolder();
	Candidates.Add(FPaths::Combine(Folder, TEXT("rounds.json")));
	Candidates.Add(FPaths::Combine(Folder, TEXT(".."), TEXT("rounds.json")));
	for (const FString& Path : Candidates)
	{
		FString Text;
		if (!FPaths::FileExists(Path) || !FFileHelper::LoadFileToString(Text, *Path)) { continue; }
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid()) { continue; }
		const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
		if (!Root->TryGetArrayField(TEXT("rounds"), List)) { continue; }
		for (const TSharedPtr<FJsonValue>& Item : *List)
		{
			const TSharedPtr<FJsonObject> R = Item->AsObject();
			if (!R.IsValid()) { continue; }
			FRoundSpan Span;
			Span.Round = static_cast<int32>(R->GetNumberField(TEXT("round")));
			double StartUs = 0.0, EndUs = 0.0;
			// start_time / end_time（µs）があれば、フォルダ先頭の時刻から秒に直す（double で誤差なく表せる桁数）。なければ start_sec / end_sec
			if (R->TryGetNumberField(TEXT("start_time"), StartUs) && R->TryGetNumberField(TEXT("end_time"), EndUs) && LiveFirstUs != 0)
			{
				Span.StartSec = (static_cast<int64>(StartUs) - LiveFirstUs) * 1.0e-6;
				Span.EndSec = (static_cast<int64>(EndUs) - LiveFirstUs) * 1.0e-6;
			}
			else
			{
				Span.StartSec = R->GetNumberField(TEXT("start_sec"));
				Span.EndSec = R->GetNumberField(TEXT("end_sec"));
			}
			Rounds.Add(Span);
		}
		if (Rounds.Num() > 0) { RoundsSource = Path; break; }
	}
	if (Rounds.Num() == 0)
	{
		for (int32 Index = 0; Index < static_cast<int32>(UE_ARRAY_COUNT(GF9RoundStart)); ++Index)
		{
			Rounds.Add({Index + 1, GF9RoundStart[Index], GF9RoundEnd[Index]});
		}
		RoundsSource = TEXT("built-in F9 table");
	}
	UE_LOG(LogHUMALive, Log, TEXT("Rounds: %d rounds from %s"), Rounds.Num(), *RoundsSource);
}

FString UHUMALiveHawkEyeComponent::DescribeRoundTime(double Seconds) const
{
	for (int32 Index = 0; Index < Rounds.Num(); ++Index)
	{
		const FRoundSpan& R = Rounds[Index];
		if (Seconds >= R.StartSec && Seconds <= R.EndSec)
		{
			const double In = Seconds - R.StartSec;
			return FString::Printf(TEXT(" (R%d %d:%05.2f)"), R.Round, static_cast<int32>(In / 60.0), FMath::Fmod(In, 60.0));
		}
		if (Seconds < R.StartSec)
		{
			return Index == 0 ? FString(TEXT(" (before R1)")) : FString::Printf(TEXT(" (rest before R%d)"), R.Round);
		}
	}
	return Rounds.Num() > 0 ? FString(TEXT(" (after last round)")) : FString();
}

bool UHUMALiveHawkEyeComponent::SeekToTime(float Seconds, bool bPlay)
{
	if (!bRunning || !bTimeline || !Playback)
	{
		Notify(TEXT("HUMA Live: seek needs the live timeline (Play the level, Watch For New Files off)."), FColor::Red);
		return false;
	}
	const double Clamped = FMath::Clamp(static_cast<double>(Seconds), 0.0, static_cast<double>(LiveLastUs - LiveFirstUs) * 1.0e-6);
	Playback->CurrentTime = Clamped;  // TickTimeline が書き換えを検出して読み直す
	Playback->bPlaying = bPlay;
	return true;
}

bool UHUMALiveHawkEyeComponent::SeekToRound(int32 Round, float SecondsInRound, bool bPlay)
{
	const FRoundSpan* Span = Rounds.FindByPredicate([Round](const FRoundSpan& R) { return R.Round == Round; });
	if (!Span)
	{
		Notify(FString::Printf(TEXT("HUMA Live: round %d is not in the round table (%s)."), Round, *RoundsSource), FColor::Red);
		return false;
	}
	const double Target = Span->StartSec + SecondsInRound;
	if (!SeekToTime(static_cast<float>(Target), bPlay)) { return false; }
	Notify(FString::Printf(TEXT("HUMA Live: jump to R%d %d:%05.2f (%.2f s)"), Round, static_cast<int32>(SecondsInRound / 60.f), FMath::Fmod(SecondsInRound, 60.f), Target));
	return true;
}

bool UHUMALiveHawkEyeComponent::JumpToScene(int32 SceneId)
{
	for (const FScenePreset& Scene : GScenePresets)
	{
		if (Scene.Id != SceneId) { continue; }
		const FRoundSpan* Span = Rounds.FindByPredicate([&Scene](const FRoundSpan& R) { return R.Round == Scene.Round; });
		if (!Span) { Notify(FString::Printf(TEXT("HUMA Live: round %d is not in the round table."), Scene.Round), FColor::Red); return false; }
		const double Target = Span->StartSec + Scene.SecondsInRound - ScenePrerollSeconds;
		if (!SeekToTime(static_cast<float>(Target), true)) { return false; }
		Notify(FString::Printf(TEXT("Scene %d: %s | R%d %d:%05.2f (%.2f s), from %.1f s before"), Scene.Id, Scene.Label, Scene.Round,
			static_cast<int32>(Scene.SecondsInRound / 60.0), FMath::Fmod(Scene.SecondsInRound, 60.0), Span->StartSec + Scene.SecondsInRound, ScenePrerollSeconds), FColor::Green);
		return true;
	}
	Notify(FString::Printf(TEXT("HUMA Live: unknown scene %d (ke * LiveScenes for the list)."), SceneId), FColor::Red);
	return false;
}

void UHUMALiveHawkEyeComponent::ListScenes() const
{
	for (int32 Index = static_cast<int32>(UE_ARRAY_COUNT(GScenePresets)) - 1; Index >= 0; --Index)  // 画面では新しいものが上に出るので逆順
	{
		const FScenePreset& Scene = GScenePresets[Index];
		Notify(FString::Printf(TEXT("  %d : R%d %d:%05.2f  %s"), Scene.Id, Scene.Round, static_cast<int32>(Scene.SecondsInRound / 60.0), FMath::Fmod(Scene.SecondsInRound, 60.0), Scene.Label), FColor::White);
	}
	Notify(TEXT("Scenes (ke * LiveScene <id>):"), FColor::Cyan);
}

// ---------------------------------------------------------------- 置くだけのアクター
AHUMALiveHawkEyeActor::AHUMALiveHawkEyeActor()
{
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Scene")));
	Live = CreateDefaultSubobject<UHUMALiveHawkEyeComponent>(TEXT("Live"));
}

void AHUMALiveHawkEyeActor::LiveToggle() { Live->ToggleLive(); }
void AHUMALiveHawkEyeActor::LiveOn() { Live->StartLive(); }
void AHUMALiveHawkEyeActor::LiveOff() { Live->StopLive(); }
void AHUMALiveHawkEyeActor::LiveRestart()
{
	Live->StopLive();
	Live->StartLive();
}
void AHUMALiveHawkEyeActor::LiveSeek(float Seconds) { Live->SeekToTime(Seconds, true); }
void AHUMALiveHawkEyeActor::LiveRound(int32 Round, float SecondsInRound) { Live->SeekToRound(Round, SecondsInRound, true); }
void AHUMALiveHawkEyeActor::LiveScene(int32 SceneId) { Live->JumpToScene(SceneId); }
void AHUMALiveHawkEyeActor::LiveScenes() { Live->ListScenes(); }
