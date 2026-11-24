#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "HUMAIdentity.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMAIdentityBetaInitializationTest, "HUMA.Identity.EmptyBetaInitialization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHUMAIdentityBetaInitializationTest::RunTest(const FString&)
{
	UHUMAIdentityData* Data = NewObject<UHUMAIdentityData>();
	Data->BetaNames = {TEXT("beta_0"), TEXT("beta_1"), TEXT("beta_2")};
	UHUMAIdentityComponent* Identity = NewObject<UHUMAIdentityComponent>();
	TestTrue(TEXT("New component beta array is initially empty"), Identity->Betas.IsEmpty());
	TestNull(TEXT("No mesh is required to initialize editor defaults"), Identity->TargetMesh.Get());
	Identity->IdentityData = Data;
	FPropertyChangedEvent Event(FindFProperty<FProperty>(UHUMAIdentityComponent::StaticClass(), TEXT("IdentityData")));
	Identity->PostEditChangeProperty(Event);
	if (!TestEqual(TEXT("Selecting regressor data creates every beta"), Identity->Betas.Num(), 3)) { return false; }
	TestTrue(TEXT("Initial values are neutral"), Identity->Betas[0] == 0 && Identity->Betas[1] == 0 && Identity->Betas[2] == 0);
	TestTrue(TEXT("Individual beta can be changed"), Identity->SetBeta(1, -2.5f));
	Identity->PostEditChangeProperty(Event);
	TestEqual(TEXT("Editor changes preserve existing beta values"), Identity->Betas[1], -2.5f);
	Identity->ResetIdentity();
	TestTrue(TEXT("Reset fills every entry with zero"), Identity->Betas.Num() == 3 && Identity->Betas[1] == 0);
	Identity->Betas.Reset();
	TestTrue(TEXT("SetBeta initializes an empty array without manual resizing"), Identity->SetBeta(2, 4.0f));
	TestEqual(TEXT("SetBeta initialization uses model dimensions"), Identity->Betas.Num(), 3);
	TestEqual(TEXT("New value is applied to the requested index"), Identity->Betas[2], 4.0f);
	return true;
}

#endif
