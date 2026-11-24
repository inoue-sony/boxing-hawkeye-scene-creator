#include "HUMAClothBindComponent.h"
#include "HUMAClothBindAsset.h"
#include "HUMAClothReadback.h"
#include "HUMAClothGpuMeshComponent.h"
#include "HUMAClothGpuState.h"
#include "HUMAClothPreparationSubsystem.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/MeshDeformer.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "ProceduralMeshComponent.h"
#include "Misc/ScopeExit.h"
#include "Materials/MaterialInterface.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "UObject/UnrealType.h"
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Misc/AutomationTest.h"
#endif

namespace
{
struct FStandardSkinningLease
{
    int32 Users = 0;
    bool bPreviousOverride = false;
    TWeakObjectPtr<UMeshDeformer> Previous;
    TWeakObjectPtr<UMeshDeformer> Adapter;
};
TMap<TWeakObjectPtr<USkeletalMeshComponent>, FStandardSkinningLease> StandardSkinningLeases;

struct FLOD0Lease
{
    int32 Users = 0;
    int32 PreviousRendered = INDEX_NONE;
    int32 PreviousStreamed = INDEX_NONE;
};
TMap<TWeakObjectPtr<USkeletalMeshComponent>, FLOD0Lease> LOD0Leases;

void AcquireLOD0(USkeletalMeshComponent* Body)
{
    ILODSyncInterface* Control = Body;
    auto* Lease = LOD0Leases.Find(Body);
    if (!Lease)
    {
        FLOD0Lease Added;
        Added.PreviousRendered = Control->GetForceRenderedLOD();
        Added.PreviousStreamed = Control->GetForceStreamedLOD();
        Lease = &LOD0Leases.Add(Body, Added);
    }
    ++Lease->Users;
    Body->SetForcedLOD(1);
}

void ReleaseLOD0(TWeakObjectPtr<USkeletalMeshComponent> Body)
{
    auto* Lease = LOD0Leases.Find(Body);
    if (!Lease || --Lease->Users > 0) return;
    if (Body.IsValid())
    {
        ILODSyncInterface* Control = Body.Get();
        if (Control->GetForceRenderedLOD() == 0) Control->SetForceRenderedLOD(Lease->PreviousRendered);
        if (Control->GetForceStreamedLOD() == 0) Control->SetForceStreamedLOD(Lease->PreviousStreamed);
    }
    LOD0Leases.Remove(Body);
}

bool AcquireStandardSkinning(USkeletalMeshComponent* Body, TSoftObjectPtr<UMeshDeformer>& Graph,
    bool& bAcquired, FString& Error)
{
    bAcquired = false;
    if (auto* Lease = StandardSkinningLeases.Find(Body))
    {
        if (Body->GetComponentMeshDeformer() == Lease->Adapter)
        { ++Lease->Users; bAcquired = true; return true; }
    }
    if (Body->HasMeshDeformer()) return true;
    UMeshDeformer* Adapter = Graph.LoadSynchronous();
    if (!Adapter)
    { Error = TEXT("Cannot load the standard skinning adapter. Enable Deformer Graph and select a valid Standard Skinning Deformer."); return false; }
    // An external override may have replaced an adapter still leased by other clothes.
    if (StandardSkinningLeases.Contains(Body))
    { Error = TEXT("The body's deformer changed while clothes were connected. Rebuild all previews for this body."); return false; }
    // UE exposes this setting as a property but has no public C++ getter.
    const auto* OverrideProperty = FindFProperty<FBoolProperty>(USkinnedMeshComponent::StaticClass(), TEXT("bSetMeshDeformer"));
    if (!OverrideProperty)
    { Error = TEXT("This engine does not expose the mesh deformer override setting."); return false; }
    FStandardSkinningLease Lease;
    Lease.Users = 1; Lease.bPreviousOverride = OverrideProperty->GetPropertyValue_InContainer(Body);
    Lease.Previous = Body->GetComponentMeshDeformer(); Lease.Adapter = Adapter;
    StandardSkinningLeases.Add(Body, Lease);
    Body->SetMeshDeformer(Adapter);
    bAcquired = true;
    return true;
}

void ReleaseStandardSkinning(TWeakObjectPtr<USkeletalMeshComponent> Body)
{
    auto* Lease = StandardSkinningLeases.Find(Body);
    if (!Lease || --Lease->Users > 0) return;
    if (Body.IsValid() && Body->GetComponentMeshDeformer() == Lease->Adapter)
    {
        if (Lease->bPreviousOverride) Body->SetMeshDeformer(Lease->Previous.Get());
        else Body->UnsetMeshDeformer();
    }
    StandardSkinningLeases.Remove(Body);
}
}

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMAClothLODLeaseTest, "HUMAClothBind.Reuse.LOD0Lease",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHUMAClothLODLeaseTest::RunTest(const FString&)
{
    auto* Body = NewObject<USkeletalMeshComponent>();
    auto* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/HUMA_Asset/character_lod.character_lod"));
    if (!TestNotNull(TEXT("Four-LOD source mesh"), Mesh)) return false;
    Body->SetSkeletalMeshAsset(Mesh);
    Body->SetForcedLOD(3);
    AcquireLOD0(Body);
    AcquireLOD0(Body);
    TestEqual(TEXT("Legacy users pin LOD0"), Body->GetForcedLOD(), 1);
    ReleaseLOD0(Body);
    TestEqual(TEXT("Other legacy users retain their pin"), Body->GetForcedLOD(), 1);
    ReleaseLOD0(Body);
    TestEqual(TEXT("Last legacy user restores the previous selection"), Body->GetForcedLOD(), 3);
    AcquireLOD0(Body);
    Body->SetForcedLOD(4);
    ReleaseLOD0(Body);
    TestEqual(TEXT("External LOD selection is not overwritten on release"), Body->GetForcedLOD(), 4);
    ILODSyncInterface* Control = Body;
    Control->SetForceRenderedLOD(2); Control->SetForceStreamedLOD(1);
    AcquireLOD0(Body);
    ReleaseLOD0(Body);
    TestEqual(TEXT("Independent rendered override is restored"), Control->GetForceRenderedLOD(), 2);
    TestEqual(TEXT("Independent streaming override is restored"), Control->GetForceStreamedLOD(), 1);
    TestFalse(TEXT("No lease remains after all users release"), LOD0Leases.Contains(Body));
    return !HasAnyErrors();
}
#endif

