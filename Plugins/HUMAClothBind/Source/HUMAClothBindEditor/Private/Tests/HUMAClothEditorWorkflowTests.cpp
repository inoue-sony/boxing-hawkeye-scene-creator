#include "HUMAClothBindAssetEditor.h"
#include "HUMAClothBindAsset.h"
#include "HUMAClothBindComponent.h"
#include "HUMAClothBindPreviewSettings.h"
#include "Animation/AnimationAsset.h"
#include "Components/SkeletalMeshComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor/EditorPerformanceSettings.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
int32 ActorCount(UWorld* World)
{
    int32 Count = 0;
    for (TActorIterator<AActor> It(World); It; ++It) if (IsValid(*It)) ++Count;
    return Count;
}

UHUMAClothBindAsset* SampleBind()
{
    return LoadObject<UHUMAClothBindAsset>(nullptr, TEXT("/Game/HUMAClothBind/Bindings/CB_Male_buttons"));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMAClothEditorPreviewTest, "HUMAClothBind.EditorWorkflow.AssetPreview",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHUMAClothEditorPreviewTest::RunTest(const FString&)
{
    auto* Source = SampleBind();
    if (!TestNotNull(TEXT("Sample Bind"), Source)) return false;
    UWorld* Level = GEditor->GetEditorWorldContext().World();
    const int32 Before = ActorCount(Level);
    TStrongObjectPtr<UHUMAClothBindAsset> Asset(DuplicateObject<UHUMAClothBindAsset>(Source, GetTransientPackage()));
    TStrongObjectPtr<UHUMAClothBindPreviewSettings> Settings(NewObject<UHUMAClothBindPreviewSettings>());
    Asset->bBakeAllBodyLODs = false;
    Asset->BakeMethod = EHUMAClothBindMethod::ClothGraph;
    const auto Scene = MakeShared<FHUMAClothBindPreviewScene>(Asset.Get(), Settings.Get());
    TestTrue(TEXT("Isolated EditorPreview world"), Scene->GetWorld() != Level && Scene->GetWorld()->WorldType == EWorldType::EditorPreview);
    TestTrue(TEXT("Unbaked settings explicitly show rest garment"), Scene->GetStatus().StartsWith(TEXT("Rest garment only")));
    Asset->Bake();
    FString Error;
    TestTrue(TEXT("Level-independent Bake is current"), Asset->IsBakeCurrent(Error));
    TestEqual(TEXT("Selected method baked"), Asset->BindMethod, FString(TEXT("cloth_graph")));
    Scene->Tick(1.f / 30.f);
    auto* Garment = Scene->GetGarmentActor();
    TestNotNull(TEXT("Preview garment actor"), Garment);
    if (Garment)
        TestTrue(TEXT("Bake updates the existing preview"), Garment->ClothBind->IsBindingInitialized());
    auto* Dynamic = AHUMAClothPreviewActor::SpawnBoundGarment(Asset.Get(), Scene->GetBodyComponent());
    if (TestNotNull(TEXT("Spawn also initializes in an editor world without BeginPlay"), Dynamic))
    {
        TestEqual(TEXT("Editor spawn initializes once"), Dynamic->ClothBind->InitializationCount, 1);
        Dynamic->Destroy();
    }
    Settings->MorphTarget = TEXT("DoesNotExist");
    Scene->ApplyControls();
    TestTrue(TEXT("Invalid Morph is visible"), Scene->GetStatus().Contains(TEXT("no Morph Target")));
    Settings->MorphTarget = NAME_None;
    Scene->ApplyControls();
    TestFalse(TEXT("Correcting Morph clears error"), Scene->GetStatus().Contains(TEXT("no Morph Target")));
    TestEqual(TEXT("Active level actors are unchanged"), ActorCount(Level), Before);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMAClothRuntimeSpawnTest, "HUMAClothBind.EditorWorkflow.RuntimeSpawn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHUMAClothRuntimeSpawnTest::RunTest(const FString&)
{
    auto* Asset = SampleBind();
    if (!TestNotNull(TEXT("Sample Bind"), Asset)) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL());
    World->BeginPlay();
    // This isolated world has no GameMode to transition the play flag.
    World->SetBegunPlay(true);
    AActor* Body = World->SpawnActor<AActor>();
    TestTrue(TEXT("Source actor begins play before garment spawn"), Body->HasActorBegunPlay());
    auto* OtherComponent = NewObject<USkeletalMeshComponent>(Body);
    Body->AddInstanceComponent(OtherComponent);
    Body->SetRootComponent(OtherComponent);
    OtherComponent->RegisterComponent();
    auto* Source = NewObject<USkeletalMeshComponent>(Body);
    Body->AddInstanceComponent(Source);
    Source->SetSkeletalMeshAsset(Asset->BodyMesh);
    Source->RegisterComponent();
    const FString Signature = Asset->BakeSignature;
    auto* Garment = AHUMAClothPreviewActor::SpawnBoundGarment(Asset, Source);
    if (TestNotNull(TEXT("Dynamic garment spawned after BeginPlay"), Garment))
    {
        TestTrue(TEXT("Explicit component is used, not the first component"), Garment->ClothBind->GetSourceBodyComponent() == Source);
        TestTrue(TEXT("Runtime binding initialized"), Garment->ClothBind->IsBindingInitialized());
        TestEqual(TEXT("No double initialization at BeginPlay"), Garment->ClothBind->InitializationCount, 1);
        TestEqual(TEXT("No Runtime Bake"), Asset->BakeSignature, Signature);
        Garment->Destroy();
        TestFalse(TEXT("Destruction clears GPU binding"), Garment->ClothBind->IsBindingInitialized());
    }
    AddExpectedError(TEXT("Spawn Bound Garment requires"), EAutomationExpectedErrorFlags::Contains, 1);
    TestNull(TEXT("Missing asset is rejected"), AHUMAClothPreviewActor::SpawnBoundGarment(nullptr, Source));
    World->EndPlay(EEndPlayReason::Quit);
    World->DestroyWorld(false);
    GEngine->DestroyWorldContext(World);
    return true;
}

namespace
{
class FWaitForBindEditorGPU : public IAutomationLatentCommand
{
public:
    FWaitForBindEditorGPU(FAutomationTestBase* InTest, TSharedPtr<FHUMAClothBindAssetEditor> InEditor,
        UHUMAClothBindAsset* InAsset, UWorld* InLevel, int32 InActorCount)
        : Test(InTest), Editor(InEditor), Asset(InAsset), Level(InLevel), LevelActorCount(InActorCount),
          Start(FPlatformTime::Seconds()),
          Path(FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("HUMAClothAssetEditor_"), TEXT(".json")))
    {
        FParse::Value(FCommandLine::Get(), TEXT("HUMAClothEditorTimeout="), Timeout);
        bPreviousThrottle = GetDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground;
    }

    virtual ~FWaitForBindEditorGPU() override
    {
        GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground = bPreviousThrottle;
    }

    virtual bool Update() override
    {
        // Offscreen editors have no focus; UE otherwise disables realtime viewports.
        GetMutableDefault<UEditorPerformanceSettings>()->bThrottleCPUWhenNotForeground = false;
        const auto Scene = Editor->GetPreviewScene();
        auto* Garment = Scene->GetGarmentActor();
        if (FPlatformTime::Seconds() - Start > Timeout)
        {
            Test->AddError(FString::Printf(TEXT("Asset editor GPU preview timed out (frames=%lld, requested=%d, %s): %s"),
                Garment ? Garment->ClothBind->CapturedFrames : 0, bRequested,
                *Editor->GetViewportStatus(), *Scene->GetStatus()));
            return Finish();
        }
        if (!Garment || Garment->ClothBind->CapturedFrames < 10) return false;
        if (!bRequested)
        {
            if (!Garment->ClothBind->RequestGPUSnapshot(Path))
            {
                Test->AddError(TEXT("Asset editor GPU snapshot was rejected."));
                return Finish();
            }
            bRequested = true;
            return false;
        }
        FString Text;
        TSharedPtr<FJsonObject> Snapshot;
        if (!FFileHelper::LoadFileToString(Text, *Path) ||
            !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Snapshot)) return false;
        TArray<FVector3f> Body;
        const auto& Rendered = Snapshot->GetArrayField(TEXT("rendered_body_positions"));
        double RenderedError = 0;
        for (const auto& Value : Snapshot->GetArrayField(TEXT("body_positions")))
        {
            const auto& Row = Value->AsArray();
            const auto& Other = Rendered[Body.Num()]->AsArray();
            for (int32 Axis = 0; Axis < 3; ++Axis)
                RenderedError = FMath::Max(RenderedError, FMath::Abs(Row[Axis]->AsNumber() - Other[Axis]->AsNumber()));
            Body.Emplace(Row[0]->AsNumber(), Row[1]->AsNumber(), Row[2]->AsNumber());
        }
        TArray<FVector> Expected;
        FString Error;
        if (!Asset->Evaluate(Body, Expected, Error))
        {
            Test->AddError(Error);
            return Finish();
        }
        const auto& Actual = Snapshot->GetArrayField(TEXT("cloth_positions"));
        Test->TestEqual(TEXT("Preview GPU writes all garment vertices"), Actual.Num(), Expected.Num());
        double Maximum = 0;
        for (int32 Index = 0; Index < FMath::Min(Actual.Num(), Expected.Num()); ++Index)
        {
            const auto& Row = Actual[Index]->AsArray();
            const FVector Position(Row[0]->AsNumber(), Row[1]->AsNumber(), Row[2]->AsNumber());
            Maximum = FMath::Max(Maximum, (Position - Expected[Index]).GetAbsMax());
        }
        Test->TestTrue(TEXT("Asset editor GPU/CPU error < 3e-4 cm"), Maximum < 3.e-4);
        Test->TestTrue(TEXT("Asset editor reads the rendered body < 1e-3 cm"), RenderedError < 1.e-3);
        Test->AddInfo(FString::Printf(TEXT("Asset editor GPU/CPU %.9g cm; rendered body %.9g cm; snapshot %s"),
            Maximum, RenderedError, *Path));
        return Finish();
    }
private:
    bool Finish()
    {
        Test->TestEqual(TEXT("Opening and rendering the asset editor leaves the level unchanged"), ActorCount(Level), LevelActorCount);
        Editor->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed);
        return true;
    }
    FAutomationTestBase* Test;
    TSharedPtr<FHUMAClothBindAssetEditor> Editor;
    TStrongObjectPtr<UHUMAClothBindAsset> Asset;
    UWorld* Level;
    int32 LevelActorCount;
    double Start;
    double Timeout = 120;
    FString Path;
    bool bRequested = false;
    bool bPreviousThrottle = true;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMAClothAssetEditorGPUTest, "HUMAClothBind.EditorGPU.AssetViewport",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHUMAClothAssetEditorGPUTest::RunTest(const FString&)
{
    if (GUsingNullRHI) { AddError(TEXT("This test requires a real RHI.")); return false; }
    auto* Source = SampleBind();
    if (!TestNotNull(TEXT("Sample Bind"), Source)) return false;
    auto* Asset = DuplicateObject<UHUMAClothBindAsset>(Source, GetTransientPackage());
    Asset->bBakeAllBodyLODs = false;
    Asset->Bake();
    FString Error;
    if (!TestTrue(TEXT("Fresh preview Bake"), Asset->IsBakeCurrent(Error))) return false;
    UWorld* Level = GEditor->GetEditorWorldContext().World();
    const int32 Before = ActorCount(Level);
    auto Editor = MakeShared<FHUMAClothBindAssetEditor>();
    Editor->Init(Asset, EToolkitMode::Standalone, nullptr);
    auto* Settings = Editor->GetPreviewSettings();
    Settings->Animation = LoadObject<UAnimationAsset>(nullptr, TEXT("/Game/Sample_HUMA/Resource/Animations/rig_animation"));
    Settings->AnimationTime = 2;
    Settings->bPlaying = true;
    Settings->MorphTarget = TEXT("jawOpen");
    Settings->MorphWeight = 0.6f;
    Editor->GetPreviewScene()->Rebuild();
    ADD_LATENT_AUTOMATION_COMMAND(FWaitForBindEditorGPU(this, Editor, Asset, Level, Before));
    return true;
}
#endif
