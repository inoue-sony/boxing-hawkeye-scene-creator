#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNodeBase.h"
#include "HUMAIdentity.h"
#include "HUMARuntimePlayback.h"
#include "AnimNode_HUMAIdentity.generated.h"

USTRUCT(BlueprintInternalUseOnly)
struct HUMADYNAMICIDENTITYCHANGER_API FAnimNode_HUMAIdentity : public FAnimNode_Base
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Links)
	FPoseLink Source;

	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override;
	virtual void CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) override;
	virtual void Update_AnyThread(const FAnimationUpdateContext& Context) override;
	virtual void Evaluate_AnyThread(FPoseContext& Output) override;
	virtual bool HasPreUpdate() const override { return true; }
	virtual void PreUpdate(const UAnimInstance* Instance) override;

private:
	TSharedPtr<const FHUMAIdentityPose, ESPMode::ThreadSafe> Pose;
	TSharedPtr<const FHUMARuntimePose, ESPMode::ThreadSafe> RuntimePose;
	bool bUseInputHipTranslation = false;
};