UHUMAClothBindComponent::UHUMAClothBindComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;
    PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
    bTickInEditor = true;
    StandardSkinningDeformer = FSoftObjectPath(TEXT("/DeformerGraph/Deformers/DG_LinearBlendSkin_Morph_Cloth.DG_LinearBlendSkin_Morph_Cloth"));
}

void UHUMAClothBindComponent::OnRegister()
{
    Super::OnRegister();
    if (!IsTemplate() && GetWorld() && GetOwner())
    {
        RegisterAllComponentTickFunctions(true);
        SetComponentTickEnabled(true);
        if (BindAsset) RebuildPreview();
        else PreviewStatus = TEXT("Select a baked Bind Asset");
    }
}

void UHUMAClothBindComponent::OnUnregister()
{
    ClearPreview();
    Super::OnUnregister();
}

void UHUMAClothBindComponent::BeginPlay()
{
    Super::BeginPlay();
    USkeletalMeshComponent* Expected = ResolveSourceBody();
    if (bPreviewEnabled && BindAsset && InitializedBindAsset == BindAsset && BodyComponent == Expected &&
        Expected && Expected->IsRegistered() && Expected->GetWorld() == GetWorld() && Expected->GetSkeletalMeshAsset() == InitializedBodyMesh)
    {
        if (bUseGPU && GPUMesh && GPUMesh->IsRegistered())
        {
            const auto GPU = GPUMesh->GetGPUState();
            if (GPU && GPU->Scene == GetWorld()->Scene && GPU->PrimitiveId == Expected->GetPrimitiveSceneId()) return;
        }
        if (!bUseGPU && PreviewMesh && PreviewMesh->IsRegistered() && State &&
            State->Scene == GetWorld()->Scene && State->PrimitiveId == Expected->GetPrimitiveSceneId()) return;
    }
    RebuildPreview();
}

