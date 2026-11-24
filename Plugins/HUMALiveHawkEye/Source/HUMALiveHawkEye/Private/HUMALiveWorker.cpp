#include "HUMALiveWorker.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NNE.h"
#include "NNEModelData.h"
#include "NNETypes.h"
#include "Algo/BinarySearch.h"
#include "HUMALiveHawkEyeComponent.h"

// ---------------------------------------------------------------- NNE で IK を実行する
bool FHUMANneIkModel::Initialize(UNNEModelData* ModelData, FString& Error)
{
	check(IsInGameThread());
	if (!ModelData) { Error = TEXT("IK model data is null."); return false; }
	const TWeakInterfacePtr<INNERuntimeCPU> Runtime = UE::NNE::GetRuntime<INNERuntimeCPU>(TEXT("NNERuntimeORTCpu"));
	if (!Runtime.IsValid())
	{
		Error = TEXT("NNE runtime 'NNERuntimeORTCpu' was not found. Enable the NNERuntimeORT plugin.");
		return false;
	}
	Model = Runtime->CreateModelCPU(ModelData);
	if (!Model.IsValid()) { Error = TEXT("NNERuntimeORTCpu could not create the IK model from the ONNX file."); return false; }
	Instance = Model->CreateModelInstanceCPU();
	if (!Instance.IsValid()) { Error = TEXT("Could not create an IK model instance."); return false; }

	const TConstArrayView<UE::NNE::FTensorDesc> Inputs = Instance->GetInputTensorDescs();
	if (Inputs.Num() != 2) { Error = FString::Printf(TEXT("The IK model has %d inputs (expected 2)."), Inputs.Num()); return false; }
	const TConstArrayView<int32> First = Inputs[0].GetShape().GetData();
	if (First.Num() != 4 || First[2] != 29 || First[3] != 3) { Error = TEXT("Unexpected IK input shape (expected [batch, T, 29, 3])."); return false; }
	WindowSize = First[1] > 0 ? First[1] : 9;

	TArray<UE::NNE::FTensorShape> Shapes;
	Shapes.Add(UE::NNE::FTensorShape::Make({1u, static_cast<uint32>(WindowSize), 29u, 3u}));
	Shapes.Add(UE::NNE::FTensorShape::Make({1u, static_cast<uint32>(WindowSize), 3u}));
	if (Instance->SetInputTensorShapes(Shapes) != UE::NNE::EResultStatus::Ok)
	{
		Error = TEXT("SetInputTensorShapes failed for the IK model.");
		return false;
	}
	const TConstArrayView<UE::NNE::FTensorShape> Outputs = Instance->GetOutputTensorShapes();
	if (Outputs.Num() == 2 && Outputs[0].Rank() == 4)
	{
		JointCount = static_cast<int>(Outputs[0].GetData()[1]);
	}
	else
	{
		// 出力の関節数が動的なとき: 1 回だけダミー入力で実行して決める必要があるが、このモデルは 26 で固定
		JointCount = 26;
	}
	return JointCount > 0;
}

bool FHUMANneIkModel::Run(const float* Centered, const float* MidHip, float* Rot, float* Root, std::string& Error)
{
	TArray<UE::NNE::FTensorBindingCPU, TInlineAllocator<2>> In, Out;
	{
		UE::NNE::FTensorBindingCPU& B = In.AddDefaulted_GetRef();
		B.Data = const_cast<float*>(Centered);
		B.SizeInBytes = static_cast<uint64>(WindowSize) * 87 * sizeof(float);
	}
	{
		UE::NNE::FTensorBindingCPU& B = In.AddDefaulted_GetRef();
		B.Data = const_cast<float*>(MidHip);
		B.SizeInBytes = static_cast<uint64>(WindowSize) * 3 * sizeof(float);
	}
	{
		UE::NNE::FTensorBindingCPU& B = Out.AddDefaulted_GetRef();
		B.Data = Rot;
		B.SizeInBytes = static_cast<uint64>(JointCount) * 9 * sizeof(float);
	}
	{
		UE::NNE::FTensorBindingCPU& B = Out.AddDefaulted_GetRef();
		B.Data = Root;
		B.SizeInBytes = 3 * sizeof(float);
	}
	if (static_cast<int32>(Instance->RunSync(In, Out)) != 0)
	{
		Error = "NNE RunSync failed";
		return false;
	}
	return true;
}

