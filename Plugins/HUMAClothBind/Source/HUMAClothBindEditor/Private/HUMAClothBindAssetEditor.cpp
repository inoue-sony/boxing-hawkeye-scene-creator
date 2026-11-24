#include "HUMAClothBindAssetEditor.h"
#include "HUMAClothBindAsset.h"
#include "HUMAClothBindComponent.h"
#include "HUMAClothBindPreviewSettings.h"
#include "HUMAClothBindBatchBaker.h"
#include "AssetRegistry/AssetData.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Editor.h"
#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/SkeletalMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "IDetailsView.h"
#include "PropertyEditorModule.h"
#include "SEditorViewport.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Text/STextBlock.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SWindow.h"

namespace
{
const FName ViewportTab(TEXT("HUMAClothBindViewport"));
const FName DetailsTab(TEXT("HUMAClothBindDetails"));
const FName PreviewSettingsTab(TEXT("HUMAClothBindPreviewSettings"));

bool ChooseBatchFolder(FString& OutputFolder)
{
    FContentBrowserModule& Browser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
    bool bAccepted = false;
    auto Window = SNew(SWindow).Title(FText::FromString(TEXT("Choose Batch Bind Output Folder")))
        .ClientSize(FVector2D(520, 500)).SupportsMinimize(false).SupportsMaximize(false);
    const TWeakPtr<SWindow> WeakWindow = Window;
    FPathPickerConfig Config;
    Config.DefaultPath = OutputFolder;
    Config.bAddDefaultPath = true;
    Config.bAllowReadOnlyFolders = false;
    Config.OnPathSelected = FOnPathSelected::CreateLambda([&OutputFolder](const FString& Path) { OutputFolder = Path; });
    Window->SetContent(SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(8)
        [SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(TEXT("Create one CB_<Garment> asset per selected mesh. Choose a folder under /Game.")))]
        + SVerticalBox::Slot().FillHeight(1).Padding(8)[Browser.Get().CreatePathPicker(Config)]
        + SVerticalBox::Slot().AutoHeight().Padding(8)
        [SNew(STextBlock).Text_Lambda([&OutputFolder] { return FText::FromString(OutputFolder); })]
        + SVerticalBox::Slot().AutoHeight().Padding(8)
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1)
            [SNew(SButton).Text(FText::FromString(TEXT("Use Folder")))
                .IsEnabled_Lambda([&OutputFolder]
                {
                    return OutputFolder == TEXT("/Game") ||
                        (OutputFolder.StartsWith(TEXT("/Game/")) && FPackageName::IsValidLongPackageName(OutputFolder));
                })
                .OnClicked_Lambda([WeakWindow, &bAccepted]
                {
                    bAccepted = true;
                    if (auto Pinned = WeakWindow.Pin()) Pinned->RequestDestroyWindow();
                    return FReply::Handled();
                })]
            + SHorizontalBox::Slot().AutoWidth().Padding(8, 0)
            [SNew(SButton).Text(FText::FromString(TEXT("Cancel"))).OnClicked_Lambda([WeakWindow]
                {
                    if (auto Pinned = WeakWindow.Pin()) Pinned->RequestDestroyWindow();
                    return FReply::Handled();
                })]]);
    GEditor->EditorAddModalWindow(Window);
    return bAccepted;
}
}

FHUMAClothBindPreviewScene::FHUMAClothBindPreviewScene(UHUMAClothBindAsset* InAsset, UHUMAClothBindPreviewSettings* InSettings)
    : FAdvancedPreviewScene(FPreviewScene::ConstructionValues()), Asset(InAsset), Settings(InSettings)
{
    SetFloorVisibility(true, true);
    Rebuild();
}

FHUMAClothBindPreviewScene::~FHUMAClothBindPreviewScene()
{
    Clear();
}

USkeletalMeshComponent* FHUMAClothBindPreviewScene::GetBodyComponent() const
{
    return BodyActor.IsValid() ? BodyActor->GetSkeletalMeshComponent() : nullptr;
}

AHUMAClothPreviewActor* FHUMAClothBindPreviewScene::GetGarmentActor() const
{
    return GarmentActor.Get();
}

