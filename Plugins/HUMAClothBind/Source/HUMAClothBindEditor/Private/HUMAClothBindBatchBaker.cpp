#include "HUMAClothBindBatchBaker.h"
#include "HUMAClothBindFactory.h"
#include "AssetToolsModule.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "ObjectTools.h"
#include "UObject/Package.h"

namespace
{
void CopySettings(const UHUMAClothBindAsset& From, UHUMAClothBindAsset& To)
{
    To.BodyMesh = From.BodyMesh;
    To.GarmentMesh = From.GarmentMesh;
    To.Gender = From.Gender;
    To.bCanonicalBodyTopology = From.bCanonicalBodyTopology;
    To.GarmentToBody = From.GarmentToBody;
    To.SigmaMin = From.SigmaMin;
    To.DistanceScale = From.DistanceScale;
    To.bBakeAllBodyLODs = From.bBakeAllBodyLODs;
    To.BodyLODIndex = From.BodyLODIndex;
    To.BakeMethod = From.BakeMethod;
    To.bBakeMethodInitialized = From.bBakeMethodInitialized;
}

void AddError(FHUMAClothBatchResult& Result, const FString& Error)
{
    Result.Errors.Add(Error);
    UE_LOG(LogTemp, Error, TEXT("HUMAClothBind batch: %s"), *Error);
}

bool MatchesPlannedOutput(const FHUMAClothBatchItem& Item, const UHUMAClothBindAsset& Settings)
{
    const FString ObjectPath = Item.PackageName + TEXT(".") + FPackageName::GetShortName(Item.PackageName);
    UObject* Current = FindObject<UObject>(nullptr, *ObjectPath);
    const auto* Existing = Item.Existing.Get();
    if (!Existing) return !Current && !FPackageName::DoesPackageExist(Item.PackageName);
    return IsValid(Existing) && Current == Existing && Existing->BodyMesh == Settings.BodyMesh &&
        Existing->GarmentMesh == Item.Garment.Get() && Existing->BodyLODIndex == 0;
}

struct FBatchScratchFiles
{
    TArray<FString> Paths;
    ~FBatchScratchFiles()
    {
        for (const FString& Path : Paths)
            if (IFileManager::Get().FileExists(*Path) && !IFileManager::Get().Delete(*Path))
                UE_LOG(LogTemp, Warning, TEXT("HUMAClothBind could not remove batch scratch file: %s"), *Path);
    }
};
}

