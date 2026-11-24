#include "HUMAClothBodySharing.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMAClothBodyTopologyTest, "HUMA.ClothBind.SharedBody.Topology",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHUMAClothBodyTopologyTest::RunTest(const FString&)
{
    FHUMAClothBodyTopology First, Second;
    First.Mapping = {0, 1, 2, 3}; First.Quads = {0, 1, 2, 3};
    First.Offsets = {0, 1, 2, 3, 4}; First.Incidents = {0, 0, 0, 0};
    First.UpdateSignature();
    Second.Mapping = First.Mapping; Second.Quads = First.Quads;
    Second.Offsets = First.Offsets; Second.Incidents = First.Incidents;
    Second.UpdateSignature();
    TestTrue(TEXT("Equivalent arrays can share immutable tables"), First.IsCompatible(Second));
    for (int32 Field = 0; Field < 4; ++Field)
    {
        FHUMAClothBodyTopology Different;
        Different.Mapping = First.Mapping; Different.Quads = First.Quads;
        Different.Offsets = First.Offsets; Different.Incidents = First.Incidents;
        auto* Values = Field == 0 ? &Different.Mapping : Field == 1 ? &Different.Quads : Field == 2 ? &Different.Offsets : &Different.Incidents;
        ++(*Values)[0];
        Different.Signature = First.Signature;
        TestFalse(TEXT("Hash collisions do not merge different body tables"), First.IsCompatible(Different));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMAClothBodyNormalKeyTest, "HUMA.ClothBind.SharedBody.EvaluationIsolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHUMAClothBodyNormalKeyTest::RunTest(const FString&)
{
    int32 MeshIdentity = 0, PositionsA = 0, PositionsB = 0;
    FHUMAClothBodyTopology TopologyA, TopologyB;
    FHUMAClothBodyNormalKey First;
    First.PrimitiveId.PrimIDValue = 1; First.LOD = 0;
    First.RenderMesh = &MeshIdentity; First.Positions = &PositionsA; First.Topology = &TopologyA;
    TMap<FHUMAClothBodyNormalKey, int32> Groups;
    Groups.Add(First, 1);
    TestEqual(TEXT("Same component/input/topology reuses its group"), Groups.FindRef(First), 1);
    auto Other = First;
    Other.PrimitiveId.PrimIDValue = 2;
    TestFalse(TEXT("Same mesh on another character never shares normals"), Groups.Contains(Other));
    Other = First; Other.LOD = 1;
    TestFalse(TEXT("Different body LOD stays isolated"), Groups.Contains(Other));
    Other = First; Other.Positions = &PositionsB;
    TestFalse(TEXT("Another final position buffer stays isolated"), Groups.Contains(Other));
    Other = First; Other.Topology = &TopologyB;
    TestFalse(TEXT("Different mapping/topology stays isolated"), Groups.Contains(Other));
    TMap<FHUMAClothBodyNormalKey, int32> NextEvaluation;
    TestTrue(TEXT("Evaluation-local cache cannot retain another evaluation's result"), NextEvaluation.IsEmpty());
    return true;
}
#endif
