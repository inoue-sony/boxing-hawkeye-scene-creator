#include "HUMAClothBindAsset.h"
#include "HUMAClothBindComponent.h"
#include "Animation/MeshDeformer.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMAClothArraySpawnContractTest, "HUMAClothBind.ArraySpawn.Contract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHUMAClothArraySpawnContractTest::RunTest(const FString&)
{
    UFunction* Function = AHUMAClothPreviewActor::StaticClass()->FindFunctionByName(TEXT("SpawnBoundGarments"));
    if (!TestNotNull(TEXT("Reflected Blueprint array function"), Function)) return false;
    const auto* Assets = FindFProperty<FArrayProperty>(Function, TEXT("Assets"));
    const auto* Returned = FindFProperty<FArrayProperty>(Function, TEXT("ReturnValue"));
    const auto* Errors = FindFProperty<FArrayProperty>(Function, TEXT("Failures"));
    if (!TestNotNull(TEXT("Assets input is an array"), Assets) ||
        !TestNotNull(TEXT("Return value is an array"), Returned) ||
        !TestNotNull(TEXT("Failures output is an array"), Errors)) return false;
    const auto* AssetType = CastField<FObjectPropertyBase>(Assets->Inner);
    const auto* ActorType = CastField<FObjectPropertyBase>(Returned->Inner);
    const auto* FailureType = CastField<FStructProperty>(Errors->Inner);
    TestTrue(TEXT("Assets array holds Bind asset references"), AssetType && AssetType->PropertyClass == UHUMAClothBindAsset::StaticClass());
    TestTrue(TEXT("Return array holds garment actor references"), ActorType && ActorType->PropertyClass == AHUMAClothPreviewActor::StaticClass());
    TestTrue(TEXT("Failures array holds Blueprint failure structs"), FailureType && FailureType->Struct == FHUMAClothSpawnFailure::StaticStruct());
    TestTrue(TEXT("Failures is an output pin"), Errors->HasAnyPropertyFlags(CPF_OutParm) && !Errors->HasAnyPropertyFlags(CPF_ConstParm));
    TArray<FHUMAClothSpawnFailure> Failures;
    Failures.AddDefaulted();
    AddExpectedError(TEXT("Spawn Bound Garments requires at least one"), EAutomationExpectedErrorFlags::Contains, 1);
    const auto Empty = AHUMAClothPreviewActor::SpawnBoundGarments({}, nullptr, Failures);
    TestTrue(TEXT("Empty list spawns nothing and reports a global input error"), Empty.IsEmpty() &&
        Failures.Num() == 1 && Failures[0].InputIndex == INDEX_NONE && !Failures[0].Asset && !Failures[0].Error.IsEmpty());
    TStrongObjectPtr<UHUMAClothBindAsset> Asset(NewObject<UHUMAClothBindAsset>());
    const TArray<UHUMAClothBindAsset*> Inputs = {Asset.Get(), nullptr};
    AddExpectedError(TEXT("Spawn Bound Garments input"), EAutomationExpectedErrorFlags::Contains, 2);
    const auto InvalidSource = AHUMAClothPreviewActor::SpawnBoundGarments(Inputs, nullptr, Failures);
    TestTrue(TEXT("Invalid source reports every input without stale prior failures"),
        InvalidSource.IsEmpty() && Failures.Num() == Inputs.Num());
    for (int32 Index = 0; Index < Failures.Num(); ++Index)
    {
        TestEqual(TEXT("Failure index matches original input"), Failures[Index].InputIndex, Index);
        TestTrue(TEXT("Failure retains its asset reference"), Failures[Index].Asset == Inputs[Index]);
        TestTrue(TEXT("Failure includes source validation reason"), Failures[Index].Error.Contains(TEXT("registered source")));
    }
    return true;
}