bool PlanHUMAClothBatch(const UHUMAClothBindAsset& Settings, TConstArrayView<UObject*> Selection,
    const FString& OutputFolder, FHUMAClothBatchPlan& Plan, FString& Error)
{
    Plan = FHUMAClothBatchPlan();
    Error.Reset();
    if (!IsValid(Settings.BodyMesh) || Settings.BodyLODIndex != 0)
    { Error = TEXT("Batch Bind requires a reference Body Mesh and LOD0 Bind settings."); return false; }
    if ((Settings.BakeMethod != EHUMAClothBindMethod::BodyField && Settings.BakeMethod != EHUMAClothBindMethod::ClothGraph) ||
        !FMath::IsFinite(Settings.SigmaMin) || Settings.SigmaMin <= 0 ||
        !FMath::IsFinite(Settings.DistanceScale) || Settings.DistanceScale < 0 || !Settings.GarmentToBody.IsValid())
    { Error = TEXT("Invalid common Bind method, transform or blending settings."); return false; }
    FString Folder = OutputFolder;
    Folder.RemoveFromEnd(TEXT("/"));
    if (Folder != TEXT("/Game") && (!Folder.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(Folder)))
    { Error = TEXT("Choose a writable content folder under /Game."); return false; }
    TMap<FString, UObject*> Unique;
    for (UObject* Object : Selection)
        if (IsValid(Object) && Object != Settings.BodyMesh && (Object->IsA<UStaticMesh>() || Object->IsA<USkeletalMesh>()))
            Unique.Add(Object->GetPathName(), Object);
    if (Unique.IsEmpty())
    { Error = TEXT("Select one or more garment StaticMesh or SkeletalMesh assets in the Content Browser."); return false; }
    TArray<FString> Paths;
    Unique.GetKeys(Paths);
    Paths.Sort();
    FHUMAClothBatchPlan Candidate;
    TSet<FName> Outputs;
    for (const FString& Path : Paths)
    {
        UObject* Garment = Unique[Path];
        const FString Name = TEXT("CB_") + Garment->GetName();
        const FString Package = Folder / Name;
        if (!FPackageName::IsValidLongPackageName(Package))
        { Error = TEXT("Invalid output package name: ") + Package; return false; }
        if (Outputs.Contains(FName(*Package)))
        { Error = TEXT("Selected garments have duplicate names in the output folder: ") + Package; return false; }
        Outputs.Add(FName(*Package));
        const FString ObjectPath = Package + TEXT(".") + Name;
        UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath);
        if (!Existing && FPackageName::DoesPackageExist(Package))
        {
            Existing = LoadObject<UObject>(nullptr, *ObjectPath);
            if (!Existing) { Error = TEXT("Cannot load existing output: ") + ObjectPath; return false; }
        }
        auto* Bind = Cast<UHUMAClothBindAsset>(Existing);
        if (Existing && (!Bind || Existing->GetPathName() != ObjectPath || Bind->BodyMesh != Settings.BodyMesh ||
            Bind->GarmentMesh != Garment || Bind->BodyLODIndex != 0))
        { Error = TEXT("Output belongs to another asset, body or garment. Choose a different folder: ") + ObjectPath; return false; }
        FHUMAClothBatchItem Item;
        Item.Garment.Reset(Garment);
        Item.Existing.Reset(Bind);
        Item.PackageName = Package;
        Candidate.Items.Add(MoveTemp(Item));
        if (Bind) ++Candidate.ExistingCount;
    }
    Candidate.Settings.Reset(NewObject<UHUMAClothBindAsset>(GetTransientPackage(), NAME_None, RF_Transient));
    CopySettings(Settings, *Candidate.Settings);
    Plan = MoveTemp(Candidate);
    return true;
}

