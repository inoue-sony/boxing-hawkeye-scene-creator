#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"
#include "Containers/Queue.h"
#include "HawkEyeLiveCore.h"
#include "NNERuntimeCPU.h"

#include <atomic>
#include <map>
#include <memory>

class UNNEModelData;

/** ik_huma_causal_w9.onnx を NNE（NNERuntimeORTCpu）で実行する。ワーカースレッドからだけ使う。 */
class FHUMANneIkModel final : public hawkeye_live::IkModel
{
public:
	bool Initialize(UNNEModelData* ModelData, FString& Error);
	virtual int Window() const override { return WindowSize; }
	virtual int Joints() const override { return JointCount; }
	virtual bool Run(const float* Centered, const float* MidHip, float* Rot, float* Root, std::string& Error) override;

private:
	TSharedPtr<UE::NNE::IModelInstanceCPU> Instance;
	TSharedPtr<UE::NNE::IModelCPU> Model;
	int WindowSize = 9;
	int JointCount = 26;
};

/** ワーカーが 1 フレームごとに返す結果。時刻はループ分のずれを足した、単調に増える値（µs）。 */
struct FHUMALiveResult
{
	int64 TimeUs = 0;
	std::vector<hawkeye_live::Pose> Poses;
	FString Error;
	double IkMilliseconds = 0.0;
	int32 Generation = 0;  // シークのたびに増える。古い世代の結果は表示側が捨てる
};

struct FHUMALiveWorkerSettings
{
	FString Folder;
	bool bLoop = true;
	bool bWatch = false;
	/** true: フォルダ全体を 1 本のタイムラインとして扱い、SeekTo で任意の時刻から読み直せる（ループは表示側が SeekTo(先頭) で行う）。 */
	bool bTimeline = false;
	int32 MaxGapFrames = 15;
	std::map<int, hawkeye_live::Profile> Profiles;
};

/** フォルダのファイルを順に読み、IK まで済ませて結果をキューに積む。表示の時計より先に読みすぎないよう待つ。 */
class FHUMALiveWorker final : public FRunnable
{
public:
	FHUMALiveWorker(FHUMALiveWorkerSettings InSettings, TUniquePtr<FHUMANneIkModel> InModel);
	virtual ~FHUMALiveWorker() override;

	void Start();
	void Shutdown();

	/** フォルダ内の先頭と最後のフレームの時刻（µs）とファイル数を調べる。 */
	static bool Probe(const FString& Folder, int64& OutFirstUs, int64& OutLastUs, int32& OutCount, FString& OutError);

	/** その時刻（データ内の絶対時刻 µs）から読み直す。戻り値は新しい世代番号。 */
	int32 SeekTo(int64 TimeUs);

	/** 表示側の時計（µs、ループ分のずれ込み）。まだ始まっていなければ INT64_MIN。 */
	std::atomic<int64> ConsumerClockUs{TNumericLimits<int64>::Lowest()};
	TQueue<TSharedPtr<FHUMALiveResult>, EQueueMode::Spsc> Results;
	std::atomic<bool> bFinished{false};
	std::atomic<int32> FileCount{0};
	std::atomic<int64> SeekRequestUs{TNumericLimits<int64>::Lowest()};
	std::atomic<int32> RequestedGeneration{0};

	virtual uint32 Run() override;
	virtual void Stop() override { bStop = true; }

private:
	FHUMALiveWorkerSettings Settings;
	TUniquePtr<FHUMANneIkModel> Model;
	FRunnableThread* Thread = nullptr;
	FThreadSafeBool bStop = false;

	void Scan(TArray<FString>& Files) const;
};
