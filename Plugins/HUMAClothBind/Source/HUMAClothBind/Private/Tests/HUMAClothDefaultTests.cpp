#include "HUMAClothBindAsset.h"
#include "Misc/AutomationTest.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/ObjectWriter.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMAClothDefaultSigmaTest, "HUMAClothBind.Defaults.HalfCentimeter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHUMAClothDefaultSigmaTest::RunTest(const FString&)
{
    auto* Asset = NewObject<UHUMAClothBindAsset>();
    TestEqual(TEXT("New Bind defaults to 0.5cm"), Asset->SigmaMin, 0.5f);
    TestEqual(TEXT("Class default supports property reset and batch settings"), GetDefault<UHUMAClothBindAsset>()->SigmaMin, 0.5f);
    Asset->SigmaMin = 2.25f;
    TArray<uint8> Bytes;
    FObjectWriter Writer(Asset, Bytes);
    auto* Restored = NewObject<UHUMAClothBindAsset>();
    FObjectReader Reader(Restored, Bytes);
    TestEqual(TEXT("Explicit settings survive nonpersistent serialization"), Restored->SigmaMin, 2.25f);
    return true;
}
#endif
