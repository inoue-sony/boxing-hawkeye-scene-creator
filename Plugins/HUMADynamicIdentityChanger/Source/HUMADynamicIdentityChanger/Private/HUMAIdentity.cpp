#include "HUMAIdentity.h"

#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, HUMADynamicIdentityChanger)
DEFINE_LOG_CATEGORY(LogHUMAIdentity);

namespace
{
	TArray<FTransform> AccumulatePose(const FReferenceSkeleton& Skeleton, const TArray<FTransform>& LocalPose)
	{
		TArray<FTransform> Result = LocalPose;
		for (int32 Bone = 0; Bone < Result.Num(); ++Bone)
		{
			const int32 Parent = Skeleton.GetParentIndex(Bone);
			if (Parent != INDEX_NONE) { Result[Bone] = Result[Bone] * Result[Parent]; }
		}
		return Result;
	}
}

bool UHUMAIdentityData::Validate(USkeletalMesh* Mesh, FString& Error) const
{
	if (!Mesh || Mesh != ReferenceMesh)
	{
		Error = TEXT("Identity data must reference this exact neutral skeletal mesh.");
		return false;
	}
	if (BetaNames.IsEmpty() || JointNames.IsEmpty() || JointTemplates.Num() != JointNames.Num()
		|| JointCoefficients.Num() != JointNames.Num() * BetaNames.Num())
	{
		Error = TEXT("Identity data has inconsistent regressor dimensions.");
		return false;
	}
	const FReferenceSkeleton& Skeleton = Mesh->GetRefSkeleton();
	const TArray<FTransform> Reference = AccumulatePose(Skeleton, Skeleton.GetRefBonePose());
	TSet<FName> Names;
	for (const FName Name : BetaNames)
	{
		if (Names.Contains(Name) || !Mesh->FindMorphTarget(Name))
		{
			Error = FString::Printf(TEXT("Duplicate or missing identity morph: %s."), *Name.ToString());
			return false;
		}
		Names.Add(Name);
	}
	Names.Reset();
	for (int32 Joint = 0; Joint < JointNames.Num(); ++Joint)
	{
		const int32 Bone = Skeleton.FindBoneIndex(JointNames[Joint]);
		if (Bone == INDEX_NONE || Names.Contains(JointNames[Joint]) || JointTemplates[Joint].ContainsNaN()
			|| !Reference[Bone].GetTranslation().Equals(JointTemplates[Joint], 0.01))
		{
			Error = FString::Printf(TEXT("Invalid or incompatible regressor joint: %s."), *JointNames[Joint].ToString());
			return false;
		}
		Names.Add(JointNames[Joint]);
	}
	for (const FVector& Coefficient : JointCoefficients)
	{
		if (Coefficient.ContainsNaN())
		{
			Error = TEXT("Joint regressor contains non-finite coefficients.");
			return false;
		}
	}
	if (!JointNames.Contains(HipBone))
	{
		Error = TEXT("The hip bone is absent from the regressor.");
		return false;
	}
	return true;
}

UHUMAIdentityComponent::UHUMAIdentityComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	bTickInEditor = true;
	bWantsInitializeComponent = true;
}

bool UHUMAIdentityComponent::Fail(const FString& Message)
{
	LastError = Message;
	UE_LOG(LogHUMAIdentity, Error, TEXT("%s: %s"), *GetPathName(), *Message);
	return false;
}

void UHUMAIdentityComponent::PostLoad()
{
	Super::PostLoad();
	InitializeBetaArrayIfEmpty();
}

void UHUMAIdentityComponent::OnRegister()
{
	Super::OnRegister();
	InitializeBetaArrayIfEmpty();
	if (!TargetMesh) { TargetMesh = FindMatchingMesh(); }
	if (TargetMesh) { TargetMesh->AddTickPrerequisiteComponent(this); }
	bDirty = true;
}

void UHUMAIdentityComponent::InitializeComponent()
{
	Super::InitializeComponent();
	if (IdentityData) { ApplyIdentity(); }
}

USkeletalMeshComponent* UHUMAIdentityComponent::FindMatchingMesh() const
{
	if (!GetOwner() || !IdentityData || !IdentityData->ReferenceMesh) { return nullptr; }
	USkeletalMeshComponent* Match = nullptr;
	TInlineComponentArray<USkeletalMeshComponent*> Meshes(GetOwner());
	for (USkeletalMeshComponent* Mesh : Meshes)
	{
		if (Mesh->GetSkeletalMeshAsset() != IdentityData->ReferenceMesh) { continue; }
		if (Match) { return nullptr; }
		Match = Mesh;
	}
	return Match;
}

