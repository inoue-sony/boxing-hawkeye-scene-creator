#include "WOCBatchSubsystem.h"
#include "WOCDeformerCore.h"
#include "NNEModelData.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWOCBatchEmptyFramesTest, "WOC.Batch.EmptyFramesAndCacheRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWOCBatchEmptyFramesTest::RunTest(const FString&)
{
	auto* World = UWorld::CreateWorld(EWorldType::Game, false);
	auto* Subsystem = World->GetSubsystem<UWOCBatchSubsystem>();
	if (!TestNotNull(TEXT("Initialized world batch subsystem"), Subsystem)) { World->DestroyWorld(false); return false; }
	auto* Model = NewObject<UNNEModelData>();
	const TObjectKey<UNNEModelData> Key(Model);
	FWOCModelCache::Cache.Add(Key).RefCount = 1;
	Subsystem->BatchGroups.Add(Key).ModelData = Model;
	Subsystem->SlotMappings.AddDefaulted();
	Subsystem->CurrentFrameNumber = GFrameCounter - 1;
	Subsystem->Tick(0);
	TestTrue(TEXT("No new animation registrations clears previous-frame handles"), Subsystem->SlotMappings.IsEmpty());
	World->DestroyWorld(false);
	TestFalse(TEXT("World teardown releases the batch group's model-cache reference"), FWOCModelCache::Cache.Contains(Key));
	FWOCModelCache::Cache.Remove(Key);
	return true;
}
#endif