void FHUMAClothBindPreviewScene::Clear()
{
    if (GarmentActor.IsValid()) GarmentActor->Destroy();
    if (RestGarment.IsValid())
    {
        RemoveComponent(RestGarment.Get());
        RestGarment->DestroyComponent();
    }
    if (BodyActor.IsValid()) BodyActor->Destroy();
    GarmentActor.Reset(); BodyActor.Reset(); RestGarment.Reset();
    AppliedMorph = NAME_None;
}

void FHUMAClothBindPreviewScene::Rebuild()
{
    Clear();
    SetupError.Reset();
    if (!Asset.IsValid() || !Settings.IsValid()) return;
    USkeletalMesh* Mesh = Settings->PreviewBodyMesh ? Settings->PreviewBodyMesh.Get() : Asset->BodyMesh.Get();
    if (!Mesh) { SetupError = TEXT("Select a Body Mesh in Bind Settings."); return; }
    ASkeletalMeshActor* Body = GetWorld()->SpawnActor<ASkeletalMeshActor>();
    if (!Body) { SetupError = TEXT("Could not create the preview body."); return; }
    BodyActor = Body;
    auto* Component = Body->GetSkeletalMeshComponent();
    Component->SetSkeletalMeshAsset(Mesh);
    Component->SetAnimationMode(EAnimationMode::AnimationSingleNode);
    Component->SetUpdateAnimationInEditor(true);
    if (Settings->Animation)
    {
        if (!Settings->Animation->GetSkeleton() || !Settings->Animation->GetSkeleton()->IsCompatibleMesh(Mesh))
            SetupError = TEXT("Preview animation is incompatible with this body's skeleton.");
        else Component->SetAnimation(Settings->Animation);
    }
    if (auto* StaticMesh = Cast<UStaticMesh>(Asset->GarmentMesh))
    {
        auto* Rest = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
        Rest->SetStaticMesh(StaticMesh);
        RestGarment = Rest;
    }
    else if (auto* SkeletalMesh = Cast<USkeletalMesh>(Asset->GarmentMesh))
    {
        auto* Rest = NewObject<USkeletalMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
        Rest->SetSkeletalMeshAsset(SkeletalMesh);
        RestGarment = Rest;
    }
    if (RestGarment.IsValid())
    {
        RestGarment->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        AddComponent(RestGarment.Get(), Asset->GarmentToBody);
    }
    GarmentActor = GetWorld()->SpawnActor<AHUMAClothPreviewActor>();
    if (GarmentActor.IsValid())
    {
        auto* Bind = GarmentActor->ClothBind.Get();
        Bind->BindAsset = Asset.Get();
        Bind->SourceBodyComponent = Component;
        Bind->SourceBodyActor = Body;
        FString Error;
        if (Asset->IsBakeCurrent(Error)) Bind->InitializeBinding(Asset.Get(), Component);
        else Bind->PreviewStatus = Error;
    }
    else SetupError = TEXT("Could not create the preview garment.");
    ApplyControls();
}

void FHUMAClothBindPreviewScene::ApplyControls()
{
    auto* Body = GetBodyComponent();
    if (!Body || !Settings.IsValid()) return;
    MorphError.Reset();
    if (!AppliedMorph.IsNone()) Body->SetMorphTarget(AppliedMorph, 0);
    AppliedMorph = Settings->MorphTarget;
    if (!AppliedMorph.IsNone())
    {
        if (Body->GetSkeletalMeshAsset()->FindMorphTarget(AppliedMorph))
            Body->SetMorphTarget(AppliedMorph, Settings->MorphWeight);
        else MorphError = TEXT("Preview body has no Morph Target named ") + AppliedMorph.ToString();
    }
    PlaybackTime = Settings->AnimationTime;
    Body->Stop();
    Body->SetPosition(PlaybackTime, false);
    Body->TickAnimation(0, false);
    Body->RefreshBoneTransforms();
    Body->UpdateComponentToWorld();
    Body->MarkRenderDynamicDataDirty();
}

