#include "HUMAClothPreparationSubsystem.h"

#include "HUMAClothBindAsset.h"
#include "Engine/World.h"
#include "Engine/SkeletalMesh.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

bool UHUMAClothPreparationSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
    const UWorld* World = Cast<UWorld>(Outer);
    return World && World->IsGameWorld() && Super::ShouldCreateSubsystem(Outer);
}

UHUMAClothPreparationSubsystem* UHUMAClothPreparationSubsystem::GetForWorld(UObject* Context)
{
    UWorld* World = Context ? Context->GetWorld() : nullptr;
    return World ? World->GetSubsystem<UHUMAClothPreparationSubsystem>() : nullptr;
}

void UHUMAClothPreparationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
#if WITH_EDITOR
    ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddLambda(
        [this](UObject* Object) { InvalidateSource(Object); });
    PropertyHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda(
        [this](UObject* Object, FPropertyChangedEvent&) { InvalidateSource(Object); });
    TransactionHandle = FCoreUObjectDelegates::OnObjectTransacted.AddLambda(
        [this](UObject* Object, const FTransactionObjectEvent&) { InvalidateSource(Object); });
#endif
}

void UHUMAClothPreparationSubsystem::Deinitialize()
{
#if WITH_EDITOR
    FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle);
    FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyHandle);
    FCoreUObjectDelegates::OnObjectTransacted.Remove(TransactionHandle);
#endif
    Entries.Reset();
    Super::Deinitialize();
}

bool UHUMAClothPreparationSubsystem::Prepare(
    UHUMAClothBindAsset* Asset, USkeletalMesh* Mesh, TArray<int32>& Mapping, FString& Error)
{
    check(IsInGameThread());
    Error.Reset();
    if (!IsValid(Asset) || !IsValid(Mesh))
    {
        Error = TEXT("Cloth preparation requires valid Bind and body mesh assets.");
        return false;
    }
    for (const FEntry& Entry : Entries)
    {
        if (Entry.Asset == Asset && Entry.Mesh == Mesh)
        {
            Mapping = Entry.Mapping;
            ++CacheHits;
            return true;
        }
    }
    ++CacheMisses;
    const double Start = FPlatformTime::Seconds();
    const bool bValid = Asset->IsBakeCurrent(Error);
    ValidationMilliseconds += (FPlatformTime::Seconds() - Start) * 1000;
    if (!bValid) return false;
    const double MappingStart = FPlatformTime::Seconds();
    bool bMapped = false;
    for (const FEntry& Existing : Entries)
    {
        const UHUMAClothBindAsset* Other = Existing.Asset.Get();
        if (Other && Existing.Mesh == Mesh && Other->BodyMesh == Asset->BodyMesh
            && Other->BodyLODIndex == Asset->BodyLODIndex && Other->BodyQuads == Asset->BodyQuads
            && Other->CanonicalToRender == Asset->CanonicalToRender)
        {
            Mapping = Existing.Mapping;
            ++SharedMappingReuses;
            bMapped = true;
            break;
        }
    }
    if (!bMapped) bMapped = Asset->BuildSourceMapping(Mesh, Mapping, Error);
    MappingMilliseconds += (FPlatformTime::Seconds() - MappingStart) * 1000;
    if (!bMapped) return false;
    FEntry& Entry = Entries.AddDefaulted_GetRef();
    Entry.Asset = Asset;
    Entry.Mesh = Mesh;
    Entry.Mapping = Mapping;
    return true;
}

void UHUMAClothPreparationSubsystem::InvalidatePreparation()
{
    check(IsInGameThread());
    Entries.Reset();
}

bool UHUMAClothPreparationSubsystem::PrepareLODs(
    UHUMAClothBindAsset* Asset, USkeletalMesh* Mesh, FHUMAClothSourceLODs& LODs, FString& Error)
{
    LODs.Reset();
    TArray<int32> Mapping;
    if (!Prepare(Asset, Mesh, Mapping, Error)) return false;
    FEntry* Entry = Entries.FindByPredicate([&](const FEntry& Item) { return Item.Asset == Asset && Item.Mesh == Mesh; });
    check(Entry);
    if (!Entry->LODs.IsEmpty()) { LODs = Entry->LODs; return true; }
    if (Mesh != Asset->BodyMesh)
    {
        for (const FEntry& Other : Entries)
        {
            const auto* OtherAsset = Other.Asset.Get();
            if (&Other != Entry && OtherAsset && Other.Mesh == Mesh && !Other.LODs.IsEmpty()
                && OtherAsset->BodyMesh == Asset->BodyMesh && OtherAsset->BodyLODIndex == Asset->BodyLODIndex
                && Other.Mapping == Mapping)
            {
                Entry->LODs = Other.LODs;
                LODs = Entry->LODs;
                ++SharedLODTransferReuses;
                return true;
            }
        }
    }
    const double Start = FPlatformTime::Seconds();
    const bool Prepared = Asset->BuildSourceLODs(Mesh, LODs, Error);
    MappingMilliseconds += (FPlatformTime::Seconds() - Start) * 1000;
    if (!Prepared) return false;
    Entry->LODs = LODs;
    return true;
}

void UHUMAClothPreparationSubsystem::InvalidateSource(const UObject* Object)
{
    if (!Object) return;
    Entries.RemoveAll([Object](const FEntry& Entry)
    {
        const UHUMAClothBindAsset* Asset = Entry.Asset.Get();
        if (!Asset || !Entry.Mesh.IsValid()) return true;
        if (Object == Entry.Mesh.Get() || Object == Asset || Object->IsIn(Asset)
            || Object == Asset->BodyMesh || Object == Asset->GarmentMesh) return true;
        return (Asset->BodyMesh && Object->IsIn(Asset->BodyMesh))
            || (Asset->GarmentMesh && Object->IsIn(Asset->GarmentMesh));
    });
}

void UHUMAClothPreparationSubsystem::InvalidateAssetEverywhere(const UObject* Object)
{
    check(IsInGameThread());
    for (TObjectIterator<UHUMAClothPreparationSubsystem> It; It; ++It)
        if (!It->IsTemplate()) It->InvalidateSource(Object);
}
