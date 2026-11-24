#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "HUMALiveHawkEyeComponent.generated.h"

class UHUMARuntimePlaybackComponent;
class UHUMARuntimePoseComponent;
class UNNEModelData;
class FHUMALiveWorker;
struct FHUMALiveResult;

DECLARE_LOG_CATEGORY_EXTERN(LogHUMALive, Log, All);

/**
 * Hawk-Eye のフレーム BSON をフォルダから順に読み、因果 IK で毎フレーム HUMA の姿勢を作って、
 * HUMA ランタイム再生（UHUMARuntimePlaybackComponent）が出した選手に当てる。
 *
 * - 選手の生成・体型・チームの外見は、今までどおり再生コンポーネントがデータセットから行う（体型は事前に固定）。
 * - ライブ中は再生コンポーネントを一時停止し、そのデータセットのモーションの代わりにこちらの姿勢を当てる。
 * - 時計はワールドの時間（タイムディレーション込み）で進むので、スロー再生もそのまま効く。
 */
UCLASS(ClassGroup = HUMA, meta = (BlueprintSpawnableComponent))
class HUMALIVEHAWKEYE_API UHUMALiveHawkEyeComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UHUMALiveHawkEyeComponent();

	/** Hawk-Eye のフレーム BSON（1 フレーム 1 ファイル、snappy 圧縮可）が入ったフォルダ。
	 * 相対パスはプロジェクトフォルダ基準。見つからなければ <Project>/Data/HawkEye を使う。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Live")
	FDirectoryPath FramesFolder;

	/** ik_huma_causal_w9.onnx。空ならプラグイン同梱の Plugins/HUMALiveHawkEye/Resources/ik_huma_causal_w9.onnx を使う。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Live")
	FFilePath IkModelFile;

	/** 選手を出している再生コンポーネント。空ならレベルから探す。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Live")
	TObjectPtr<UHUMARuntimePlaybackComponent> Playback;

	/** 再生コンポーネントの準備ができたら自動で始める。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Live")
	bool bAutoStart = true;

	/** フォルダの最後まで読んだら先頭に戻る（録画データでライブを模擬するとき）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Live")
	bool bLoop = true;

	/** フォルダに後から追加されるファイルを待って読む（本物のライブに近い動き）。オンのときは bLoop を使わない。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Live")
	bool bWatchForNewFiles = false;

	/** 受信の揺れを吸収するため、届いたデータからこの秒数だけ遅らせて表示する。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Live", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float DelaySeconds = 0.1f;

	/** これより長く届かない選手は隠す（秒）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Live", meta = (ClampMin = "0.02"))
	float HideAfterSeconds = 0.3f;

	/** これより長く（フレーム数）届かなかったら IK の窓をリセットする。npb_huma_ik の既定と同じ 15。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Live", meta = (ClampMin = "1"))
	int32 MaxGapFrames = 15;

	/** 画面左上に状態を表示する。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Live")
	bool bShowStatus = true;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA|Live")
	bool bRunning = false;

	float LogAccumulator = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA|Live")
	int32 FramesProcessed = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA|Live")
	float AverageIkMilliseconds = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "HUMA|Live")
	FString LastError;

	UFUNCTION(BlueprintCallable, Category = "HUMA|Live")
	bool StartLive();

	/** ライブを止め、再生コンポーネントの元のモーションに戻す。 */
	UFUNCTION(BlueprintCallable, Category = "HUMA|Live")
	void StopLive() { StopLiveInternal(true); }

	UFUNCTION(BlueprintCallable, Category = "HUMA|Live")
	void ToggleLive();

	UFUNCTION(BlueprintPure, Category = "HUMA|Live")
	FString GetStatusText() const;

	/** ラウンドの時刻表（rounds.json）。空なら Frames Folder の rounds.json を探す。見つからなければ F9 の値を使う。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Live", meta = (FilePathFilter = "json"))
	FFilePath RoundsFile;

	/** シーンへ飛ぶとき、指定時刻の何秒前から見せるか。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Live", meta = (ClampMin = "0.0"))
	float ScenePrerollSeconds = 1.0f;

	/** 再生バー上の秒（フォルダ先頭からの経過）へ飛ぶ。タイムラインモードのときだけ有効。 */
	UFUNCTION(BlueprintCallable, Category = "HUMA|Live|Debug")
	bool SeekToTime(float Seconds, bool bPlay = true);

	/** ラウンド番号とラウンド内の経過秒で飛ぶ。 */
	UFUNCTION(BlueprintCallable, Category = "HUMA|Live|Debug")
	bool SeekToRound(int32 Round, float SecondsInRound, bool bPlay = true);

	/** 登録済みのシーン（自由視点コンテンツ案の候補）へ飛ぶ。Id は 11, 12, 21, 31, 32, 33, 41, 42。 */
	UFUNCTION(BlueprintCallable, Category = "HUMA|Live|Debug")
	bool JumpToScene(int32 SceneId);

	/** 登録済みのシーンの一覧を画面とログに出す。 */
	UFUNCTION(BlueprintCallable, Category = "HUMA|Live|Debug")
	void ListScenes() const;

	/**
	 * Sequencer から再生位置を指定する（再生バー上の秒）。0 以上のあいだは、この値が表示の時刻になる
	 * （再生・一時停止・スロー再生はこの値のカーブで表す）。負の値で通常の再生に戻る。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, Category = "HUMA|Live|Sequence")
	float SequenceDataTime = -1.f;

	/** この trackId（体型データセット）の選手を、手袋・服ごと隠す。0 なら隠さない（目線カメラ用）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Live|Sequence")
	int32 HiddenTrackId = 0;

	/** SequenceDataTime を使うとき、その時刻の姿勢が届くまでゲームスレッドで待つ（動画書き出し用。最大 3 秒）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Live|Sequence")
	bool bWaitForPosesInSequence = true;

	/** 体型データセットの trackId の選手について、今の表示時刻の Hawk-Eye の重心（ワールド座標、cm）を返す。 */
	UFUNCTION(BlueprintPure, Category = "HUMA|Live")
	bool GetTrackCenterOfMass(int32 TrackId, FVector& OutLocation) const;

	/** 今の表示時刻（再生バー上の秒）。 */
	UFUNCTION(BlueprintPure, Category = "HUMA|Live")
	double GetDisplayTimeSeconds() const;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction) override;