void FHUMAClothBindPreviewScene::Tick(float DeltaTime)
{
    FAdvancedPreviewScene::Tick(DeltaTime);
    auto* Body = GetBodyComponent();
    if (Body && Settings.IsValid() && Settings->bPlaying && Settings->Animation && SetupError.IsEmpty())
    {
        const float Length = Settings->Animation->GetPlayLength();
        PlaybackTime = Length > 0 ? FMath::Fmod(PlaybackTime + DeltaTime, Length) : 0;
        Settings->AnimationTime = PlaybackTime;
        Body->SetPosition(PlaybackTime, false);
        Body->TickAnimation(0, false);
        Body->RefreshBoneTransforms();
        Body->MarkRenderDynamicDataDirty();
    }
    GetWorld()->Tick(LEVELTICK_All, DeltaTime);
    if (RestGarment.IsValid())
        RestGarment->SetVisibility(!GarmentActor.IsValid() || !GarmentActor->ClothBind->IsBindingInitialized());
}

FString FHUMAClothBindPreviewScene::GetStatus() const
{
    if (!SetupError.IsEmpty()) return SetupError;
    if (!MorphError.IsEmpty()) return MorphError;
    if (!Asset.IsValid()) return TEXT("No Bind asset.");
    if (!Asset->GarmentMesh)
        return TEXT("Select Garment Mesh for a single preview, or select garment meshes in the Content Browser and use Batch Bind Selected Garments.");
    if (!GarmentActor.IsValid()) return TEXT("No preview garment.");
    if (!GarmentActor->ClothBind->IsBindingInitialized())
        return TEXT("Rest garment only / ") + GarmentActor->ClothBind->PreviewStatus;
    return GarmentActor->ClothBind->PreviewStatus;
}

class SHUMAClothBindViewport : public SEditorViewport
{
public:
    SLATE_BEGIN_ARGS(SHUMAClothBindViewport) {}
    SLATE_END_ARGS()
    void Construct(const FArguments&, TSharedPtr<FHUMAClothBindPreviewScene> InScene)
    {
        Scene = InScene;
        SEditorViewport::Construct(SEditorViewport::FArguments());
    }
protected:
    virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override
    {
        class FClient : public FEditorViewportClient
        {
        public:
            FClient(TSharedPtr<FHUMAClothBindPreviewScene> InScene, const TSharedRef<SEditorViewport>& Widget)
                : FEditorViewportClient(nullptr, InScene.Get(), Widget), OwnedScene(InScene) {}
        private:
            TSharedPtr<FHUMAClothBindPreviewScene> OwnedScene;
        };
        auto ViewClient = MakeShared<FClient>(Scene, SharedThis(this));
        ViewClient->SetViewLocation(FVector(250, -350, 150));
        ViewClient->SetViewRotation((FVector(0, 0, 90) - ViewClient->GetViewLocation()).Rotation());
        ViewClient->SetViewMode(VMI_Lit);
        ViewClient->SetRealtime(true);
        return ViewClient;
    }
private:
    TSharedPtr<FHUMAClothBindPreviewScene> Scene;
};

FString FHUMAClothBindAssetEditor::GetViewportStatus() const
{
    if (!Viewport.IsValid()) return TEXT("Viewport widget not created");
    const FVector2D Size = Viewport->GetCachedGeometry().GetLocalSize();
    return FString::Printf(TEXT("realtime=%d visible=%d size=%.0fx%.0f time=%.3f"),
        Viewport->IsRealtime(), Viewport->IsVisible(), Size.X, Size.Y, PreviewSettings->AnimationTime);
}

