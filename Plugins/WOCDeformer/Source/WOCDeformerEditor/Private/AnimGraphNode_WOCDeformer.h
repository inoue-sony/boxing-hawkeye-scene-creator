// Copyright (c) 2025 WOCDeformer Plugin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AnimGraphNode_Base.h"
#include "AnimNode_WOCDeformer.h"

#include "AnimGraphNode_WOCDeformer.generated.h"

UCLASS()
class UAnimGraphNode_WOCDeformer : public UAnimGraphNode_Base
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = Settings)
	FAnimNode_WOCDeformer Node;

	// UEdGraphNode
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual FLinearColor GetNodeTitleColor() const override;
	virtual FText GetMenuCategory() const override;
	virtual void ValidateAnimNodeDuringCompilation(USkeleton* ForSkeleton, FCompilerResultsLog& MessageLog) override;
};
