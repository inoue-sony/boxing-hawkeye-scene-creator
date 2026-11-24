#include "HUMAClothBindAsset.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMAClothCanonicalBakeTest, "HUMAClothBind.CanonicalBake.SampleAssets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHUMAClothCanonicalBakeTest::RunTest(const FString&)
{
    const auto* GenderProperty = FindFProperty<FProperty>(UHUMAClothBindAsset::StaticClass(), TEXT("Gender"));
    TestTrue(TEXT("Legacy Gender is serialized but no longer editable"), GenderProperty && !GenderProperty->HasAnyPropertyFlags(CPF_Edit));
    for (const FString& Name : {FString(TEXT("CB_Male_buttons")), FString(TEXT("CB_Female_skirt"))})
    {
        auto* Source = LoadObject<UHUMAClothBindAsset>(nullptr, *(TEXT("/Game/HUMAClothBind/Bindings/") + Name));
        if (!TestNotNull(*Name, Source)) return false;
        FString Error;
        TestTrue(TEXT("Legacy asset remains current without re-Bake"), Source->IsBakeCurrent(Error));
        for (EHUMAClothBindMethod Method : {EHUMAClothBindMethod::BodyField, EHUMAClothBindMethod::ClothGraph})
        {
            TStrongObjectPtr<UHUMAClothBindAsset> Asset(NewObject<UHUMAClothBindAsset>());
            Asset->BodyMesh = Source->BodyMesh;
            Asset->GarmentMesh = Source->GarmentMesh;
            Asset->GarmentToBody = Source->GarmentToBody;
            Asset->bBakeAllBodyLODs = false;
            Asset->BakeMethod = Method;
            Asset->Gender = EHUMAClothGender::Female;
            Asset->Bake();
            if (!TestTrue(*Asset->BakeStatus, Asset->IsBakeCurrent(Error))) return false;
            TestTrue(TEXT("New Bake records shared topology"), Asset->bCanonicalBodyTopology);
            const auto Anchors = Asset->AnchorTriangles;
            const auto Bary = Asset->AnchorBarycentrics;
            const auto Weights = Asset->InfluenceWeights;
            const auto Indices = Asset->InfluenceAnchors;
            const auto Signature = Asset->BakeSignature;
            Asset->Gender = EHUMAClothGender::Male;
            TestTrue(TEXT("Legacy Gender no longer changes fresh source fingerprints"), Asset->IsBakeCurrent(Error));
            Asset->Bake();
            TestEqual(TEXT("Shared snapshot signature ignores Gender"), Asset->BakeSignature, Signature);
            TestTrue(TEXT("Gender-independent anchors, barycentrics and weights"), Asset->AnchorTriangles == Anchors &&
                Asset->AnchorBarycentrics == Bary && Asset->InfluenceWeights == Weights && Asset->InfluenceAnchors == Indices);
            const FString Path = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("CanonicalBake_"), TEXT(".json"));
            TestTrue(TEXT("Canonical source snapshot exported"), Asset->ExportSnapshot(Path));
            FString Text;
            TSharedPtr<FJsonObject> Snapshot;
            if (!FFileHelper::LoadFileToString(Text, *Path) ||
                !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Snapshot))
            { AddError(TEXT("Could not read the exported canonical snapshot.")); return false; }
            TestFalse(TEXT("New snapshot does not need gender"), Snapshot->HasField(TEXT("gender")));
            TestEqual(TEXT("Explicit topology marker"), Snapshot->GetStringField(TEXT("body_topology")), FString(TEXT("huma_v1_canonical_quads")));
            const auto& Triangles = Snapshot->GetArrayField(TEXT("body_triangles"));
            TestEqual(TEXT("Shared fan contains two triangles per quad"), Triangles.Num(), Asset->BodyQuads.Num() / 4 * 6);
            for (int32 Quad = 0; Quad < Asset->BodyQuads.Num() / 4; ++Quad)
                for (int32 Corner = 0; Corner < 6; ++Corner)
                {
                    const int32 Fan[] = {0, 1, 2, 0, 2, 3};
                    if (int32(Triangles[Quad * 6 + Corner]->AsNumber()) != Asset->BodyQuads[Quad * 4 + Fan[Corner]])
                    { AddError(TEXT("Canonical snapshot did not use the shared fan.")); return false; }
                }
            IFileManager::Get().Delete(*Path);
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMAClothLegacyTopologyTest, "HUMAClothBind.CanonicalBake.LegacyImport",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHUMAClothLegacyTopologyTest::RunTest(const FString&)
{
    auto* Source = LoadObject<UHUMAClothBindAsset>(nullptr, TEXT("/Game/HUMAClothBind/Bindings/CB_Male_buttons"));
    if (!TestNotNull(TEXT("Legacy sample"), Source)) return false;
    TestFalse(TEXT("Legacy sample has no automatic topology migration"), Source->bCanonicalBodyTopology);
    const FString Path = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("LegacyTopology_"), TEXT(".json"));
    if (!TestTrue(TEXT("Legacy package exported"), Source->ExportBakeData(Path))) return false;
    TStrongObjectPtr<UHUMAClothBindAsset> Imported(NewObject<UHUMAClothBindAsset>());
    Imported->BodyMesh = Source->BodyMesh;
    Imported->GarmentMesh = Source->GarmentMesh;
    Imported->GarmentToBody = Source->GarmentToBody;
    Imported->SigmaMin = Source->SigmaMin;
    Imported->DistanceScale = Source->DistanceScale;
    Imported->Gender = Source->Gender;
    TestTrue(TEXT("Legacy JSON still imports"), Imported->ImportBake(Path));
    FString Error;
    TestTrue(TEXT("Legacy signature/current checks preserved"), Imported->IsBakeCurrent(Error));
    TestFalse(TEXT("Legacy table stays legacy"), Imported->bCanonicalBodyTopology);
    TestTrue(TEXT("Legacy weights unchanged"), Imported->InfluenceWeights == Source->InfluenceWeights);
    Imported->Bake();
    TestTrue(TEXT("Explicit re-Bake upgrades to canonical mode"), Imported->bCanonicalBodyTopology && Imported->IsBakeCurrent(Error));
    TestTrue(TEXT("Canonical package exported"), Imported->ExportBakeData(Path));
    TStrongObjectPtr<UHUMAClothBindAsset> RoundTrip(DuplicateObject<UHUMAClothBindAsset>(Source, GetTransientPackage()));
    TestTrue(TEXT("Canonical JSON imports over legacy settings"), RoundTrip->ImportBake(Path));
    TestTrue(TEXT("Canonical mode survives JSON roundtrip"), RoundTrip->bCanonicalBodyTopology && RoundTrip->IsBakeCurrent(Error));
    IFileManager::Get().Delete(*Path);
    return true;
}
#endif