USkeletalMeshComponent* UHUMAClothBindComponent::ResolveSourceBody() const
{
    if (SourceBodyComponent) return IsValid(SourceBodyComponent) ? SourceBodyComponent.Get() : nullptr;
    AActor* Source = SourceBodyActor ? SourceBodyActor.Get() : GetOwner();
    return IsValid(Source) ? Source->FindComponentByClass<USkeletalMeshComponent>() : nullptr;
}

bool UHUMAClothBindComponent::IsBindingInitialized() const
{
    return bPreviewEnabled && BindAsset && InitializedBindAsset == BindAsset && IsValid(BodyComponent) &&
        BodyComponent == ResolveSourceBody() && BodyComponent->GetSkeletalMeshAsset() == InitializedBodyMesh &&
        ((bUseGPU && GPUMesh && GPUMesh->IsRegistered()) ||
         (!bUseGPU && PreviewMesh && PreviewMesh->IsRegistered() && State.IsValid()));
}

int32 UHUMAClothBindComponent::ApplyMaterialSlots(const TMap<FName, TObjectPtr<UMaterialInterface>>& Materials)
{
    UMeshComponent* Target = GPUMesh ? static_cast<UMeshComponent*>(GPUMesh.Get()) : PreviewMesh.Get();
    if (!BindAsset || !Target)
    {
        UE_LOG(LogTemp, Error, TEXT("HUMAClothBind: Initialize garment %s before applying material overrides."), *GetPathName());
        return INDEX_NONE;
    }
    for (const auto& Item : Materials)
    {
        if (!IsValid(Item.Value.Get()))
        {
            UE_LOG(LogTemp, Error, TEXT("HUMAClothBind: Material override for slot %s is null/invalid; no sections were changed."), *Item.Key.ToString());
            return INDEX_NONE;
        }
    }
    int32 Changed = 0;
    for (int32 Section = 0; Section < BindAsset->SectionMaterials.Num(); ++Section)
    {
        const int32 Material = BindAsset->SectionMaterials[Section];
        FName SlotName;
        if (const UStaticMesh* Mesh = Cast<UStaticMesh>(BindAsset->GarmentMesh))
        {
            if (Mesh->GetStaticMaterials().IsValidIndex(Material)) SlotName = Mesh->GetStaticMaterials()[Material].MaterialSlotName;
        }
        else if (const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(BindAsset->GarmentMesh))
        {
            if (Skeletal->GetMaterials().IsValidIndex(Material)) SlotName = Skeletal->GetMaterials()[Material].MaterialSlotName;
        }
        const TObjectPtr<UMaterialInterface>* Override = Materials.Find(SlotName);
        if (!Override) continue;
        Target->SetMaterial(Section, Override->Get());
        ++Changed;
    }
    for (const auto& Item : Materials) MaterialOverrides.Add(Item.Key, Item.Value);
    AppliedMaterialSlots = Changed;
    return Changed;
}

bool UHUMAClothBindComponent::InitializeBinding(UHUMAClothBindAsset* Asset, USkeletalMeshComponent* Source)
{
    if (!IsRegistered() || !IsValid(Asset) || !IsValid(Source) || !Source->IsRegistered() ||
        Source->GetWorld() != GetWorld())
    {
        PreviewStatus = TEXT("Initialize Binding requires a registered component, a Bind asset and a registered source in the same world.");
        UE_LOG(LogTemp, Error, TEXT("HUMAClothBind: %s"), *PreviewStatus);
        return false;
    }
    BindAsset = Asset;
    SourceBodyComponent = Source;
    SourceBodyActor = Source->GetOwner();
    bPreviewEnabled = true;
    RebuildPreview();
    if (IsBindingInitialized()) return true;
    UE_LOG(LogTemp, Error, TEXT("HUMAClothBind initialization failed: %s"), *PreviewStatus);
    return false;
}

