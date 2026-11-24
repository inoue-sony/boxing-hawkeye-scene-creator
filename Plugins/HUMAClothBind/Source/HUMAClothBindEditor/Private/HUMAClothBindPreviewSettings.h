#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "HUMAClothBindPreviewSettings.generated.h"

class UAnimationAsset;
class USkeletalMesh;

UCLASS(Transient)
class UHUMAClothBindPreviewSettings : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, Category="Preview", meta=(ToolTip="Optional runtime body variant. Empty uses the Bind asset's reference body. Does not change the Bake source."))
    TObjectPtr<USkeletalMesh> PreviewBodyMesh;
    UPROPERTY(EditAnywhere, Category="Preview")
    TObjectPtr<UAnimationAsset> Animation;
    UPROPERTY(EditAnywhere, Category="Preview")
    bool bPlaying = false;
    UPROPERTY(EditAnywhere, Category="Preview", meta=(ClampMin="0.0", Units="s"))
    float AnimationTime = 0;
    UPROPERTY(EditAnywhere, Category="Preview")
    FName MorphTarget;
    UPROPERTY(EditAnywhere, Category="Preview", meta=(UIMin="0.0", UIMax="1.0"))
    float MorphWeight = 0;
};