private:
	struct FTimedPose
	{
		int64 TimeUs = 0;
		TArray<FQuat> Rotations;  // 67、UE コンポーネント空間の親ローカル
		FVector Hip = FVector::ZeroVector;
		FVector Com = FVector::ZeroVector;  // Hawk-Eye の重心（UE ワールド、cm）
		bool bHasCom = false;
	};

	UPROPERTY(Transient)
	TObjectPtr<UNNEModelData> ModelData;

	TSharedPtr<FHUMALiveWorker> Worker;
	TMap<int32, TArray<FTimedPose>> Buffers;
	TArray<int32> JointToMeshBone;
	FQuat MeshRootRotation = FQuat::Identity;
	TArray<FQuat> RefRotations;
	TSet<TWeakObjectPtr<UHUMARuntimePoseComponent>> Prerequisites;
	double ClockUs = 0.0;
	bool bClockStarted = false;
	int64 NewestUs = 0;
	bool bTriedAutoStart = false;
	bool bResumePlaybackOnStop = false;
	FString LiveManifest;  // ライブを始めたときのデータセット。シーンが切り替わったら止める

	// タイムライン（フォルダ全体を再生バーでシークできるモード。bWatchForNewFiles がオフのとき）
	bool bTimeline = false;
	int64 LiveFirstUs = 0;
	int64 LiveLastUs = 0;
	double OriginalDuration = 0.0;
	double LastWrittenTime = 0.0;
	double HeldClockUs = 0.0;     // シーク直後、新しい姿勢が届くまで表示に使う元の時刻
	int32 ExpectedGeneration = 0;
	bool bBuffering = false;
	bool bPendingReset = false;
	double SinceSeekSeconds = 1.0;

	// ラウンドの開始・終了（再生バー上の秒）
	struct FRoundSpan { int32 Round = 0; double StartSec = 0.0; double EndSec = 0.0; };
	TArray<FRoundSpan> Rounds;
	FString RoundsSource;
	void LoadRounds();
	FString DescribeRoundTime(double Seconds) const;
	void Notify(const FString& Message, const FColor& Color = FColor::Yellow) const;

	bool bSequenceDriven = false;
	bool TickSequenceDriven();

	void TickTimeline(float DeltaTime);
	void SeekLive(double TimeSeconds);
	void StopLiveInternal(bool bRestorePlayback);
	bool Fail(const FString& Message);
	UHUMARuntimePlaybackComponent* ResolvePlayback();
	FString ResolveModelPath() const;
	FString ResolveFramesFolder() const;
	void DrainResults();
	void ApplyPoses(double Clock);
	void SetPlayerHidden(AActor* Player, bool bHidden);
	TMap<TWeakObjectPtr<AActor>, bool> HiddenState;
	double LastHideScanSeconds = -1.0;
	bool SampleTrack(int32 TrackId, double Clock, TArray<FQuat>& OutRotations, FVector& OutHip) const;
};

