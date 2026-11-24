#include "HUMAClothBatch.h"
#include "Misc/AutomationTest.h"
#include "HAL/IConsoleManager.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMAClothBatchDefaultTest, "HUMAClothBind.Batch.DefaultAndRollback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHUMAClothBatchDefaultTest::RunTest(const FString&)
{
    auto* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.HUMACloth.Optimization"));
    if (!TestNotNull(TEXT("Runtime path selector is registered"), Variable)) return false;
    const int32 Previous = Variable->GetInt();
    TestEqual(TEXT("Fresh product default enables batching"), Previous, 2);
    for (int32 Path : {0, 1, 2})
    {
        Variable->Set(Path, ECVF_SetByCode);
        TestEqual(TEXT("Legacy/prepared/batched paths remain selectable"), Variable->GetInt(), Path);
    }
    Variable->Set(Previous, ECVF_SetByCode);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMAClothBatchPackingTest, "HUMAClothBind.Batch.OffsetIsolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHUMAClothBatchPackingTest::RunTest(const FString&)
{
    FHUMAClothGpuState First, Second, Combined;
    auto Topology = MakeShared<FHUMAClothBodyTopology, ESPMode::ThreadSafe>();
    auto Fill = [&](FHUMAClothGpuState& Part, float Shift)
    {
        Part.BodyTopology = Topology; Part.Slots = 2;
        Part.Rest = {FVector3f(Shift,0,0), FVector3f(Shift+1,0,0), FVector3f(Shift,1,0)};
        Part.UV = {FVector2f(0,0),FVector2f(1,0),FVector2f(0,1)};
        Part.Bary = {FVector3f(1,0,0)};
        Part.AnchorTriangles = {0,1,2};
        Part.RestPosition = {FVector3f(Shift,0,0)};
        Part.RestTangent = {FVector3f(1,0,0)}; Part.RestBitangent = {FVector3f(0,1,0)};
        Part.RestNormal = {FVector3f(0,0,1)};
        Part.InfluenceAnchors = {0,0,0,0,0,0};
        Part.InfluenceWeights = {1,0,1,0,1,0};
        Part.Triangles = {0,1,2}; Part.ClothOffsets = {0,1,2,3}; Part.ClothIncidents = {0,0,0};
    };
    Fill(First, 0); Fill(Second, 10);
    const TArray<const FHUMAClothGpuState*> Parts = {&First, &Second};
    TArray<FHUMAClothBatchRange> Ranges;
    FString Error;
    if (!TestTrue(TEXT("Compatible tables pack"), BuildHUMAClothBatch(Parts, Combined, Ranges, Error))) return false;
    TestEqual(TEXT("Vertex ranges retain separate parts"), Ranges[1].FirstVertex, 3u);
    TestEqual(TEXT("Triangle offsets cannot cross parts"), Combined.Triangles[3], 3u);
    TestEqual(TEXT("Anchor influences offset independently"), Combined.InfluenceAnchors[6], 1u);
    TestEqual(TEXT("Incident triangle offsets"), Combined.ClothIncidents[3], 1u);
    TestEqual(TEXT("Terminal incidence offset"), Combined.ClothOffsets.Last(), 6u);
    TestEqual(TEXT("Weights unchanged"), Combined.InfluenceWeights[7], Second.InfluenceWeights[1]);
    TestEqual(TEXT("Body anchor vertex ids stay canonical"), Combined.AnchorTriangles[3], First.AnchorTriangles[0]);
    FHUMAClothGpuState Invalid;
    TArray<FHUMAClothBatchRange> InvalidRanges;
    Second.Slots = 3;
    TestFalse(TEXT("Different slot layouts reject"), BuildHUMAClothBatch(Parts, Invalid, InvalidRanges, Error));
    Second.Slots = 2; Second.Triangles[0] = 3;
    TestFalse(TEXT("Invalid triangle rejects"), BuildHUMAClothBatch(Parts, Invalid, InvalidRanges, Error));
    return true;
}
#endif
