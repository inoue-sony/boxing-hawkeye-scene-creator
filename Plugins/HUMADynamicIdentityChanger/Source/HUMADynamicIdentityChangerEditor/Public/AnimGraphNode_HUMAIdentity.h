#pragma once

#include "CoreMinimal.h"
#include "AnimGraphNode_Base.h"
#include "AnimNode_HUMAIdentity.h"
#include "AnimGraphNode_HUMAIdentity.generated.h"

UCLASS()
class UAnimGraphNode_HUMAIdentity : public UAnimGraphNode_Base
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category = Settings)
	FAnimNode_HUMAIdentity Node;

	virtual FText GetNodeTitle(ENodeTitleType::Type Type) const override
	{
		return NSLOCTEXT("HUMA", "IdentityNode", "HUMA Identity - Rotation Only");
	}
	virtual FText GetTooltipText() const override
	{
		return NSLOCTEXT("HUMA", "IdentityTooltip", "Use the per-component identity bind translations, animated local rotations, and unscaled hip displacement.");
	}
	virtual FText GetMenuCategory() const override { return NSLOCTEXT("HUMA", "Category", "HUMA"); }
};
