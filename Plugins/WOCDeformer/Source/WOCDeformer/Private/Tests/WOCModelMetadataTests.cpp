#include "WOCBasisAsset.h"
#include "WOCDeformerCore.h"
#include "NNEModelData.h"
#include "Misc/AutomationTest.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWOCModelMetadataTest, "WOC.Metadata.CookPersistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWOCModelMetadataTest::RunTest(const FString&)
{
	auto* Model = NewObject<UNNEModelData>();
	TArray<uint8> Bytes;
	auto Add = [&](const ANSICHAR* Key, const ANSICHAR* Value)
	{
		const int32 KeyLength = FCStringAnsi::Strlen(Key), ValueLength = FCStringAnsi::Strlen(Value);
		Bytes.Append({114, uint8(4 + KeyLength + ValueLength), 10, uint8(KeyLength)});
		Bytes.Append(reinterpret_cast<const uint8*>(Key), KeyLength);
		Bytes.Append({18, uint8(ValueLength)});
		Bytes.Append(reinterpret_cast<const uint8*>(Value), ValueLength);
	};
	Add("joint_names", "hip,spine");
	Add("max_joints", "2");
	const auto LegacyBytes = Bytes;
	Add("num_coeffs_per_joint", "12");
	Model->Init(TEXT("onnx"), Bytes);
	auto* Basis = NewObject<UWOCBasisAsset>();
	FWOCBasisLOD& LOD = Basis->LODs.AddDefaulted_GetRef();
	LOD.NumJoints = 2;
	LOD.NumCoeffs = 12;
	auto* LegacyModel = NewObject<UNNEModelData>();
	LegacyModel->Init(TEXT("onnx"), LegacyBytes);
	AddExpectedError(TEXT("WOC legacy ONNX omits num_coeffs_per_joint"), EAutomationExpectedErrorFlags::Contains, 1);
	TestTrue(TEXT("SDK v1 legacy coefficient convention remains supported"), Basis->CacheModelMetadata(LegacyModel));
	TestTrue(TEXT("Metadata captured without inference"), Basis->CacheModelMetadata(Model));
	TestEqual(TEXT("Separate records for separate models"), Basis->ModelMetadata.Num(), 2);
	TestTrue(TEXT("Repeat capture is idempotent"), Basis->CacheModelMetadata(Model) && Basis->ModelMetadata.Num() == 2);
	auto* Reloaded = DuplicateObject<UWOCBasisAsset>(Basis, GetTransientPackage());
	Model->ClearFileDataAndFileType();
	TArray<FName> Names;
	int32 Coefficients = 0;
	FString Error;
	TestTrue(TEXT("Serialized metadata works with stripped raw ONNX"), Reloaded->ResolveModelMetadata(Model, Names, Coefficients, Error));
	TestEqual(TEXT("Model joint count retained"), Names.Num(), 2);
	TestEqual(TEXT("Model coefficient count retained"), Coefficients, 12);
	TestFalse(TEXT("Another model cannot use an unrelated record"),
		Reloaded->ResolveModelMetadata(NewObject<UNNEModelData>(), Names, Coefficients, Error));
	Reloaded->ModelMetadata[1].JointNames[1] = TEXT("hip");
	TestFalse(TEXT("Duplicate metadata joints reject"), Reloaded->ResolveModelMetadata(Model, Names, Coefficients, Error));
	Reloaded->ModelMetadata[1].JointNames[1] = TEXT("spine");
	Reloaded->LODs[0].NumJoints = 3;
	TestFalse(TEXT("Basis dimension mismatch rejects"), Reloaded->ResolveModelMetadata(Model, Names, Coefficients, Error));
	return true;
}
#endif