// ---------------------------------------------------------------- ワーカー
FHUMALiveWorker::FHUMALiveWorker(FHUMALiveWorkerSettings InSettings, TUniquePtr<FHUMANneIkModel> InModel)
	: Settings(MoveTemp(InSettings)), Model(MoveTemp(InModel))
{
}

FHUMALiveWorker::~FHUMALiveWorker()
{
	Shutdown();
}

void FHUMALiveWorker::Start()
{
	if (!Thread)
	{
		Thread = FRunnableThread::Create(this, TEXT("HUMALiveHawkEye"), 0, TPri_Normal);
	}
}

void FHUMALiveWorker::Shutdown()
{
	if (Thread)
	{
		Stop();
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
}

void FHUMALiveWorker::Scan(TArray<FString>& Files) const
{
	Files.Reset();
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Settings.Folder, TEXT("*.bson")), true, false);
	Files.Sort();  // ファイル名は Hawk-Eye の時刻（桁数が同じ）なので、名前順 = 時刻順
}

bool FHUMALiveWorker::Probe(const FString& Folder, int64& OutFirstUs, int64& OutLastUs, int32& OutCount, FString& OutError)
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Folder, TEXT("*.bson")), true, false);
	Files.Sort();
	OutCount = Files.Num();
	if (Files.Num() == 0) { OutError = TEXT("no .bson files in the folder"); return false; }
	auto ReadTime = [&](const FString& Name, int64& Out) -> bool
	{
		TArray<uint8> Bytes;
		hawkeye_live::Frame Frame;
		std::string Err;
		if (!FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(Folder, Name)) ||
			!hawkeye_live::ParseFrame(Bytes.GetData(), static_cast<size_t>(Bytes.Num()), Frame, Err))
		{
			OutError = Name + TEXT(": ") + (Err.empty() ? FString(TEXT("could not read the file")) : FString(UTF8_TO_TCHAR(Err.c_str())));
			return false;
		}
		Out = Frame.time_us;
		return true;
	};
	return ReadTime(Files[0], OutFirstUs) && ReadTime(Files.Last(), OutLastUs) && OutLastUs >= OutFirstUs;
}

int32 FHUMALiveWorker::SeekTo(int64 TimeUs)
{
	const int32 Generation = ++RequestedGeneration;
	SeekRequestUs = TimeUs;
	return Generation;
}