bool UHUMAClothBindComponent::RequestGPUSnapshot(const FString& Path)
{
    if (!GPUMesh) return false;
    const auto GPU = GPUMesh->GetGPUState();
    FScopeLock Lock(&GPU->Mutex);
    if (GPU->bSnapshotPending || GPU->bSnapshotRequested) return false;
    GPU->SnapshotPath = Path; GPU->bSnapshotRequested = true;
    return true;
}

bool UHUMAClothBindComponent::SetSourcePoseForValidation(float Position, int32 BodyLOD)
{
#if WITH_EDITOR
    const bool HasLOD = GPUMesh ? GPUMesh->GetGPUState()->GetLODData(BodyLOD) != nullptr
        : BindAsset && BindAsset->GetBodyLODBind(BodyLOD);
    if (!BodyComponent || !BindAsset || !HasLOD ||
        !FMath::IsFinite(Position) || Position < 0 || !BodyComponent->GetSingleNodeInstance())
    {
        UE_LOG(LogTemp, Error, TEXT("HUMAClothBind pose validation needs a connected single-node body and a baked LOD."));
        return false;
    }
    BodyComponent->SetForcedLOD(BodyLOD + 1);
    BodyComponent->UpdateLODStatus();
    BodyComponent->Stop();
    BodyComponent->SetPosition(Position, false);
    BodyComponent->TickAnimation(0, false);
    BodyComponent->RefreshBoneTransforms();
    BodyComponent->UpdateComponentToWorld();
    BodyComponent->MarkRenderDynamicDataDirty();
    return true;
#else
    UE_LOG(LogTemp, Error, TEXT("HUMAClothBind explicit pose validation requires an Editor build."));
    return false;
#endif
}

void UHUMAClothBindComponent::ClearPreview()
{
    if (AActor* Source = LifetimeSource.Get())
    {
        Source->OnEndPlay.RemoveDynamic(this, &UHUMAClothBindComponent::OnSourceEndPlay);
        Source->OnDestroyed.RemoveDynamic(this, &UHUMAClothBindComponent::OnSourceDestroyed);
    }
    LifetimeSource.Reset();
    if (GPUMesh)
    {
        UnregisterHUMAClothGPU(GPUMesh->GetGPUState());
        GPUMesh->DestroyComponent(); GPUMesh = nullptr;
    }
    if (State) { UnregisterHUMAClothReadback(State); State.Reset(); }
    if (PreviewMesh) { PreviewMesh->DestroyComponent(); PreviewMesh = nullptr; }
    if (BodyComponent) RemoveTickPrerequisiteComponent(BodyComponent);
    if (bUsingStandardSkinningAdapter) ReleaseStandardSkinning(AdaptedBody);
    ReleaseLOD0(LOD0Body);
    LOD0Body.Reset();
    AdaptedBody.Reset();
    bUsingStandardSkinningAdapter = false;
    BodyComponent = nullptr;
    InitializedBindAsset.Reset();
    InitializedBodyMesh.Reset();
    SourceMapping.Reset();
    bUsingCompatibleBodyMesh = false;
    AppliedRevision = 0;
    CapturedFrames = 0;
    ActiveBodyLOD = ActiveBindBodyLOD = INDEX_NONE; LODTransitions = 0;
    SourceLODProjectionMaxErrorCm = 0;
    AppliedMaterialSlots = 0;
    BodyNormalPasses = BodyNormalReuses = BodyNormalEvaluation = 0; BodyNormalGroup = INDEX_NONE;
    LastFrameMotionCm = 0;
    LastClothPositions.Reset();
}

