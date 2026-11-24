#include "HUMAClothBindBatchBaker.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMAClothBatchPlanTest, "HUMAClothBind.EditorBatch.Plan",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHUMAClothBatchPlanTest::RunTest(const FString&)
{
    TStrongObjectPtr<UHUMAClothBindAsset> Settings(NewObject<UHUMAClothBindAsset>());
    Settings->BodyMesh = NewObject<USkeletalMesh>();
    TStrongObjectPtr<UStaticMesh> First(NewObject<UStaticMesh>(GetTransientPackage(), TEXT("BatchFirst")));
    TStrongObjectPtr<UStaticMesh> Second(NewObject<UStaticMesh>(GetTransientPackage(), TEXT("BatchSecond")));
    const FString Folder = TEXT("/Game/__HUMABatchPlanValidation__/Run_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const TArray<UObject*> Selection = {First.Get(), Second.Get(), First.Get(), Settings.Get(), Settings->BodyMesh.Get()};
    FHUMAClothBatchPlan Plan;
    FString Error;
    TestTrue(TEXT("Plan only needs reference settings and selected meshes"), PlanHUMAClothBatch(*Settings, Selection, Folder, Plan, Error));
    TestEqual(TEXT("Non-mesh, body and duplicate selections are ignored"), Plan.Items.Num(), 2);
    const float Sigma = Settings->SigmaMin;
    Settings->SigmaMin += 1;
    TestEqual(TEXT("Common settings are snapshotted"), Plan.Settings->SigmaMin, Sigma);
    TestFalse(TEXT("Read-only mount rejected"), PlanHUMAClothBatch(*Settings, Selection, TEXT("/Engine/Bindings"), Plan, Error));
    TestTrue(TEXT("Failed planning cannot leave executable partial work"), Plan.Items.IsEmpty() && !Plan.Settings);
    auto* A = NewObject<UStaticMesh>(CreatePackage(*(Folder + TEXT("/A"))), TEXT("SameName"), RF_Transient);
    auto* B = NewObject<UStaticMesh>(CreatePackage(*(Folder + TEXT("/B"))), TEXT("SameName"), RF_Transient);
    const TArray<UObject*> Duplicates = {A, B};
    TestFalse(TEXT("Duplicate output names rejected"), PlanHUMAClothBatch(*Settings, Duplicates, Folder, Plan, Error));
    TestTrue(TEXT("Duplicate-name diagnostic is explicit"), Error.Contains(TEXT("duplicate names")));
    auto* C = NewObject<UStaticMesh>(CreatePackage(*(Folder + TEXT("/C"))), TEXT("samename"), RF_Transient);
    const TArray<UObject*> CaseCollision = {A, C};
    TestFalse(TEXT("Case-insensitive package name collisions rejected"), PlanHUMAClothBatch(*Settings, CaseCollision, Folder, Plan, Error));
    const TArray<UObject*> One = {First.Get()};
    const FString ExistingFolder = Folder + TEXT("/Existing");
    const FString Name = TEXT("CB_") + First->GetName();
    TStrongObjectPtr<UHUMAClothBindAsset> Existing(NewObject<UHUMAClothBindAsset>(
        CreatePackage(*(ExistingFolder / Name)), *Name, RF_Public | RF_Standalone));
    Existing->BodyMesh = Settings->BodyMesh;
    Existing->GarmentMesh = Second.Get();
    TestFalse(TEXT("Another garment's output is protected"), PlanHUMAClothBatch(*Settings, One, ExistingFolder, Plan, Error));
    Existing->GarmentMesh = First.Get();
    TestTrue(TEXT("Matching existing output can be planned"), PlanHUMAClothBatch(*Settings, One, ExistingFolder, Plan, Error));
    TestEqual(TEXT("Existing outputs require confirmation"), Plan.ExistingCount, 1);
    Existing->GarmentMesh = Second.Get();
    AddExpectedError(TEXT("Output changed after planning"), EAutomationExpectedErrorFlags::Contains, 1);
    const auto Changed = BakeHUMAClothBatch(Plan, true);
    TestTrue(TEXT("A stale plan cannot overwrite another garment"), Changed.SavedAssets.IsEmpty() && Changed.Errors.Num() == 1);
    TestTrue(TEXT("Changed output is untouched"), Existing->GarmentMesh == Second.Get());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMAClothBatchSampleTest, "HUMAClothBind.EditorBatch.SampleAssets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHUMAClothBatchSampleTest::RunTest(const FString&)
{
    auto* Source = LoadObject<UHUMAClothBindAsset>(nullptr, TEXT("/Game/HUMAClothBind/Bindings/CB_Male_buttons"));
    auto* Tee = LoadObject<UHUMAClothBindAsset>(nullptr, TEXT("/Game/HUMAClothBind/Bindings/CB_Male_tee"));
    if (!TestNotNull(TEXT("Sample buttons"), Source) || !TestNotNull(TEXT("Sample tee"), Tee)) return false;
    TStrongObjectPtr<UHUMAClothBindAsset> Settings(NewObject<UHUMAClothBindAsset>());
    Settings->BodyMesh = Source->BodyMesh;
    Settings->Gender = Source->Gender;
    Settings->GarmentToBody = Source->GarmentToBody;
    Settings->bBakeAllBodyLODs = false;
    Settings->BakeMethod = EHUMAClothBindMethod::ClothGraph;
    Settings->SigmaMin = 2.25f;
    Settings->DistanceScale = 0.75f;
    Settings->GarmentToBody.AddToTranslation(FVector(0.1, 0.2, 0.3));
    const FString TemplateSignature = Settings->BakeSignature;
    UWorld* Level = GEditor->GetEditorWorldContext().World();
    auto CountActors = [Level]
    {
        int32 Count = 0;
        for (TActorIterator<AActor> It(Level); It; ++It) if (IsValid(*It)) ++Count;
        return Count;
    };
    const int32 ActorsBefore = CountActors();
    const bool bLevelDirty = Level->GetPackage()->IsDirty();
    const FString Folder = TEXT("/Game/__HUMAAssetBatchValidation__/Run_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const TArray<UObject*> Selection = {Source->GarmentMesh.Get(), Tee->GarmentMesh.Get(), Source->GarmentMesh.Get(), Source->BodyMesh.Get()};
    FHUMAClothBatchPlan Plan;
    FString Error;
    if (!TestTrue(TEXT("Asset-only batch plan"), PlanHUMAClothBatch(*Settings, Selection, Folder, Plan, Error)))
    { AddError(Error); return false; }
    const auto Graph = BakeHUMAClothBatch(Plan, false);
    if (!TestTrue(TEXT("Two graph assets saved"), Graph.IsSuccess() && Graph.SavedAssets.Num() == 2))
    { for (const FString& Failure : Graph.Errors) AddError(Failure); return false; }
    for (const auto* Asset : Graph.SavedAssets)
    {
        TestEqual(TEXT("Graph method inherited"), Asset->BindMethod, FString(TEXT("cloth_graph")));
        TestEqual(TEXT("Sigma inherited"), Asset->SigmaMin, Settings->SigmaMin);
        TestEqual(TEXT("Distance scale inherited"), Asset->DistanceScale, Settings->DistanceScale);
        TestTrue(TEXT("Transform inherited"), Asset->GarmentToBody.Equals(Settings->GarmentToBody));
        TestTrue(TEXT("Body/gender inherited"), Asset->BodyMesh == Settings->BodyMesh && Asset->Gender == Settings->Gender);
        TestTrue(TEXT("LOD choice inherited"), !Asset->bBakeAllBodyLODs && Asset->AdditionalBodyLODs.IsEmpty());
        TestTrue(TEXT("Current serialized tables"), Asset->IsBakeCurrent(Error));
        TestTrue(TEXT("Saved package exists"), IFileManager::Get().FileExists(*FPackageName::LongPackageNameToFilename(
            Asset->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension())));
    }
    Settings->BakeMethod = EHUMAClothBindMethod::BodyField;
    if (!TestTrue(TEXT("Re-plan existing outputs"), PlanHUMAClothBatch(*Settings, Selection, Folder, Plan, Error))) return false;
    AddExpectedError(TEXT("explicit overwrite confirmation"), EAutomationExpectedErrorFlags::Contains, 1);
    const auto Denied = BakeHUMAClothBatch(Plan, false);
    TestTrue(TEXT("No overwrite without approval"), Denied.SavedAssets.IsEmpty() && Denied.Errors.Num() == 1);
    for (const auto* Asset : Graph.SavedAssets)
        TestEqual(TEXT("Denied overwrite retains old method"), Asset->BindMethod, FString(TEXT("cloth_graph")));
    const auto Field = BakeHUMAClothBatch(Plan, true);
    TestTrue(TEXT("Approved overwrite saves both"), Field.IsSuccess() && Field.SavedAssets == Graph.SavedAssets);
    for (const auto* Asset : Field.SavedAssets)
        TestEqual(TEXT("Changed common method applied"), Asset->BindMethod, FString(TEXT("body_field")));
    Settings->bBakeAllBodyLODs = true;
    const TArray<UObject*> ButtonsOnly = {Source->GarmentMesh.Get()};
    if (!TestTrue(TEXT("Plan all body LODs"), PlanHUMAClothBatch(*Settings, ButtonsOnly, Folder + TEXT("/AllLODs"), Plan, Error))) return false;
    const auto AllLODs = BakeHUMAClothBatch(Plan, false);
    if (TestTrue(TEXT("All-LOD output saved"), AllLODs.IsSuccess() && AllLODs.SavedAssets.Num() == 1))
    {
        TestTrue(TEXT("All-LOD setting inherited"), AllLODs.SavedAssets[0]->bBakeAllBodyLODs);
        TestEqual(TEXT("Every body LOD serialized"), AllLODs.SavedAssets[0]->AdditionalBodyLODs.Num() + 1, Source->BodyMesh->GetLODNum());
        TestTrue(TEXT("All-LOD table is current"), AllLODs.SavedAssets[0]->IsBakeCurrent(Error));
    }
    Settings->bBakeAllBodyLODs = false;
    TStrongObjectPtr<UStaticMesh> Empty(NewObject<UStaticMesh>(GetTransientPackage(), TEXT("EmptyBatchGarment")));
    const TArray<UObject*> Mixed = {Empty.Get(), Source->GarmentMesh.Get()};
    if (!TestTrue(TEXT("Plan mixed valid/invalid geometry"), PlanHUMAClothBatch(*Settings, Mixed, Folder + TEXT("/Failures"), Plan, Error))) return false;
    AddExpectedError(TEXT("HUMAClothBind batch:"), EAutomationExpectedErrorFlags::Contains, 1);
    const auto Partial = BakeHUMAClothBatch(Plan, false);
    TestTrue(TEXT("A failed garment does not stop the valid garment"),
        Partial.Requested == 2 && Partial.Errors.Num() == 1 && Partial.SavedAssets.Num() == 1 && !Partial.IsSuccess());
    TestNull(TEXT("Failed Bake creates no empty output asset"), FindObject<UObject>(nullptr,
        *((Folder + TEXT("/Failures/CB_EmptyBatchGarment.CB_EmptyBatchGarment")))));
    if (!TestTrue(TEXT("Plan cancellation case"), PlanHUMAClothBatch(*Settings, Selection, Folder + TEXT("/Cancel"), Plan, Error))) return false;
    int32 Polls = 0;
    const auto Cancelled = BakeHUMAClothBatch(Plan, false, [&Polls] { return ++Polls > 1; });
    TestTrue(TEXT("Cancellation preserves completed saves"),
        Cancelled.bCancelled && Cancelled.SavedAssets.Num() == 1 && Cancelled.Errors.IsEmpty() && !Cancelled.IsSuccess());
    TestNull(TEXT("Template does not need a garment or prior Bake"), Settings->GarmentMesh.Get());
    TestEqual(TEXT("Template Bake data unchanged"), Settings->BakeSignature, TemplateSignature);
    TestEqual(TEXT("No level actors created"), CountActors(), ActorsBefore);
    TestEqual(TEXT("Level dirty state unchanged"), Level->GetPackage()->IsDirty(), bLevelDirty);
    return true;
}
#endif