uint32 FHUMALiveWorker::Run()
{
	constexpr int64 LookaheadUs = 300000;  // 表示の時計より 0.3 秒先まで読む
	constexpr int64 StepUs = 20000;        // 50 fps
	constexpr int64 PrerollUs = 9 * StepUs;  // シーク先の手前から読んで、IK の窓（9 フレーム）を実データで満たす
	const int64 NoSeek = TNumericLimits<int64>::Lowest();
	hawkeye_live::SolverOptions Options;
	Options.max_gap_frames = Settings.MaxGapFrames;
	hawkeye_live::LiveSolver Solver(*Model, Options);

	TArray<FString> Files;
	Scan(Files);
	FileCount = Files.Num();
	UE_LOG(LogHUMALive, Log, TEXT("Worker: %d bson files in %s (window %d, joints %d)%s"), Files.Num(), *Settings.Folder, Model->Window(), Model->Joints(), Settings.bTimeline ? TEXT(" [timeline]") : TEXT(""));

	// ファイル名（Hawk-Eye の時刻 µs）からシーク先を探す。名前が時刻でないときは 50fps の順番とみなす
	TArray<int64> FileTimes;
	bool bNamesAreTimes = Files.Num() > 0;
	for (const FString& Name : Files)
	{
		const FString Stem = FPaths::GetBaseFilename(Name);
		if (Stem.IsEmpty() || !Stem.IsNumeric()) { bNamesAreTimes = false; break; }
		FileTimes.Add(FCString::Atoi64(*Stem));
	}
	int64 FirstUs = TNumericLimits<int64>::Lowest();
	int64 LastUs = TNumericLimits<int64>::Lowest();
	auto FindIndex = [&](int64 TimeUs) -> int32
	{
		if (Files.Num() == 0) { return 0; }
		if (bNamesAreTimes)
		{
			return FMath::Clamp(Algo::LowerBound(FileTimes, TimeUs), 0, Files.Num() - 1);
		}
		const int64 Base = (FirstUs == TNumericLimits<int64>::Lowest()) ? 0 : FirstUs;
		return FMath::Clamp(static_cast<int32>((TimeUs - Base) / StepUs), 0, Files.Num() - 1);
	};

	int32 Next = 0;
	int64 OffsetUs = 0;
	int64 Produced = 0;
	int32 Generation = 0;
	FString LastName;
	TArray<uint8> Bytes;

	while (!bStop)
	{
		if (Settings.bTimeline)
		{
			const int64 Request = SeekRequestUs.exchange(NoSeek);
			if (Request != NoSeek)
			{
				Generation = RequestedGeneration.load();
				Next = FindIndex(Request - PrerollUs);
				Solver.Reset();
				Produced = 0;
				bFinished = false;
				continue;
			}
		}
		if (Next >= Files.Num())
		{
			if (Settings.bTimeline)
			{
				bFinished = true;
				FPlatformProcess::Sleep(0.005f);  // 末尾まで読んだ。SeekTo を待つ
				continue;
			}
			if (Settings.bWatch)
			{
				FPlatformProcess::Sleep(0.005f);
				TArray<FString> Now;
				Scan(Now);
				Files.Reset();
				for (const FString& Name : Now)
				{
					if (LastName.IsEmpty() || Name.Compare(LastName) > 0) { Files.Add(Name); }
				}
				FileCount += Files.Num();
				Next = 0;
				continue;
			}
			if (Settings.bLoop && Files.Num() > 0 && FirstUs != TNumericLimits<int64>::Lowest())
			{
				OffsetUs += (LastUs - FirstUs) + StepUs;
				Solver.Reset();
				Next = 0;
				continue;
			}
			bFinished = true;
			break;
		}

		const FString Path = FPaths::Combine(Settings.Folder, Files[Next]);
		hawkeye_live::Frame Frame;
		std::string ParseError;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path) || !hawkeye_live::ParseFrame(Bytes.GetData(), static_cast<size_t>(Bytes.Num()), Frame, ParseError))
		{
			TSharedPtr<FHUMALiveResult> Bad = MakeShared<FHUMALiveResult>();
			Bad->Generation = Generation;
			Bad->Error = FString::Printf(TEXT("%s: %s"), *Files[Next], ParseError.empty() ? TEXT("could not read the file") : UTF8_TO_TCHAR(ParseError.c_str()));
			Results.Enqueue(Bad);
			LastName = Files[Next];
			++Next;
			continue;
		}
		if (FirstUs == TNumericLimits<int64>::Lowest()) { FirstUs = Frame.time_us; }
		LastUs = FMath::Max(LastUs, Frame.time_us);
		const int64 TimeUs = Frame.time_us + OffsetUs;

		// 表示の時計より先に読みすぎない（最初の 1 フレームは時計を始めるために先に出す）。待っている間もシークは受け付ける
		bool bSeekPending = false;
		while (!bStop && Produced > 0)
		{
			if (Settings.bTimeline && SeekRequestUs.load() != NoSeek) { bSeekPending = true; break; }
			const int64 Clock = ConsumerClockUs.load();
			if (Clock != TNumericLimits<int64>::Lowest() && TimeUs <= Clock + LookaheadUs) { break; }
			FPlatformProcess::Sleep(0.002f);
		}
		if (bStop) { break; }
		if (bSeekPending) { continue; }

		TSharedPtr<FHUMALiveResult> Result = MakeShared<FHUMALiveResult>();
		Result->TimeUs = TimeUs;
		Result->Generation = Generation;
		std::string SolveError;
		const double Start = FPlatformTime::Seconds();
		Result->Poses = Solver.Push(Frame, Settings.Profiles, SolveError);
		Result->IkMilliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;
		for (hawkeye_live::Pose& Pose : Result->Poses) { Pose.time_us = TimeUs; }
		if (!SolveError.empty()) { Result->Error = UTF8_TO_TCHAR(SolveError.c_str()); }
		Results.Enqueue(Result);
		++Produced;
		if (Produced == 1 || Produced == 20 || Produced % 250 == 0)
		{
			UE_LOG(LogHUMALive, Log, TEXT("Worker: gen %d frame %lld (%s) poses=%d ik=%.2f ms t=%.3f s"), Generation, Produced, *Files[Next], static_cast<int32>(Result->Poses.size()), Result->IkMilliseconds, TimeUs / 1.0e6);
		}
		LastName = Files[Next];
		++Next;
	}
	return 0;
}