void UHUMAClothBindComponent::RebuildPreview()
{
    const double Start = FPlatformTime::Seconds();
    ++InitializationCount;
    ON_SCOPE_EXIT { InitializationMilliseconds = float((FPlatformTime::Seconds() - Start) * 1000.0); };
    ClearPreview();
    FString Error;
    if (!bPreviewEnabled || !BindAsset)
    { PreviewStatus = TEXT("Select a baked Bind Asset"); return; }
    BodyComponent = ResolveSourceBody();
    if (!IsValid(BodyComponent) || !BodyComponent->IsRegistered() || BodyComponent->GetWorld() != GetWorld())
    { PreviewStatus = TEXT("Source body must be a registered SkeletalMeshComponent in the same world."); return; }
    if (bDestroyOwnerWithSource && BodyComponent->GetOwner() == GetOwner())
    {
        PreviewStatus = TEXT("Destroy Owner With Source requires a separate garment actor, not the body actor itself.");
        UE_LOG(LogTemp, Error, TEXT("HUMAClothBind %s"), *PreviewStatus);
        return;
    }
    if (bDestroyOwnerWithSource && BodyComponent->GetOwner() != GetOwner())
    {
        LifetimeSource = BodyComponent->GetOwner();
        LifetimeSource->OnEndPlay.AddUniqueDynamic(this, &UHUMAClothBindComponent::OnSourceEndPlay);
        LifetimeSource->OnDestroyed.AddUniqueDynamic(this, &UHUMAClothBindComponent::OnSourceDestroyed);
    }
    USkeletalMesh* TargetMesh = BodyComponent->GetSkeletalMeshAsset();
    FHUMAClothSourceLODs Sources;
    if (auto* Preparation = GetWorld()->GetSubsystem<UHUMAClothPreparationSubsystem>())
    {
        const bool Prepared = bUseGPU ? Preparation->PrepareLODs(BindAsset, TargetMesh, Sources, Error)
            : Preparation->Prepare(BindAsset, TargetMesh, SourceMapping, Error);
        if (!Prepared)
        { PreviewStatus = Error; UE_LOG(LogTemp, Error, TEXT("HUMAClothBind %s: %s"), *GetName(), *Error); return; }
    }
    else if (!BindAsset->IsBakeCurrent(Error) || !(bUseGPU ? BindAsset->BuildSourceLODs(TargetMesh, Sources, Error)
        : BindAsset->BuildSourceMapping(TargetMesh, SourceMapping, Error)))
    { PreviewStatus = Error; UE_LOG(LogTemp, Error, TEXT("HUMAClothBind %s: %s"), *GetName(), *Error); return; }
    const bool bReuseMesh = TargetMesh != BindAsset->BodyMesh;
    bUsingCompatibleBodyMesh = bReuseMesh;
    AddTickPrerequisiteComponent(BodyComponent);
    const FSkeletalMeshRenderData* Data = TargetMesh->GetResourceForRendering();
    if (!Data || Data->LODRenderData.IsEmpty()) { PreviewStatus = TEXT("Body has no LOD0 data."); return; }
    for (const auto& Pair : Sources)
    {
        if (!Data->LODRenderData.IsValidIndex(Pair.Key))
        { PreviewStatus = TEXT("Prepared source LOD is unavailable."); return; }
        const int32 Count = Pair.Value.Transfer ? Pair.Value.Mapping.Num() : Data->LODRenderData[Pair.Key].GetNumVertices();
        for (int32 RenderIndex : Pair.Value.Mapping)
            if (RenderIndex < 0 || RenderIndex >= Count)
            { PreviewStatus = TEXT("Body LOD mapping is stale. Re-bake this asset."); return; }
    }
    if (bEnableStandardSkinningAdapter)
    {
        if (!AcquireStandardSkinning(BodyComponent, StandardSkinningDeformer, bUsingStandardSkinningAdapter, Error))
        { PreviewStatus = Error; UE_LOG(LogTemp, Error, TEXT("HUMAClothBind %s: %s"), *GetName(), *Error); return; }
        if (bUsingStandardSkinningAdapter) AdaptedBody = BodyComponent;
    }
    if (!BodyComponent->HasMeshDeformer())
    { PreviewStatus = TEXT("Source needs a Mesh Deformer or Enable Standard Skinning Adapter."); return; }
    if (!bUseGPU || (!bReuseMesh && BindAsset->AdditionalBodyLODs.IsEmpty()))
    { AcquireLOD0(BodyComponent); LOD0Body = BodyComponent.Get(); }
    if (bUseGPU)
    {
        GPUMesh = NewObject<UHUMAClothGpuMeshComponent>(GetOwner(), NAME_None, RF_Transient);
        GPUMesh->Initialize(BindAsset, BodyComponent, Sources);
        GPUMesh->SetupAttachment(BodyComponent);
        GPUMesh->RegisterComponent();
        RegisterHUMAClothGPU(GPUMesh->GetGPUState());
        InitializedBindAsset = BindAsset;
        InitializedBodyMesh = TargetMesh;
        if (!MaterialOverrides.IsEmpty())
        {
            const auto Overrides = MaterialOverrides;
            ApplyMaterialSlots(Overrides);
        }
        PreviewStatus = TEXT("Waiting for same-frame GPU SoftBind");
        return;
    }
    PreviewMesh = NewObject<UProceduralMeshComponent>(GetOwner(), NAME_None, RF_Transient);
    PreviewMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    PreviewMesh->RegisterComponent();
    PreviewMesh->SetWorldTransform(BodyComponent->GetComponentTransform());
    TArray<FVector> Normals;
    TArray<FColor> Colors;
    TArray<FProcMeshTangent> Tangents;
    int32 Offset = 0;
    for (int32 Section = 0; Section < BindAsset->SectionIndexCounts.Num(); ++Section)
    {
        const int32 Count = BindAsset->SectionIndexCounts[Section];
        if (Count < 0 || Offset + Count > BindAsset->ClothTriangles.Num()) { PreviewStatus = TEXT("Invalid baked material sections."); ClearPreview(); return; }
        TArray<int32> Indices;
        Indices.Append(&BindAsset->ClothTriangles[Offset], Count);
        Offset += Count;
        PreviewMesh->CreateMeshSection(Section, BindAsset->ClothRestPositions, Indices, Normals, BindAsset->ClothUVs, Colors, Tangents, false);
        const int32 MaterialIndex = BindAsset->SectionMaterials.IsValidIndex(Section) ? BindAsset->SectionMaterials[Section] : Section;
        if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(BindAsset->GarmentMesh)) PreviewMesh->SetMaterial(Section, Mesh->GetMaterials().IsValidIndex(MaterialIndex) ? Mesh->GetMaterials()[MaterialIndex].MaterialInterface : nullptr);
        if (UStaticMesh* Mesh = Cast<UStaticMesh>(BindAsset->GarmentMesh)) PreviewMesh->SetMaterial(Section, Mesh->GetMaterial(MaterialIndex));
    }
    PreviewMesh->SetVisibility(false);
    State = MakeShared<FHUMAClothReadback, ESPMode::ThreadSafe>();
    State->Scene = GetWorld()->Scene;
    State->PrimitiveId = BodyComponent->GetPrimitiveSceneId();
    State->RenderVertexCount = Data->LODRenderData[0].GetNumVertices();
    RegisterHUMAClothReadback(State);
    InitializedBindAsset = BindAsset;
    InitializedBodyMesh = TargetMesh;
    PreviewStatus = TEXT("Waiting for final GPU positions");
    if (!MaterialOverrides.IsEmpty())
    {
        const auto Overrides = MaterialOverrides;
        ApplyMaterialSlots(Overrides);
    }
}

void UHUMAClothBindComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction)
{
    Super::TickComponent(DeltaTime, TickType, TickFunction);
    if (bDestroyOwnerWithSource && InitializedBindAsset.IsValid()
        && (!IsValid(BodyComponent) || !BodyComponent->IsRegistered() || !LifetimeSource.IsValid()))
    {
        if (GetOwner() && !GetOwner()->IsActorBeingDestroyed()) GetOwner()->Destroy();
        return;
    }
    if (InitializedBindAsset.IsValid() &&
        (InitializedBindAsset != BindAsset || !IsValid(BodyComponent) || BodyComponent != ResolveSourceBody() ||
         BodyComponent->GetSkeletalMeshAsset() != InitializedBodyMesh))
        RebuildPreview();
    if (GPUMesh)
    {
        GPUMesh->SetVisibility(bPreviewEnabled);
        const auto GPU = GPUMesh->GetGPUState();
        FScopeLock Lock(&GPU->Mutex);
        PreviewStatus = GPU->Status; CapturedFrames = GPU->Frames; EvaluationMilliseconds = 0;
        ActiveBodyLOD = GPU->ActiveBodyLOD; LODTransitions = GPU->LODTransitions;
        const auto* Data = GPU->GetLODData(ActiveBodyLOD);
        ActiveBindBodyLOD = Data ? Data->BindBodyLOD : INDEX_NONE;
        SourceLODProjectionMaxErrorCm = Data && Data->SourceTransfer ? Data->SourceTransfer->MaxRestDistanceCm : 0;
        BodyNormalPasses = GPU->BodyNormalPasses; BodyNormalReuses = GPU->BodyNormalReuses;
        BodyNormalGroup = GPU->BodyNormalGroup; BodyNormalEvaluation = GPU->BodyNormalEvaluation;
        return;
    }
    if (!State || !BindAsset || !BodyComponent || !PreviewMesh) return;
    if (!bPreviewEnabled) { PreviewMesh->SetVisibility(false); return; }
    PreviewMesh->SetWorldTransform(BodyComponent->GetComponentTransform());
    TArray<FVector3f> Canonical;
    {
        FScopeLock Lock(&State->Mutex);
        PreviewStatus = State->Status;
        if (!State->Status.StartsWith(TEXT("Final GPU"))) { PreviewMesh->SetVisibility(false); return; }
        if (State->Revision == AppliedRevision) return;
        AppliedRevision = State->Revision;
        Canonical.Reserve(SourceMapping.Num());
        for (int32 Index : SourceMapping) Canonical.Add(State->Positions[Index]);
    }
    TArray<FVector> Cloth;
    FString Error;
    const double Start = FPlatformTime::Seconds();
    if (!BindAsset->Evaluate(Canonical, Cloth, Error)) { PreviewStatus = Error; PreviewMesh->SetVisibility(false); return; }
    EvaluationMilliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;
    LastFrameMotionCm = 0;
    if (LastClothPositions.Num() == Cloth.Num())
        for (int32 Index = 0; Index < Cloth.Num(); ++Index) LastFrameMotionCm = FMath::Max(LastFrameMotionCm, float(FVector::Distance(Cloth[Index], LastClothPositions[Index])));
    LastClothPositions = Cloth;
    TArray<FVector> Normals;
    Normals.Init(FVector::ZeroVector, Cloth.Num());
    for (int32 Index = 0; Index < BindAsset->ClothTriangles.Num(); Index += 3)
    {
        const int32* Triangle = &BindAsset->ClothTriangles[Index];
        const FVector Area = FVector::CrossProduct(Cloth[Triangle[2]] - Cloth[Triangle[0]], Cloth[Triangle[1]] - Cloth[Triangle[0]]);
        for (int32 Corner = 0; Corner < 3; ++Corner) Normals[Triangle[Corner]] += Area;
    }
    for (FVector& Normal : Normals) Normal.Normalize();
    TArray<FVector2D> UVs;
    TArray<FColor> Colors;
    TArray<FProcMeshTangent> Tangents;
    for (int32 Section = 0; Section < BindAsset->SectionIndexCounts.Num(); ++Section)
        PreviewMesh->UpdateMeshSection(Section, Cloth, Normals, UVs, Colors, Tangents);
    PreviewMesh->SetVisibility(true);
    ++CapturedFrames;
}