FHUMAClothBatchResult BakeHUMAClothBatch(const FHUMAClothBatchPlan& Plan, bool bAllowOverwrite, TFunction<bool()> ShouldCancel)
{
    FHUMAClothBatchResult Result;
    Result.Requested = Plan.Items.Num();
    if (!Plan.Settings || Plan.Items.IsEmpty())
    { AddError(Result, TEXT("No valid batch plan. Select garments and common Bind settings first.")); return Result; }
    if (Plan.ExistingCount > 0 && !bAllowOverwrite)
    { AddError(Result, TEXT("Existing Bind assets require explicit overwrite confirmation.")); return Result; }
    IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    TStrongObjectPtr<UHUMAClothBindFactory> Factory(NewObject<UHUMAClothBindFactory>());
    Factory->bEditAfterNew = false;
    FScopedSlowTask Progress(Plan.Items.Num(), FText::FromString(TEXT("Batch Bind selected garments (no level placement)")));
    Progress.MakeDialog(true);
    for (const FHUMAClothBatchItem& Item : Plan.Items)
    {
        if (Progress.ShouldCancel() || (ShouldCancel && ShouldCancel()))
        { Result.bCancelled = true; break; }
        if (!IsValid(Item.Garment.Get()) || !MatchesPlannedOutput(Item, *Plan.Settings))
        { AddError(Result, Item.PackageName + TEXT(": Output changed after planning. Run Batch Bind again to confirm the current output.")); continue; }
        Progress.EnterProgressFrame(1, FText::FromString(Item.Garment->GetName()));
        const FName CandidateName(*(TEXT("HUMAClothBatch_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
        TStrongObjectPtr<UHUMAClothBindAsset> Candidate(NewObject<UHUMAClothBindAsset>(GetTransientPackage(), CandidateName, RF_Transient));
        CopySettings(*Plan.Settings, *Candidate);
        Candidate->GarmentMesh = Item.Garment.Get();
        const FString ExportPath = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("HUMAClothBatch_"), TEXT(".json"));
        const FString NativeDirectory = FPaths::ProjectSavedDir() / TEXT("HUMAClothBind");
        FBatchScratchFiles Scratch{{ExportPath, NativeDirectory / (CandidateName.ToString() + TEXT("_source.json")),
            NativeDirectory / (CandidateName.ToString() + TEXT("_bake.json"))}};
        Candidate->Bake();
        FString Error;
        if (!Candidate->BakeStatus.StartsWith(TEXT("Baked")) || !Candidate->IsBakeCurrent(Error))
        { AddError(Result, Item.Garment->GetPathName() + TEXT(": ") + Candidate->BakeStatus + TEXT(" ") + Error); continue; }
        if (!Candidate->ExportBakeData(ExportPath))
        { AddError(Result, Item.Garment->GetPathName() + TEXT(": Could not export the fresh Bake.")); continue; }
        if (!MatchesPlannedOutput(Item, *Plan.Settings))
        { AddError(Result, Item.PackageName + TEXT(": Output changed during Bake. Run Batch Bind again to confirm the current output.")); continue; }
        UHUMAClothBindAsset* Output = Item.Existing.Get();
        const bool bNew = !Output;
        if (bNew)
            Output = Cast<UHUMAClothBindAsset>(Tools.CreateAsset(FPackageName::GetShortName(Item.PackageName),
                FPackageName::GetLongPackagePath(Item.PackageName), UHUMAClothBindAsset::StaticClass(), Factory.Get()));
        if (!Output)
        { AddError(Result, TEXT("Could not create Bind asset: ") + Item.PackageName); continue; }
        TStrongObjectPtr<UHUMAClothBindAsset> Previous(NewObject<UHUMAClothBindAsset>(GetTransientPackage(), NAME_None, RF_Transient));
        CopySettings(*Output, *Previous);
        const FString PreviousStatus = Output->BakeStatus;
        CopySettings(*Candidate, *Output);
        if (!Output->ImportBake(ExportPath))
        {
            Error = Output->BakeStatus;
            CopySettings(*Previous, *Output);
            Output->BakeStatus = PreviousStatus;
            AddError(Result, Item.PackageName + TEXT(": Import failed; previous settings retained. ") + Error);
            if (bNew && !ObjectTools::DeleteSingleObject(Output, false))
                UE_LOG(LogTemp, Warning, TEXT("HUMAClothBind could not discard unsaved output: %s"), *Item.PackageName);
            continue;
        }
        const TArray<UPackage*> Packages = {Output->GetPackage()};
        const auto Saved = FApp::IsUnattended()
            ? (UEditorLoadingAndSavingUtils::SavePackages(Packages, false) ? FEditorFileUtils::PR_Success : FEditorFileUtils::PR_Failure)
            : FEditorFileUtils::PromptForCheckoutAndSave(Packages, false, false);
        if (Saved == FEditorFileUtils::PR_Cancelled)
        {
            Result.bCancelled = true;
            Result.Errors.Add(Item.PackageName + TEXT(": Save cancelled; this Bake remains unsaved in memory."));
            UE_LOG(LogTemp, Warning, TEXT("HUMAClothBind batch save cancelled: %s"), *Item.PackageName);
            break;
        }
        const FString Filename = FPackageName::LongPackageNameToFilename(Item.PackageName, FPackageName::GetAssetPackageExtension());
        if (Saved != FEditorFileUtils::PR_Success || !IFileManager::Get().FileExists(*Filename))
        { AddError(Result, Item.PackageName + TEXT(": Could not save; this Bake remains unsaved in memory.")); continue; }
        Result.SavedAssets.Add(Output);
        UE_LOG(LogTemp, Display, TEXT("HUMAClothBind batch saved %s / %s"), *Output->GetPathName(), *Output->BindMethod);
    }
    return Result;
}
