#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "HUMAClothBindAsset.h"
#include "HUMAClothGpuState.h"
#include "HUMAClothPreparationSubsystem.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Rendering/SkeletalMeshRenderData.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMAClothCompatibleLODTest, "HUMAClothBind.Reuse.CompatibleBodyLODs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHUMAClothCompatibleLODTest::RunTest(const FString&)
{
    auto* Asset = LoadObject<UHUMAClothBindAsset>(nullptr, TEXT("/Game/GarmentAssets/CB_button_01.CB_button_01"));
    auto* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/HUMA_Asset/character_lod.character_lod"));
    if (!TestNotNull(TEXT("Current bind"), Asset) || !TestNotNull(TEXT("Current different source body"), Mesh)) return false;
    const auto* Reference = Asset->BodyMesh ? Asset->BodyMesh->GetResourceForRendering() : nullptr;
    const auto* Target = Mesh->GetResourceForRendering();
    if (!TestNotNull(TEXT("Reference render data"), Reference) || !TestNotNull(TEXT("Target render data"), Target)) return false;
    TestTrue(TEXT("Exercises cross-mesh reuse"), Asset->BodyMesh != Mesh);
    const auto& RootLOD = Target->LODRenderData[0];
    const auto* RootIndices = RootLOD.MultiSizeIndexContainer.GetIndexBuffer();
    for (const auto& Section : RootLOD.RenderSections)
    {
        int32 Body = 0, Other = 0;
        for (uint32 Index = Section.BaseIndex; Index < Section.BaseIndex + Section.NumTriangles * 3; Index += 3)
        {
            bool FullBody = true;
            for (int32 Corner = 0; Corner < 3; ++Corner)
            {
                const float ID = RootLOD.StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(RootIndices->Get(Index + Corner), 1).X;
                FullBody &= ID >= 0 && ID < Asset->CanonicalToRender.Num();
            }
            FullBody ? ++Body : ++Other;
        }
        AddInfo(FString::Printf(TEXT("Source material slot%d: canonical triangles%d, other triangles%d"), Section.MaterialIndex, Body, Other));
    }
    FHUMAClothSourceLODs SourceLODs;
    FString Error;
    if (!TestTrue(TEXT("Prepare every actual source LOD"), Asset->BuildSourceLODs(Mesh, SourceLODs, Error)))
    { AddError(Error); return false; }
    TestEqual(TEXT("All source LODs available"), SourceLODs.Num(), Target->LODRenderData.Num());
    FHUMAClothGpuState GPU(*Asset, SourceLODs);
    for (const auto& Pair : SourceLODs)
    {
        const auto* State = GPU.GetLODData(Pair.Key);
        if (TestNotNull(TEXT("GPU state has every source LOD"), State))
        {
            TestEqual(TEXT("GPU keeps original Bind domain"), State->BindBodyLOD, 0);
            TestTrue(TEXT("GPU retains the correct transfer"), State->SourceTransfer == Pair.Value.Transfer);
        }
    }
    for (int32 LOD = 0; LOD < Target->LODRenderData.Num(); ++LOD)
    {
        const auto* Entry = Asset->GetBodyLODBind(LOD);
        if (!TestNotNull(FString::Printf(TEXT("Baked LOD%d"), LOD), Entry)) continue;
        if (!Reference->LODRenderData.IsValidIndex(LOD)) { AddError(TEXT("Missing reference LOD")); continue; }
        const auto& SourceLOD = Reference->LODRenderData[LOD];
        const auto& TargetLOD = Target->LODRenderData[LOD];
        int32 Negative = 0;
        for (int32 Render : Entry->CanonicalToRender)
            if (SourceLOD.StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(Render, 1).X < 0) ++Negative;
        AddInfo(FString::Printf(TEXT("LOD%d: bind body=%d, reference render=%u, target render=%u, required negative IDs=%d"),
            LOD, Entry->CanonicalToRender.Num(), SourceLOD.GetNumVertices(), TargetLOD.GetNumVertices(), Negative));
        const auto* Prepared = SourceLODs.Find(LOD);
        if (!TestNotNull(TEXT("Prepared source mapping"), Prepared)) continue;
        TestEqual(TEXT("Compatible LOD uses the original Bind domain"), Prepared->BindBodyLOD, 0);
        TestEqual(TEXT("Canonical sample count retained"), Prepared->Mapping.Num(), Asset->CanonicalToRender.Num());
        if (LOD == 0)
        {
            TestFalse(TEXT("LOD0 has no interpolation cost/change"), Prepared->Transfer.IsValid());
            continue;
        }
        if (!TestTrue(TEXT("Lower LOD has a transfer"), Prepared->Transfer.IsValid())) continue;
        TArray<FVector3f> Positions, Mapped, RigidPositions, RigidMapped;
        for (uint32 Vertex = 0; Vertex < TargetLOD.GetNumVertices(); ++Vertex)
            Positions.Add(TargetLOD.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(Vertex));
        if (!TestTrue(TEXT("CPU transfer evaluates"), Prepared->Transfer->Evaluate(Positions, Mapped, Error)))
        { AddError(Error); continue; }
        const FTransform3f Rigid(FQuat4f(FVector3f(0, 0, 1), 0.7f), FVector3f(13, -7, 9));
        for (const auto& Point : Positions) RigidPositions.Add(Rigid.TransformPosition(Point));
        TestTrue(TEXT("Rigid source transfer evaluates"), Prepared->Transfer->Evaluate(RigidPositions, RigidMapped, Error));
        double Maximum = 0;
        for (int32 Vertex = 0; Vertex < Mapped.Num(); ++Vertex)
            Maximum = FMath::Max(Maximum, double(FVector3f::Distance(Rigid.TransformPosition(Mapped[Vertex]), RigidMapped[Vertex])));
        TestTrue(TEXT("Barycentric transfer is rigid equivariant"), Maximum < 0.0001);
        AddInfo(FString::Printf(TEXT("LOD%d transfer: samples%d, rest max%.6fcm/rms%.6fcm, rigid max%.9gcm"),
            LOD, Mapped.Num(), Prepared->Transfer->MaxRestDistanceCm, Prepared->Transfer->RmsRestDistanceCm, Maximum));
        Positions.Pop();
        TestFalse(TEXT("Wrong source LOD buffer size rejects"), Prepared->Transfer->Evaluate(Positions, Mapped, Error));
        TestFalse(TEXT("Invalid input has a diagnostic"), Error.IsEmpty());
    }
    TSharedPtr<const FHUMAClothLODTransfer, ESPMode::ThreadSafe> Invalid;
    TestFalse(TEXT("Missing source LOD rejects"), BuildHUMAClothLODTransfer(Mesh, 100, Asset->CanonicalToRender, Invalid, Error));
    TestFalse(TEXT("Missing LOD has a diagnostic"), Error.IsEmpty());
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    auto* Cache = World->GetSubsystem<UHUMAClothPreparationSubsystem>();
    if (TestNotNull(TEXT("Source preparation cache"), Cache))
    {
        FHUMAClothSourceLODs First, Second;
        TestTrue(TEXT("Cached full-LOD preparation"), Cache->PrepareLODs(Asset, Mesh, First, Error));
        auto* Other = LoadObject<UHUMAClothBindAsset>(nullptr, TEXT("/Game/GarmentAssets/CB_button_02.CB_button_02"));
        if (TestNotNull(TEXT("Second garment sharing the body"), Other))
        {
            TestTrue(TEXT("Another garment reuses transfer preparation"), Cache->PrepareLODs(Other, Mesh, Second, Error));
            TestEqual(TEXT("Shared transfer construction is counted"), Cache->SharedLODTransferReuses, 1);
            TestTrue(TEXT("All garments share the immutable LOD1 transfer"), First.FindChecked(1).Transfer == Second.FindChecked(1).Transfer);
        }
        Cache->InvalidatePreparation();
        TestTrue(TEXT("Invalidation rebuilds source LOD maps"), Cache->PrepareLODs(Asset, Mesh, Second, Error));
        TestTrue(TEXT("Invalidation does not return the stale transfer"), First.FindChecked(1).Transfer != Second.FindChecked(1).Transfer);
    }
    return !HasAnyErrors();
}
#endif