#if WITH_EDITOR
void UHUMAClothBindComponent::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
    Super::PostEditChangeProperty(Event);
    if (IsRegistered()) RebuildPreview();
}
#endif

void UHUMAClothBindComponent::OnSourceEndPlay(AActor* Source, EEndPlayReason::Type Reason)
{
    OnSourceDestroyed(Source);
}

void UHUMAClothBindComponent::OnSourceDestroyed(AActor* Source)
{
    if (bDestroyOwnerWithSource && GetOwner() && GetOwner() != Source && !GetOwner()->IsActorBeingDestroyed())
        GetOwner()->Destroy();
}

AHUMAClothPreviewActor::AHUMAClothPreviewActor()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = true;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    ClothBind = CreateDefaultSubobject<UHUMAClothBindComponent>(TEXT("ClothBind"));
}

AHUMAClothPreviewActor* AHUMAClothPreviewActor::SpawnBoundGarment(UHUMAClothBindAsset* Asset, USkeletalMeshComponent* Source)
{
    FString Error;
    auto* Actor = SpawnBoundGarmentInternal(Asset, Source, Error);
    if (!Actor) UE_LOG(LogTemp, Error, TEXT("HUMAClothBind %s"), *Error);
    return Actor;
}

TArray<AHUMAClothPreviewActor*> AHUMAClothPreviewActor::SpawnBoundGarments(
    const TArray<UHUMAClothBindAsset*>& Assets, USkeletalMeshComponent* Source, TArray<FHUMAClothSpawnFailure>& Failures)
{
    Failures.Reset();
    TArray<AHUMAClothPreviewActor*> Actors;
    Actors.Reserve(Assets.Num());
    if (Assets.IsEmpty())
    {
        FHUMAClothSpawnFailure Failure;
        Failure.Error = TEXT("Spawn Bound Garments requires at least one Bind asset.");
        UE_LOG(LogTemp, Error, TEXT("HUMAClothBind %s"), *Failure.Error);
        Failures.Add(MoveTemp(Failure));
        return Actors;
    }
    for (int32 Index = 0; Index < Assets.Num(); ++Index)
    {
        FString Error;
        if (auto* Actor = SpawnBoundGarmentInternal(Assets[Index], Source, Error)) Actors.Add(Actor);
        else
        {
            FHUMAClothSpawnFailure Failure;
            Failure.InputIndex = Index;
            Failure.Asset = Assets[Index];
            Failure.Error = MoveTemp(Error);
            UE_LOG(LogTemp, Error, TEXT("HUMAClothBind Spawn Bound Garments input %d (%s): %s"),
                Index, *GetPathNameSafe(Assets[Index]), *Failure.Error);
            Failures.Add(MoveTemp(Failure));
        }
    }
    return Actors;
}