/** レベルに置くだけで動くアクター。ke * LiveToggle / LiveOn / LiveOff / LiveRestart で切り替えられる。 */
UCLASS(Blueprintable)
class HUMALIVEHAWKEYE_API AHUMALiveHawkEyeActor : public AActor
{
	GENERATED_BODY()
public:
	AHUMALiveHawkEyeActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HUMA|Live")
	TObjectPtr<UHUMALiveHawkEyeComponent> Live;

	/**
	 * Sequencer で再生位置を指定する（再生バー上の秒）。負の値なら通常の再生。
	 * Sequencer はコンポーネントの中のプロパティにキーを打てないので、アクター側に置いて Live へ渡す。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, Category = "HUMA|Live|Sequence")
	float SequenceDataTime = -1.f;

	/** この trackId の選手を手袋・服ごと隠す（0 = 隠さない）。目線カメラのシーンで、カメラ側の選手を消すのに使う。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, Category = "HUMA|Live|Sequence")
	int32 HiddenTrackId = 0;

	UFUNCTION(BlueprintCallable, Category = "HUMA|Live")
	void LiveToggle();

	UFUNCTION(BlueprintCallable, Category = "HUMA|Live")
	void LiveOn();

	UFUNCTION(BlueprintCallable, Category = "HUMA|Live")
	void LiveOff();

	UFUNCTION(BlueprintCallable, Category = "HUMA|Live")
	void LiveRestart();

	/** ke * LiveSeek 370.7 … 再生バー上の秒へ飛ぶ */
	UFUNCTION(BlueprintCallable, Category = "HUMA|Live|Debug")
	void LiveSeek(float Seconds);

	/** ke * LiveRound 4 141.97 … ラウンド 4 の 2:21.97 へ飛ぶ */
	UFUNCTION(BlueprintCallable, Category = "HUMA|Live|Debug")
	void LiveRound(int32 Round, float SecondsInRound);

	/** ke * LiveScene 31 … 登録済みのシーンへ飛ぶ（ScenePrerollSeconds 前から再生） */
	UFUNCTION(BlueprintCallable, Category = "HUMA|Live|Debug")
	void LiveScene(int32 SceneId);

	/** ke * LiveScenes … シーンの一覧を表示 */
	UFUNCTION(BlueprintCallable, Category = "HUMA|Live|Debug")
	void LiveScenes();
};
