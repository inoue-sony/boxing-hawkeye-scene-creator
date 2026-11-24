#include "AnimNode_HUMAIdentity.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Components/SkeletalMeshComponent.h"

void FAnimNode_HUMAIdentity::Initialize_AnyThread(const FAnimationInitializeContext& Context)
{
	FAnimNode_Base::Initialize_AnyThread(Context);
	Pose.Reset();
	RuntimePose.Reset();
	bUseInputHipTranslation = false;
	Source.Initialize(Context);
}

void FAnimNode_HUMAIdentity::CacheBones_AnyThread(const FAnimationCacheBonesContext& Context)
{
	Source.CacheBones(Context);
}

void FAnimNode_HUMAIdentity::Update_AnyThread(const FAnimationUpdateContext& Context)
{
	Source.Update(Context);
}

void FAnimNode_HUMAIdentity::PreUpdate(const UAnimInstance* Instance)
{
	Pose.Reset();
	RuntimePose.Reset();
	bUseInputHipTranslation = false;
	const USkeletalMeshComponent* Mesh = Instance ? Instance->GetSkelMeshComponent() : nullptr;
	if (!Mesh || !Mesh->GetOwner()) { return; }
	const UAnimInstance* InputInstance = Mesh->GetAnimInstance();
	if (UAnimSingleNodeInstance* SingleNode = Mesh->GetSingleNodeInstance())
	{
		bUseInputHipTranslation = SingleNode->GetCurrentAsset() != nullptr;
	}
	else
	{
		bUseInputHipTranslation = InputInstance != nullptr;
	}
	TInlineComponentArray<UHUMARuntimePoseComponent*> RuntimeComponents(Mesh->GetOwner());
	for (const UHUMARuntimePoseComponent* Component : RuntimeComponents)
	{
		if (Component->TargetMesh == Mesh) { RuntimePose = Component->GetPose(); break; }
	}
	TInlineComponentArray<UHUMAIdentityComponent*> Components(Mesh->GetOwner());
	for (const UHUMAIdentityComponent* Component : Components)
	{
		if (Component->TargetMesh == Mesh) { Pose = Component->GetIdentityPose(); return; }
	}
}

void FAnimNode_HUMAIdentity::Evaluate_AnyThread(FPoseContext& Output)
{
	Source.Evaluate(Output);
	if (!Pose) { return; }
	const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
	for (const FCompactPoseBoneIndex Compact : Output.Pose.ForEachBoneIndex())
	{
		const int32 Bone = Bones.MakeMeshPoseIndex(Compact).GetInt();
		if (!Pose->ShapedPose.IsValidIndex(Bone)) { continue; }
		FVector Translation = Pose->ShapedPose[Bone].GetTranslation();
		if (Bone == Pose->HipIndex)
		{
			if (RuntimePose)
			{
				Translation += RuntimePose->HipDisplacement;
			}
			else if (bUseInputHipTranslation)
			{
				Translation += Output.Pose[Compact].GetTranslation() - Pose->ReferencePose[Bone].GetTranslation();
			}
		}
		if (RuntimePose && RuntimePose->MeshLocalRotations.IsValidIndex(Bone))
		{
			Output.Pose[Compact].SetRotation(RuntimePose->MeshLocalRotations[Bone]);
		}
		Output.Pose[Compact].SetTranslation(Translation);
		Output.Pose[Compact].SetScale3D(Pose->ReferencePose[Bone].GetScale3D());
	}
	for (int32 Beta = 0; Beta < Pose->Betas.Num(); ++Beta)
	{
		Output.Curve.Set(Pose->BetaNames[Beta], Pose->Betas[Beta]);
	}
}