void FHUMAClothBindAssetEditor::Init(UHUMAClothBindAsset* InAsset, EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& Host)
{
    Asset = InAsset;
    PreviewSettings.Reset(NewObject<UHUMAClothBindPreviewSettings>());
    PreviewScene = MakeShared<FHUMAClothBindPreviewScene>(Asset, PreviewSettings.Get());
    FPropertyEditorModule& Properties = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
    FDetailsViewArgs Args;
    Args.bHideSelectionTip = true;
    Args.NotifyHook = nullptr;
    Details = Properties.CreateDetailView(Args);
    Details->SetObject(Asset);
    Details->OnFinishedChangingProperties().AddSP(this, &FHUMAClothBindAssetEditor::OnAssetChanged);
    PreviewDetails = Properties.CreateDetailView(Args);
    PreviewDetails->SetObject(PreviewSettings.Get());
    PreviewDetails->OnFinishedChangingProperties().AddSP(this, &FHUMAClothBindAssetEditor::OnPreviewChanged);
    const auto Layout = FTabManager::NewLayout(TEXT("HUMAClothBindEditor_v1"))
        ->AddArea(FTabManager::NewPrimaryArea()->SetOrientation(Orient_Horizontal)
            ->Split(FTabManager::NewStack()->SetSizeCoefficient(0.65f)->AddTab(ViewportTab, ETabState::OpenedTab)->SetHideTabWell(true))
            ->Split(FTabManager::NewSplitter()->SetOrientation(Orient_Vertical)->SetSizeCoefficient(0.35f)
                ->Split(FTabManager::NewStack()->SetSizeCoefficient(0.65f)->AddTab(DetailsTab, ETabState::OpenedTab))
                ->Split(FTabManager::NewStack()->SetSizeCoefficient(0.35f)->AddTab(PreviewSettingsTab, ETabState::OpenedTab))));
    InitAssetEditor(Mode, Host, TEXT("HUMAClothBindEditor"), Layout, true, true, Asset);
    auto Extender = MakeShared<FExtender>();
    Extender->AddToolBarExtension(TEXT("Asset"), EExtensionHook::After, GetToolkitCommands(),
        FToolBarExtensionDelegate::CreateLambda([this](FToolBarBuilder& Builder)
        {
            Builder.AddToolBarButton(FUIAction(FExecuteAction::CreateSP(this, &FHUMAClothBindAssetEditor::BakeAndSave)),
                NAME_None, FText::FromString(TEXT("Bake and Save")),
                FText::FromString(TEXT("Bake this asset without level actors, then save only after a successful Bake.")), FSlateIcon());
            Builder.AddToolBarButton(FUIAction(FExecuteAction::CreateSP(this, &FHUMAClothBindAssetEditor::BatchBindSelectedGarments)),
                NAME_None, FText::FromString(TEXT("Batch Bind Selected Garments")),
                FText::FromString(TEXT("Use this asset's Bind settings for garment meshes selected in the Content Browser. No level actors are required or placed.")), FSlateIcon());
            Builder.AddToolBarButton(FUIAction(FExecuteAction::CreateLambda([this] { PreviewScene->Rebuild(); })),
                NAME_None, FText::FromString(TEXT("Refresh Preview")), FText::FromString(TEXT("Recreate the isolated preview.")), FSlateIcon());
        }));
    AddToolbarExtender(Extender);
    RegenerateMenusAndToolbars();
}

void FHUMAClothBindAssetEditor::RegisterTabSpawners(const TSharedRef<FTabManager>& Manager)
{
    FAssetEditorToolkit::RegisterTabSpawners(Manager);
    Manager->RegisterTabSpawner(ViewportTab, FOnSpawnTab::CreateSP(this, &FHUMAClothBindAssetEditor::SpawnViewport))
        .SetDisplayName(FText::FromString(TEXT("Preview")));
    Manager->RegisterTabSpawner(DetailsTab, FOnSpawnTab::CreateSP(this, &FHUMAClothBindAssetEditor::SpawnDetails))
        .SetDisplayName(FText::FromString(TEXT("Bind Settings")));
    Manager->RegisterTabSpawner(PreviewSettingsTab, FOnSpawnTab::CreateSP(this, &FHUMAClothBindAssetEditor::SpawnPreviewSettings))
        .SetDisplayName(FText::FromString(TEXT("Preview Controls")));
}

void FHUMAClothBindAssetEditor::UnregisterTabSpawners(const TSharedRef<FTabManager>& Manager)
{
    Manager->UnregisterTabSpawner(ViewportTab);
    Manager->UnregisterTabSpawner(DetailsTab);
    Manager->UnregisterTabSpawner(PreviewSettingsTab);
    FAssetEditorToolkit::UnregisterTabSpawners(Manager);
}

TSharedRef<SDockTab> FHUMAClothBindAssetEditor::SpawnViewport(const FSpawnTabArgs&)
{
    return SNew(SDockTab)[SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(8)
        [SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] { return FText::FromString(PreviewScene->GetStatus()); })]
        + SVerticalBox::Slot().FillHeight(1)[SAssignNew(Viewport, SHUMAClothBindViewport, PreviewScene)]];
}

TSharedRef<SDockTab> FHUMAClothBindAssetEditor::SpawnDetails(const FSpawnTabArgs&)
{
    return SNew(SDockTab)[Details.ToSharedRef()];
}

TSharedRef<SDockTab> FHUMAClothBindAssetEditor::SpawnPreviewSettings(const FSpawnTabArgs&)
{
    return SNew(SDockTab)[PreviewDetails.ToSharedRef()];
}