void UHUMAIdentityComponent::InitializeBetaArrayIfEmpty()
{
	if (IdentityData && Betas.IsEmpty())
	{
		Betas.Init(0.0f, IdentityData->BetaNames.Num());
	}
}

void UHUMAIdentityComponent::ReleaseOverride()
{
	if (USkeletalMeshComponent* Mesh = AppliedMesh.Get())
	{
		Mesh->HandleExistingParallelEvaluationTask(true, true);
		Mesh->ClearRefPoseOverride();
		if (Pose)
		{
			for (const FName Name : Pose->BetaNames) { Mesh->SetMorphTarget(Name, 0.0f); }
		}
		Mesh->RemoveTickPrerequisiteComponent(this);
	}
	Pose.Reset();
	AppliedMesh.Reset();
}

void UHUMAIdentityComponent::OnUnregister()
{
	ReleaseOverride();
	Super::OnUnregister();
}

bool UHUMAIdentityComponent::SetBeta(int32 Index, float Value)
{
	if (!IdentityData || !IdentityData->BetaNames.IsValidIndex(Index) || !FMath::IsFinite(Value))
	{
		return Fail(TEXT("SetBeta requires a valid beta index and a finite value."));
	}
	InitializeBetaArrayIfEmpty();
	if (Betas.Num() != IdentityData->BetaNames.Num())
	{
		return Fail(TEXT("Beta count does not match the identity model."));
	}
	Betas[Index] = Value;
	bDirty = true;
	return true;
}

bool UHUMAIdentityComponent::SetBetas(const TArray<float>& Values)
{
	if (!IdentityData || Values.Num() != IdentityData->BetaNames.Num())
	{
		return Fail(TEXT("SetBetas requires exactly one value per model beta."));
	}
	for (const float Value : Values)
	{
		if (!FMath::IsFinite(Value)) { return Fail(TEXT("Beta values must be finite.")); }
	}
	Betas = Values;
	bDirty = true;
	return true;
}

void UHUMAIdentityComponent::ResetIdentity()
{
	if (!IdentityData) { Fail(TEXT("Assign identity data before resetting beta.")); return; }
	Betas.Init(0.0f, IdentityData->BetaNames.Num());
	bDirty = true;
}

bool UHUMAIdentityComponent::ApplyIdentity()
{
	check(IsInGameThread());
	bDirty = false;
	if (!IdentityData) { return Fail(TEXT("Assign IdentityData.")); }
	if (!TargetMesh) { TargetMesh = FindMatchingMesh(); }
	if (!TargetMesh)
	{
		return Fail(TEXT("Cannot select a unique body mesh matching IdentityData. Assign TargetMesh explicitly if multiple matching meshes exist."));
	}
	FString Error;
	USkeletalMesh* Mesh = TargetMesh->GetSkeletalMeshAsset();
	if (!IdentityData->Validate(Mesh, Error)) { return Fail(Error); }
	InitializeBetaArrayIfEmpty();
	if (Betas.Num() != IdentityData->BetaNames.Num()) { return Fail(TEXT("Incorrect beta count.")); }
	for (const float Value : Betas)
	{
		if (!FMath::IsFinite(Value)) { return Fail(TEXT("Beta values must be finite.")); }
	}
	if (AppliedMesh.IsValid() && AppliedMesh != TargetMesh) { ReleaseOverride(); }
	TargetMesh->HandleExistingParallelEvaluationTask(true, true);
	TargetMesh->AddTickPrerequisiteComponent(this);

	TSharedRef<FHUMAIdentityPose, ESPMode::ThreadSafe> NewPose = MakeShared<FHUMAIdentityPose, ESPMode::ThreadSafe>();
	const FReferenceSkeleton& Skeleton = Mesh->GetRefSkeleton();
	NewPose->ReferencePose = Skeleton.GetRefBonePose();
	NewPose->ShapedPose = NewPose->ReferencePose;
	NewPose->BetaNames = IdentityData->BetaNames;
	NewPose->Betas = Betas;
	NewPose->HipIndex = Skeleton.FindBoneIndex(IdentityData->HipBone);
	TArray<int32> BoneToJoint;
	BoneToJoint.Init(INDEX_NONE, Skeleton.GetNum());
	TArray<FVector> Positions = IdentityData->JointTemplates;
	for (int32 Joint = 0; Joint < Positions.Num(); ++Joint)
	{
		BoneToJoint[Skeleton.FindBoneIndex(IdentityData->JointNames[Joint])] = Joint;
		for (int32 Beta = 0; Beta < Betas.Num(); ++Beta)
		{
			Positions[Joint] += IdentityData->JointCoefficients[Joint * Betas.Num() + Beta] * Betas[Beta];
		}
		if (Positions[Joint].ContainsNaN()) { return Fail(TEXT("Beta combination overflowed joint positions.")); }
	}
	TArray<FTransform> GlobalPose;
	GlobalPose.SetNum(Skeleton.GetNum());
	for (int32 Bone = 0; Bone < Skeleton.GetNum(); ++Bone)
	{
		const int32 Parent = Skeleton.GetParentIndex(Bone);
		const FTransform ParentPose = Parent == INDEX_NONE ? FTransform::Identity : GlobalPose[Parent];
		if (BoneToJoint[Bone] != INDEX_NONE)
		{
			NewPose->ShapedPose[Bone].SetTranslation(ParentPose.InverseTransformPosition(Positions[BoneToJoint[Bone]]));
		}
		GlobalPose[Bone] = NewPose->ShapedPose[Bone] * ParentPose;
	}
	TargetMesh->SetRefPoseOverride(NewPose->ShapedPose);
	for (int32 Beta = 0; Beta < Betas.Num(); ++Beta)
	{
		TargetMesh->SetMorphTarget(IdentityData->BetaNames[Beta], Betas[Beta]);
	}
	Pose = NewPose;
	AppliedMesh = TargetMesh;
	LastError.Reset();
	return true;
}

void UHUMAIdentityComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction)
{
	Super::TickComponent(DeltaTime, TickType, TickFunction);
	if (bDirty && IdentityData) { ApplyIdentity(); }
}

#if WITH_EDITOR
void UHUMAIdentityComponent::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	InitializeBetaArrayIfEmpty();
	bDirty = true;
	Super::PostEditChangeProperty(Event);
}
#endif

TArray<FVector> UHUMAIdentityComponent::GetAppliedBonePositions() const
{
	TArray<FVector> Result;
	if (Pose && TargetMesh && TargetMesh->GetSkeletalMeshAsset())
	{
		for (const FTransform& Bone : AccumulatePose(TargetMesh->GetSkeletalMeshAsset()->GetRefSkeleton(), Pose->ShapedPose))
		{
			Result.Add(Bone.GetTranslation());
		}
	}
	return Result;
}

TArray<FTransform> UHUMAIdentityComponent::GetEvaluatedLocalPose()
{
	if (!TargetMesh) { Fail(TEXT("No TargetMesh for pose inspection.")); return {}; }
	TargetMesh->HandleExistingParallelEvaluationTask(true, true);
	return TargetMesh->GetBoneSpaceTransforms();
}

TArray<FVector> UHUMAIdentityComponent::GetReferenceBonePositions(USkeletalMesh* Mesh)
{
	TArray<FVector> Result;
	if (!Mesh) { UE_LOG(LogHUMAIdentity, Error, TEXT("Reference pose requires a mesh.")); return Result; }
	for (const FTransform& Bone : AccumulatePose(Mesh->GetRefSkeleton(), Mesh->GetRefSkeleton().GetRefBonePose()))
	{
		Result.Add(Bone.GetTranslation());
	}
	return Result;
}

TArray<FName> UHUMAIdentityComponent::GetReferenceBoneNames(USkeletalMesh* Mesh)
{
	TArray<FName> Result;
	if (!Mesh) { UE_LOG(LogHUMAIdentity, Error, TEXT("Reference bone names require a mesh.")); return Result; }
	for (int32 Bone = 0; Bone < Mesh->GetRefSkeleton().GetNum(); ++Bone)
	{
		Result.Add(Mesh->GetRefSkeleton().GetBoneName(Bone));
	}
	return Result;
}

AHUMAIdentityCharacter::AHUMAIdentityCharacter()
{
	CharacterMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("CharacterMesh"));
	SetRootComponent(CharacterMesh);
	CharacterMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	CharacterMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	Identity = CreateDefaultSubobject<UHUMAIdentityComponent>(TEXT("Identity"));
	Identity->TargetMesh = CharacterMesh;
}

void AHUMAIdentityCharacter::BeginPlay()
{
	Super::BeginPlay();
	if (bUseRuntimeMotion) { CharacterMesh->SetAnimationMode(EAnimationMode::AnimationSingleNode); }
	else if (Motion) { CharacterMesh->PlayAnimation(Motion, true); }
	else { UE_LOG(LogHUMAIdentity, Error, TEXT("%s has no motion asset."), *GetPathName()); }
	Identity->ApplyIdentity();
}
