#if WITH_DEV_AUTOMATION_TESTS

#include "HUMARuntimeData.h"
#include "HUMAIdentity.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMARuntimeSamplingTest, "HUMA.Runtime.Sampling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHUMARuntimeSamplingTest::RunTest(const FString&)
{
	FHUMARuntimeTrack Track;
	Track.FPS = 30;
	Track.FrameCount = 3;
	Track.JointCount = 1;
	Track.ValidRanges = {FIntPoint(0, 3)};
	Track.Rotations = {
		FQuat4f::Identity,
		FQuat4f(FVector3f(0, 0, 1), PI / 2),
		-FQuat4f(FVector3f(0, 0, 1), PI)
	};
	Track.HipDisplacements = {FVector3f(0, 0, 10), FVector3f(10, 20, 30), FVector3f(20, 40, 50)};
	FHUMARuntimeSample Sample;
	TestTrue(TEXT("Sample first frame"), Track.Sample(0, Sample));
	TestTrue(TEXT("First displacement"), Sample.HipDisplacement.Equals(FVector(0, 0, 10), 1.e-6));
	TestTrue(TEXT("Half-frame sample"), Track.Sample(0.5 / 30, Sample));
	TestTrue(TEXT("Linear hip interpolation"), Sample.HipDisplacement.Equals(FVector(5, 10, 20), 1.e-6));
	TestTrue(TEXT("Quaternion slerp"), Sample.LocalRotations[0].Equals(FQuat(FVector::UpVector, PI / 4), 1.e-6));
	TestTrue(TEXT("Reverse seek to frame zero"), Track.Sample(0, Sample));
	TestTrue(TEXT("Reverse seek rotation"), Sample.LocalRotations[0].Equals(FQuat::Identity, 1.e-6));
	TestTrue(TEXT("Hold last sample at exclusive-end control position"), Track.Sample(3.0 / 30, Sample));
	TestTrue(TEXT("Final displacement"), Sample.HipDisplacement.Equals(FVector(20, 40, 50), 1.e-6));
	TestFalse(TEXT("Out-of-range track time is not visible"), Track.Sample(4.0 / 30, Sample));
	Track.ValidRanges = {FIntPoint(0, 1), FIntPoint(2, 3)};
	TestFalse(TEXT("Missing source frame is not visible"), Track.Sample(1.0 / 30, Sample));
	TestTrue(TEXT("A valid sample before a gap remains visible"), Track.Sample(0.5 / 30, Sample));
	TestTrue(TEXT("Never interpolate across gaps"), Sample.HipDisplacement.Equals(FVector(0, 0, 10), 1.e-6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMARuntimeDatasetTest, "HUMA.Runtime.DatasetValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHUMARuntimeDatasetTest::RunTest(const FString&)
{
	UHUMAIdentityData* Data = LoadObject<UHUMAIdentityData>(nullptr, TEXT("/Game/HUMAIdentity/DA_HUMAIdentity_Male.DA_HUMAIdentity_Male"));
	if (!TestNotNull(TEXT("Sample identity data is available"), Data)) { return false; }
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("retarget_runtime"));
	FHUMARuntimeDataset Dataset;
	FString Error;
	if (!TestTrue(TEXT("Load saved 30 fps runtime dataset"), LoadHUMARuntimeDataset(Root / TEXT("manifest.json"),
		Data, TEXT("HUMA_FULLBODY_v1.0.0_MALE_LOD0"), Dataset, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("All source players"), Dataset.Tracks.Num(), 15);
	TestEqual(TEXT("All model joints"), Dataset.JointToMeshBone.Num(), 67);
	TestTrue(TEXT("Duration from input fps"), FMath::Abs(Dataset.Duration - 413.0 / 30) < 1.e-10);
	for (const FHUMARuntimeTrack& Track : Dataset.Tracks)
	{
		TestEqual(TEXT("Source fps retained"), Track.FPS, 30.0);
		TestEqual(TEXT("Identity beta count"), Track.Betas.Num(), 576);
	}
	TestFalse(TEXT("Wrong model rejected"), LoadHUMARuntimeDataset(Root / TEXT("manifest.json"), Data, TEXT("WRONG_MODEL"), Dataset, Error));

	const FString Temporary = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()
		/ TEXT("Automation") / (TEXT("HUMARuntime_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	IFileManager::Get().MakeDirectory(*Temporary, true);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Temporary, false, true); };
	auto LoadJson = [this](const FString& Path)
	{
		FString Text;
		TSharedPtr<FJsonObject> Object;
		if (!FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object))
		{
			AddError(TEXT("Cannot read JSON test fixture: ") + Path);
		}
		return Object;
	};
	auto SaveJson = [](const FString& Path, const TSharedPtr<FJsonObject>& Object)
	{
		FString Text;
		return FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<>::Create(&Text)) && FFileHelper::SaveStringToFile(Text, *Path);
	};
	TSharedPtr<FJsonObject> Manifest = LoadJson(Root / TEXT("manifest.json"));
	if (!Manifest) { return false; }
	TArray<TSharedPtr<FJsonValue>> Entries = Manifest->GetArrayField(TEXT("tracks"));
	Entries.SetNum(1);
	Manifest->SetArrayField(TEXT("tracks"), Entries);
	Manifest->SetNumberField(TEXT("track_count"), 1);
	const TSharedPtr<FJsonObject> Entry = Entries[0]->AsObject();
	const FString MotionRelative = Entry->GetStringField(TEXT("motion"));
	const FString IdentityRelative = Entry->GetStringField(TEXT("identity"));
	for (const FString& Relative : {
		Manifest->GetStringField(TEXT("skeleton")), Manifest->GetStringField(TEXT("beta_layout")),
		MotionRelative, IdentityRelative, FPaths::GetPath(MotionRelative) / TEXT("motion.bin")})
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Temporary / Relative), true);
		if (!TestEqual(TEXT("Copy one-track fixture"), IFileManager::Get().Copy(*(Temporary / Relative), *(Root / Relative)), COPY_OK)) { return false; }
	}
	const FString ManifestPath = Temporary / TEXT("manifest.json");
	if (!TestTrue(TEXT("Write fixture manifest"), SaveJson(ManifestPath, Manifest))) { return false; }
	TestTrue(TEXT("Valid independent fixture"), LoadHUMARuntimeDataset(ManifestPath, Data, TEXT("HUMA_FULLBODY_v1.0.0_MALE_LOD0"), Dataset, Error));
	TSharedPtr<FJsonObject> Motion = LoadJson(Temporary / MotionRelative);
	if (!Motion) { return false; }
	Motion->GetObjectField(TEXT("payload"))->SetStringField(TEXT("sha256"), FString::ChrN(64, TEXT('0')));
	if (!SaveJson(Temporary / MotionRelative, Motion)) { return false; }
	TestFalse(TEXT("Corrupt checksum rejected"), LoadHUMARuntimeDataset(ManifestPath, Data, TEXT("HUMA_FULLBODY_v1.0.0_MALE_LOD0"), Dataset, Error));
	TestTrue(TEXT("Checksum error is explicit"), Error.Contains(TEXT("SHA256")));
	Motion = LoadJson(Root / MotionRelative);
	if (!Motion) { return false; }
	Motion->GetObjectField(TEXT("timing"))->SetNumberField(TEXT("fps"), 0);
	if (!SaveJson(Temporary / MotionRelative, Motion)) { return false; }
	TestFalse(TEXT("Zero fps rejected"), LoadHUMARuntimeDataset(ManifestPath, Data, TEXT("HUMA_FULLBODY_v1.0.0_MALE_LOD0"), Dataset, Error));
	TestTrue(TEXT("FPS error is explicit"), Error.Contains(TEXT("fps")));
	return true;
}

#endif
