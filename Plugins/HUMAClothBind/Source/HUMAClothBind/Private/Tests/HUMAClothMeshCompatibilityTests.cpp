#include "HUMAClothMeshCompatibility.h"
#include "HUMAClothBindAsset.h"
#include "HUMAClothGpuState.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMAClothCompatibilityTest, "HUMAClothBind.Reuse.CanonicalTopology",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHUMAClothCompatibilityTest::RunTest(const FString&)
{
    using namespace HUMACloth;
    const TArray<int32> IDs = {0,1,2,3}, Triangles = {0,1,2,0,2,3}, Quads = {0,1,2,3};
    const TArray<FVector3f> Positions = {{0,0,0},{1,0,0},{1,1,0},{0,1,0}};
    const TArray<int32> ReorderedIDs = {2,0,3,1}, ReorderedTriangles = {1,3,0,1,0,2};
    const TArray<FVector3f> OtherShape = {{2,3,1},{0,0,0},{0,3,2},{2,0,0}};
    FCanonicalSurface Reference, Target;
    FString Error;
    TestTrue(TEXT("Reference surface"), BuildCanonicalSurface(IDs, Positions, Triangles, 4, Reference, Error));
    TestTrue(TEXT("Different shape and render order"), BuildCanonicalSurface(ReorderedIDs, OtherShape, ReorderedTriangles, 4, Target, Error));
    TestTrue(TEXT("Same canonical topology"), HaveCompatibleCanonicalTopology(Reference, Target, Quads, Error));
    TestTrue(TEXT("Target mapping rebuilt"), Target.Mapping == TArray<int32>({1,3,0,2}));
    const TArray<int32> Reversed = {0,2,1,0,3,2}, Different = {0,1,3,1,2,3};
    TestTrue(TEXT("Reversed surface readable"), BuildCanonicalSurface(IDs, Positions, Reversed, 4, Target, Error));
    TestFalse(TEXT("Winding mismatch detected"), HaveCompatibleCanonicalTopology(Reference, Target, Quads, Error));
    TestTrue(TEXT("Retriangulated surface readable"), BuildCanonicalSurface(IDs, Positions, Different, 4, Target, Error));
    TestTrue(TEXT("Opposite quad diagonal accepted"), HaveCompatibleCanonicalTopology(Reference, Target, Quads, Error));

    auto CheckTarget = [&](const TCHAR* Label, const TArray<int32>& Indices, bool Expected)
    {
        FCanonicalSurface Surface;
        FString Reason;
        if (!TestTrue(FString::Printf(TEXT("%s is readable"), Label),
            BuildCanonicalSurface(IDs, Positions, Indices, 4, Surface, Reason))) return;
        TestEqual(Label, HaveCompatibleCanonicalTopology(Reference, Surface, Quads, Reason), Expected);
        if (!Expected) TestFalse(TEXT("Rejection has a reason"), Reason.IsEmpty());
    };
    CheckTarget(TEXT("Triangle order and cyclic corner order"), {2,3,0,1,2,0}, true);
    CheckTarget(TEXT("Only one reversed triangle rejected"), {0,2,1,0,2,3}, false);
    CheckTarget(TEXT("Missing triangle rejected despite complete ID coverage"), {0,1,2}, false);
    CheckTarget(TEXT("Duplicate triangle rejected"), {0,1,2,0,2,3,0,1,2}, false);
    CheckTarget(TEXT("Extra triangle rejected"), {0,1,2,0,2,3,0,1,3}, false);
    CheckTarget(TEXT("Both diagonals at once rejected"), {0,1,2,0,2,3,0,1,3,1,2,3}, false);
    CheckTarget(TEXT("Different boundary connectivity rejected"), {0,1,2,1,2,3}, false);

    FCanonicalSurface ReverseReference;
    TestTrue(TEXT("Reverse reference readable"), BuildCanonicalSurface(IDs, Positions, Reversed, 4, ReverseReference, Error));
    TestTrue(TEXT("Both reversed accepted"), HaveCompatibleCanonicalTopology(ReverseReference, ReverseReference, Quads, Error));
    const TArray<int32> ReverseDifferent = {0,3,1,1,3,2};
    TestTrue(TEXT("Reverse opposite diagonal readable"), BuildCanonicalSurface(IDs, Positions, ReverseDifferent, 4, Target, Error));
    TestTrue(TEXT("Both reversed with different diagonals accepted"),
        HaveCompatibleCanonicalTopology(ReverseReference, Target, Quads, Error));
    TestFalse(TEXT("Normal target rejected against reversed reference"),
        HaveCompatibleCanonicalTopology(ReverseReference, Reference, Quads, Error));

    const TArray<int32> WrongIDs = {0,1,3,2};
    TestTrue(TEXT("Wrong ID assignment readable"), BuildCanonicalSurface(WrongIDs, Positions, Triangles, 4, Target, Error));
    TestFalse(TEXT("Wrong canonical connectivity rejected"), HaveCompatibleCanonicalTopology(Reference, Target, Quads, Error));

    const TArray<int32> ExtraIDs = {0,1,2,3,4,5,6}, ExtraTriangles = {0,1,2,0,2,3,4,5,6};
    const TArray<FVector3f> ExtraPositions = {{0,0,0},{1,0,0},{1,1,0},{0,1,0},{2,0,0},{3,0,0},{2,1,0}};
    TestTrue(TEXT("Extra body parts readable"), BuildCanonicalSurface(ExtraIDs, ExtraPositions, ExtraTriangles, 4, Target, Error));
    TestTrue(TEXT("Out-of-range body parts ignored"), HaveCompatibleCanonicalTopology(Reference, Target, Quads, Error));
    const TArray<int32> NegativeIDs = {0,1,2,3,-1,-1,-1};
    TestTrue(TEXT("Invalid auxiliary IDs ignored"), BuildCanonicalSurface(NegativeIDs, ExtraPositions, ExtraTriangles, 4, Target, Error));
    TestTrue(TEXT("Auxiliary parts do not affect coverage"), HaveCompatibleCanonicalTopology(Reference, Target, Quads, Error));
    const TArray<int32> SeamIDs = {0,1,2,3,0}, SeamTriangles = {4,1,2,4,2,3};
    const TArray<FVector3f> SeamPositions = {{0,0,0},{1,0,0},{1,1,0},{0,1,0},{0,0,0}};
    TestTrue(TEXT("Coincident render seam readable"), BuildCanonicalSurface(SeamIDs, SeamPositions, SeamTriangles, 4, Target, Error));
    TestTrue(TEXT("Render seam does not duplicate faces"), HaveCompatibleCanonicalTopology(Reference, Target, Quads, Error));
    const TArray<int32> Missing = {0,1,2,-1}, Conflicting = {0,1,2,0};
    TestFalse(TEXT("Missing canonical ID rejected"), BuildCanonicalSurface(Missing, Positions, Triangles, 4, Target, Error));
    TestFalse(TEXT("Inconsistent duplicate rejected"), BuildCanonicalSurface(Conflicting, Positions, Triangles, 4, Target, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMAClothCanonicalCoverageTest, "HUMAClothBind.Reuse.CanonicalFaceCoverage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHUMAClothCanonicalCoverageTest::RunTest(const FString&)
{
    using namespace HUMACloth;
    const TArray<int32> IDs = {0,1,2,3,4,5,6}, Quads = {0,1,2,3,3,2,4,5,5,4,6,6};
    const TArray<FVector3f> Positions = {{0,0,0},{1,0,0},{1,1,0},{0,1,0},{1,2,0},{0,2,0},{0,3,0}};
    const TArray<int32> Triangles = {0,1,2,0,2,3,3,2,4,3,4,5,5,4,6};
    const TArray<int32> OtherDiagonals = {0,1,3,1,2,3,3,2,5,2,4,5,5,4,6};
    FString Error;
    FCanonicalSurface Reference, Target;
    TestTrue(TEXT("Mixed triangle and quad reference readable"), BuildCanonicalSurface(IDs, Positions, Triangles, 7, Reference, Error));
    TestTrue(TEXT("Mixed target readable"), BuildCanonicalSurface(IDs, Positions, OtherDiagonals, 7, Target, Error));
    TestTrue(TEXT("Padded triangle consumed once with both quad diagonals"),
        HaveCompatibleCanonicalTopology(Reference, Target, Quads, Error));

    auto CheckStoredFaces = [&](const TCHAR* Label, const TArray<int32>& Faces, bool Expected)
    {
        FString Reason;
        TestEqual(Label, HaveCompatibleCanonicalTopology(Reference, Target, Faces, Reason), Expected);
        if (!Expected) TestFalse(TEXT("Invalid stored face has a reason"), Reason.IsEmpty());
    };
    CheckStoredFaces(TEXT("Leading duplicate pads a triangle"), {0,1,2,3,3,2,4,5,5,5,4,6}, true);
    CheckStoredFaces(TEXT("Middle duplicate pads a triangle"), {0,1,2,3,3,2,4,5,5,4,4,6}, true);
    CheckStoredFaces(TEXT("Closing duplicate pads a triangle"), {0,1,2,3,3,2,4,5,5,4,6,5}, true);
    CheckStoredFaces(TEXT("Non-adjacent repeat rejected"), {0,1,2,3,3,2,4,5,5,4,5,6}, false);
    CheckStoredFaces(TEXT("Fewer than three corners rejected"), {0,1,2,3,3,2,4,5,5,5,4,4}, false);
    CheckStoredFaces(TEXT("Negative stored canonical ID rejected"), {0,1,2,3,3,2,4,5,5,4,6,-1}, false);
    CheckStoredFaces(TEXT("Out-of-range stored canonical ID rejected"), {0,1,2,3,3,2,4,5,5,4,6,7}, false);
    CheckStoredFaces(TEXT("Extra triangles cannot be ignored"), {0,1,2,3,3,2,4,5}, false);
    CheckStoredFaces(TEXT("Reused stored quad rejected"), {0,1,2,3,3,2,4,5,5,4,6,6,0,1,2,3}, false);
    CheckStoredFaces(TEXT("Incomplete stored face array rejected"), {0,1,2}, false);
    CheckStoredFaces(TEXT("Missing stored faces rejected"), {}, false);

    const TArray<int32> MixedWinding = {0,1,2,0,2,3,3,4,2,3,5,4,5,4,6};
    const TArray<int32> MixedWindingOtherDiagonal = {0,1,3,1,2,3,3,5,2,2,5,4,5,4,6};
    FCanonicalSurface MixedReference, MixedTarget;
    TestTrue(TEXT("Per-quad reference winding readable"),
        BuildCanonicalSurface(IDs, Positions, MixedWinding, 7, MixedReference, Error));
    TestTrue(TEXT("Per-quad target winding readable"),
        BuildCanonicalSurface(IDs, Positions, MixedWindingOtherDiagonal, 7, MixedTarget, Error));
    TestTrue(TEXT("Each quad may share its own reversed reference winding"),
        HaveCompatibleCanonicalTopology(MixedReference, MixedTarget, Quads, Error));
    TestFalse(TEXT("Only one full quad reversed rejected"),
        HaveCompatibleCanonicalTopology(Reference, MixedTarget, Quads, Error));

    const TArray<int32> ReversedTriangle = {0,1,3,1,2,3,3,2,5,2,4,5,5,6,4};
    TestTrue(TEXT("Reversed padded triangle readable"), BuildCanonicalSurface(IDs, Positions, ReversedTriangle, 7, Target, Error));
    TestFalse(TEXT("Padded triangle winding mismatch rejected"), HaveCompatibleCanonicalTopology(Reference, Target, Quads, Error));
    const TArray<int32> ReverseTriangleReference = {0,1,2,0,2,3,3,2,4,3,4,5,5,6,4};
    FCanonicalSurface ReversePaddedReference;
    TestTrue(TEXT("Reversed padded reference readable"),
        BuildCanonicalSurface(IDs, Positions, ReverseTriangleReference, 7, ReversePaddedReference, Error));
    TestTrue(TEXT("Both reversed padded triangles accepted with opposite quad diagonals"),
        HaveCompatibleCanonicalTopology(ReversePaddedReference, Target, Quads, Error));
    const TArray<int32> MissingTriangle = {0,1,3,1,2,3,3,2,5,2,4,5};
    TestTrue(TEXT("Missing padded triangle retains all IDs"), BuildCanonicalSurface(IDs, Positions, MissingTriangle, 7, Target, Error));
    TestFalse(TEXT("Missing padded triangle rejected"), HaveCompatibleCanonicalTopology(Reference, Target, Quads, Error));
    const TArray<int32> DuplicateTriangle = {0,1,3,1,2,3,3,2,5,2,4,5,5,4,6,5,4,6};
    TestTrue(TEXT("Duplicate padded triangle readable"), BuildCanonicalSurface(IDs, Positions, DuplicateTriangle, 7, Target, Error));
    TestFalse(TEXT("Duplicate padded triangle rejected"), HaveCompatibleCanonicalTopology(Reference, Target, Quads, Error));
    const TArray<int32> DegenerateTriangle = {0,1,2,0,2,3,3,2,4,3,4,5,5,5,6};
    TestFalse(TEXT("Degenerate canonical render triangle rejected with a reason"),
        BuildCanonicalSurface(IDs, Positions, DegenerateTriangle, 7, Target, Error));
    TestFalse(TEXT("Degenerate render triangle has a reason"), Error.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMAClothMappingStateTest, "HUMAClothBind.Reuse.GPUState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHUMAClothMappingStateTest::RunTest(const FString&)
{
    auto* Asset = NewObject<UHUMAClothBindAsset>();
    Asset->CanonicalToRender = {0,1,2};
    auto* Child = NewObject<UHUMAClothBindAsset>(Asset);
    Child->BodyLODIndex = 1; Child->CanonicalToRender = {0,1,2};
    Asset->AdditionalBodyLODs.Add(Child);
    const TArray<int32> Target = {2,0,1};
    FHUMAClothGpuState Original(*Asset), Reused(*Asset, Target, false);
    TestEqual(TEXT("Original keeps child LODs"), Original.BodyLODs.Num(), 1);
    TestEqual(TEXT("Reused mesh has no unsafe child LODs"), Reused.BodyLODs.Num(), 0);
    TestTrue(TEXT("GPU uses target render mapping"), Reused.BodyTopology->Mapping == TArray<uint32>({2,0,1}));
    TestTrue(TEXT("Original asset is unchanged"), Asset->CanonicalToRender == TArray<int32>({0,1,2}));
    return true;
}
#endif
