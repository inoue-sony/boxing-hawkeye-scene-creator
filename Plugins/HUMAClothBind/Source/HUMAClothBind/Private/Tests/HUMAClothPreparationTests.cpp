#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "HUMAClothPreparationSubsystem.h"
#include "HUMAClothBindAsset.h"
#include "Engine/World.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMAClothPreparationTest, "HUMAClothBind.Preparation.CacheInvalidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHUMAClothPreparationTest::RunTest(const FString&)
{
    auto* Source = LoadObject<UHUMAClothBindAsset>(nullptr, TEXT("/Game/GarmentAssets/CB_button_01.CB_button_01"));
    auto* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/character_lod.character_lod"));
    if (!TestNotNull(TEXT("Sample bind asset"), Source) || !TestNotNull(TEXT("Sample target mesh"), Mesh)) return false;
    auto* Asset = DuplicateObject<UHUMAClothBindAsset>(Source, GetTransientPackage(),
        MakeUniqueObjectName(GetTransientPackage(), UHUMAClothBindAsset::StaticClass()));
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    auto* Cache = World->GetSubsystem<UHUMAClothPreparationSubsystem>();
    if (!TestNotNull(TEXT("Game-world preparation subsystem"), Cache)) { World->DestroyWorld(false); return false; }
    FString Error;
    TArray<int32> First, Second;
    // Source fingerprint incorporates the original asset's mesh paths, not the Bind object's path.
    bool Success = TestTrue(TEXT("Full validated preparation"), Cache->Prepare(Asset, Mesh, First, Error));
    if (!Success) AddError(Error);
    Success &= TestTrue(TEXT("Repeated source reuses preparation"), Cache->Prepare(Asset, Mesh, Second, Error));
    TestEqual(TEXT("One validated miss"), Cache->CacheMisses, 1);
    TestEqual(TEXT("One cache hit"), Cache->CacheHits, 1);
    TestTrue(TEXT("Mapping is unchanged"), First == Second);
    auto* Other = DuplicateObject<UHUMAClothBindAsset>(Source, GetTransientPackage(),
        MakeUniqueObjectName(GetTransientPackage(), UHUMAClothBindAsset::StaticClass()));
    Success &= TestTrue(TEXT("Another garment can share an identical validated body mapping"), Cache->Prepare(Other, Mesh, Second, Error));
    TestEqual(TEXT("Identical body tables share mapping work"), Cache->SharedMappingReuses, 1);
    TestTrue(TEXT("Shared mapping equals direct validated result"), First == Second);
    Cache->InvalidatePreparation();
    Success &= TestTrue(TEXT("Explicit invalidation revalidates"), Cache->Prepare(Asset, Mesh, Second, Error));
    TestEqual(TEXT("Explicit invalidation causes a miss"), Cache->CacheMisses, 3);
    Asset->SigmaMin += 0.5f;
    FPropertyChangedEvent Changed(FindFProperty<FProperty>(UHUMAClothBindAsset::StaticClass(), TEXT("SigmaMin")));
    Asset->PostEditChangeProperty(Changed);
    TestFalse(TEXT("Changed source settings are not accepted from cache"), Cache->Prepare(Asset, Mesh, Second, Error));
    TestFalse(TEXT("Stale bake error is explicit"), Error.IsEmpty());
    World->DestroyWorld(false);
    return Success;
}
#endif