namespace
{
int32 GarmentCount(UWorld* World)
{
    int32 Count = 0;
    for (TActorIterator<AHUMAClothPreviewActor> It(World); It; ++It) if (IsValid(*It)) ++Count;
    return Count;
}

void TestArraySpawn(FAutomationTestBase& Test, UWorld* World, UHUMAClothBindAsset* Buttons, UHUMAClothBindAsset* Tee)
{
    auto* Body = World->SpawnActor<ASkeletalMeshActor>();
    auto* Source = Body->GetSkeletalMeshComponent();
    Source->SetSkeletalMeshAsset(Buttons->BodyMesh);
    Source->SetMeshDeformer(nullptr);
    const FString ButtonsSignature = Buttons->BakeSignature, TeeSignature = Tee->BakeSignature;
    const int32 Before = GarmentCount(World);
    TStrongObjectPtr<UHUMAClothBindAsset> Unbaked(NewObject<UHUMAClothBindAsset>());
    TArray<FHUMAClothSpawnFailure> Failures;
    const TArray<UHUMAClothBindAsset*> Inputs = {Buttons, nullptr, Unbaked.Get(), Tee};
    Test.AddExpectedError(TEXT("Spawn Bound Garments input"), EAutomationExpectedErrorFlags::Contains, 2);
    const auto Spawned = AHUMAClothPreviewActor::SpawnBoundGarments(Inputs, Source, Failures);
    Test.TestEqual(TEXT("Successful entries survive other failures"), Spawned.Num(), 2);
    Test.TestEqual(TEXT("Failed entries leave no garment actors"), GarmentCount(World), Before + 2);
    Test.TestEqual(TEXT("Both failure entries reported"), Failures.Num(), 2);
    if (Failures.Num() == 2)
    {
        Test.TestTrue(TEXT("Null input failure identifies index and asset"), Failures[0].InputIndex == 1 && !Failures[0].Asset);
        Test.TestTrue(TEXT("Unbaked failure identifies index and asset"), Failures[1].InputIndex == 2 && Failures[1].Asset == Unbaked.Get());
        Test.TestTrue(TEXT("Unbaked failure includes a useful reason"), Failures[1].Error.Contains(TEXT("Bake"), ESearchCase::IgnoreCase));
    }
    for (int32 Index = 0; Index < Spawned.Num(); ++Index)
    {
        Test.TestTrue(TEXT("All returned actors are valid"), IsValid(Spawned[Index]));
        Test.TestTrue(TEXT("Compact return array preserves successful input order"),
            Spawned[Index]->ClothBind->BindAsset == (Index == 0 ? Buttons : Tee));
        Test.TestTrue(TEXT("Array spawn uses the requested component"), Spawned[Index]->ClothBind->GetSourceBodyComponent() == Source);
        Test.TestTrue(TEXT("Binding initialized"), Spawned[Index]->ClothBind->IsBindingInitialized());
        Test.TestEqual(TEXT("Each actor initializes once"), Spawned[Index]->ClothBind->InitializationCount, 1);
        Test.TestTrue(TEXT("Standard adapter lease acquired for every success"), Spawned[Index]->ClothBind->bUsingStandardSkinningAdapter);
        Test.TestTrue(TEXT("Spawned garment attaches to body"), Spawned[Index]->GetAttachParentActor() == Body);
    }
    Test.TestEqual(TEXT("Buttons Bake remains unchanged"), Buttons->BakeSignature, ButtonsSignature);
    Test.TestEqual(TEXT("Tee Bake remains unchanged"), Tee->BakeSignature, TeeSignature);
    if (!Spawned.IsEmpty())
    {
        Spawned[0]->Destroy();
        Test.TestTrue(TEXT("Destroying one garment retains the shared adapter"), Source->GetComponentMeshDeformer() != nullptr);
        for (int32 Index = 1; Index < Spawned.Num(); ++Index) Spawned[Index]->Destroy();
    }
    Test.TestNull(TEXT("Last garment restores original no-deformer override"), Source->GetComponentMeshDeformer());
    Test.TestEqual(TEXT("All successes can be removed using the return array"), GarmentCount(World), Before);
    const TArray<UHUMAClothBindAsset*> Repeated = {Buttons, Buttons};
    const auto Duplicates = AHUMAClothPreviewActor::SpawnBoundGarments(Repeated, Source, Failures);
    Test.TestTrue(TEXT("Failures output resets for a later successful call"), Failures.IsEmpty());
    Test.TestEqual(TEXT("Each duplicate entry intentionally creates its own actor"), Duplicates.Num(), 2);
    if (Duplicates.Num() == 2) Test.TestTrue(TEXT("Duplicate entries return distinct actors"), Duplicates[0] != Duplicates[1]);
    for (auto* Actor : Duplicates) Actor->Destroy();
    Test.TestEqual(TEXT("Repeated spawn/despawn leaves no garments"), GarmentCount(World), Before);
    Test.TestNull(TEXT("Repeated spawn/despawn releases adapter"), Source->GetComponentMeshDeformer());
    Source->UnregisterComponent();
    Test.AddExpectedError(TEXT("Spawn Bound Garments input"), EAutomationExpectedErrorFlags::Contains, 2);
    const auto Unregistered = AHUMAClothPreviewActor::SpawnBoundGarments(Repeated, Source, Failures);
    Test.TestTrue(TEXT("Unregistered source cannot spawn and reports each failure"), Unregistered.IsEmpty() && Failures.Num() == 2);
    Body->Destroy();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMAClothArraySpawnLifecycleTest, "HUMAClothBind.ArraySpawn.SampleLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHUMAClothArraySpawnLifecycleTest::RunTest(const FString&)
{
    auto* Buttons = LoadObject<UHUMAClothBindAsset>(nullptr, TEXT("/Game/HUMAClothBind/Bindings/CB_Male_buttons"));
    auto* Tee = LoadObject<UHUMAClothBindAsset>(nullptr, TEXT("/Game/HUMAClothBind/Bindings/CB_Male_tee"));
    if (!TestNotNull(TEXT("Sample buttons"), Buttons) || !TestNotNull(TEXT("Sample tee"), Tee)) return false;
    TestArraySpawn(*this, GEditor->GetEditorWorldContext().World(), Buttons, Tee);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL());
    World->BeginPlay();
    World->SetBegunPlay(true);
    TestArraySpawn(*this, World, Buttons, Tee);
    World->EndPlay(EEndPlayReason::Quit);
    World->DestroyWorld(false);
    GEngine->DestroyWorldContext(World);
    return true;
}
#endif