void FHUMAClothBindAssetEditor::OnAssetChanged(const FPropertyChangedEvent&)
{
    PreviewScene->Rebuild();
}

void FHUMAClothBindAssetEditor::OnPreviewChanged(const FPropertyChangedEvent& Event)
{
    if (Event.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UHUMAClothBindPreviewSettings, PreviewBodyMesh) ||
        Event.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UHUMAClothBindPreviewSettings, Animation))
        PreviewScene->Rebuild();
    else PreviewScene->ApplyControls();
}

void FHUMAClothBindAssetEditor::BakeAndSave()
{
    Asset->Bake();
    FString Error;
    if (Asset->BakeStatus.StartsWith(TEXT("Baked")) && Asset->IsBakeCurrent(Error)) SaveAsset_Execute();
    else
    {
        FNotificationInfo Info(FText::FromString(TEXT("Cloth Bind Bake failed: ") + Asset->BakeStatus));
        Info.ExpireDuration = 8;
        FSlateNotificationManager::Get().AddNotification(Info);
        UE_LOG(LogTemp, Error, TEXT("HUMAClothBind editor Bake failed: %s"), *Asset->BakeStatus);
    }
    PreviewScene->Rebuild();
    Details->ForceRefresh();
}

void FHUMAClothBindAssetEditor::BatchBindSelectedGarments()
{
    FContentBrowserModule& Browser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
    TArray<FAssetData> Selected;
    Browser.Get().GetSelectedAssets(Selected);
    TArray<UObject*> Garments;
    for (const FAssetData& Entry : Selected)
        if (UObject* Object = Entry.GetAsset(); Object && Object != Asset->BodyMesh &&
            (Object->IsA<UStaticMesh>() || Object->IsA<USkeletalMesh>())) Garments.Add(Object);
    if (!Asset->BodyMesh || Garments.IsEmpty())
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(
            TEXT("Set Body Mesh in this editor, then select garment meshes in the Content Browser. You do not need to select a level actor.")));
        return;
    }
    FString Folder = FPackageName::GetLongPackagePath(Asset->GetOutermost()->GetName());
    if (Folder != TEXT("/Game") && !Folder.StartsWith(TEXT("/Game/"))) Folder = TEXT("/Game/HUMAClothBind/Bindings");
    if (!ChooseBatchFolder(Folder)) return;
    FHUMAClothBatchPlan Plan;
    FString Error;
    if (!PlanHUMAClothBatch(*Asset, Garments, Folder, Plan, Error))
    {
        UE_LOG(LogTemp, Error, TEXT("HUMAClothBind batch: %s"), *Error);
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
        return;
    }
    if (Plan.ExistingCount > 0 && FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(FString::Printf(
        TEXT("%d matching Bind assets already exist. Re-bake and overwrite them using this editor's common settings?\n\nOutput folder: %s"),
        Plan.ExistingCount, *Folder))) != EAppReturnType::Yes) return;
    const FHUMAClothBatchResult Result = BakeHUMAClothBatch(Plan, Plan.ExistingCount > 0);
    TArray<UObject*> Saved;
    for (UHUMAClothBindAsset* Output : Result.SavedAssets) Saved.Add(Output);
    if (!Saved.IsEmpty()) Browser.Get().SyncBrowserToAssets(Saved);
    FString Summary = FString::Printf(TEXT("Saved %d / %d Bind assets.\nOutput folder: %s\nNo level actors were placed."),
        Result.SavedAssets.Num(), Result.Requested, *Folder);
    if (Result.bCancelled) Summary += TEXT("\nCancelled. Completed assets remain saved.");
    for (int32 Index = 0; Index < FMath::Min(8, Result.Errors.Num()); ++Index)
        Summary += TEXT("\n") + Result.Errors[Index];
    if (Result.Errors.Num() > 8) Summary += TEXT("\nMore errors are listed in the Output Log.");
    UE_LOG(LogTemp, Display, TEXT("HUMAClothBind batch result: saved=%d requested=%d failed=%d cancelled=%d"),
        Result.SavedAssets.Num(), Result.Requested, Result.Errors.Num(), Result.bCancelled);
    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Summary));
    PreviewScene->Rebuild();
    Details->ForceRefresh();
}