AHUMAClothPreviewActor* AHUMAClothPreviewActor::SpawnBoundGarmentInternal(
    UHUMAClothBindAsset* Asset, USkeletalMeshComponent* Source, FString& Error)
{
    UWorld* World = IsValid(Source) ? Source->GetWorld() : nullptr;
    if (!IsValid(Asset) || !World || !Source->IsRegistered() || !IsValid(Source->GetOwner()))
    {
        Error = TEXT("Spawn Bound Garment requires a Bind asset and a registered source component.");
        return nullptr;
    }
    const FTransform Transform = Source->GetComponentTransform();
    auto* Actor = World->SpawnActorDeferred<AHUMAClothPreviewActor>(
        StaticClass(), Transform, Source->GetOwner(), nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
    if (!Actor)
    {
        Error = TEXT("Could not spawn a garment actor.");
        return nullptr;
    }
    Actor->ClothBind->BindAsset = Asset;
    Actor->ClothBind->SourceBodyComponent = Source;
    Actor->ClothBind->SourceBodyActor = Source->GetOwner();
    Actor->ClothBind->bDestroyOwnerWithSource = true;
    Actor->FinishSpawning(Transform);
    if (!Actor->ClothBind->IsBindingInitialized()) Actor->ClothBind->RebuildPreview();
    if (!Actor->ClothBind->IsBindingInitialized())
    {
        Error = TEXT("Spawn Bound Garment failed: ") + Actor->ClothBind->PreviewStatus;
        Actor->Destroy();
        return nullptr;
    }
    Actor->AttachToComponent(Source, FAttachmentTransformRules::KeepWorldTransform);
    return Actor;
}