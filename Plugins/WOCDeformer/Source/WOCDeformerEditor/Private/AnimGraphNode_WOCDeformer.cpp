#include "AnimGraphNode_WOCDeformer.h"
#include "WOCBasisAsset.h"
#include "Kismet2/CompilerResultsLog.h"

#define LOCTEXT_NAMESPACE "WOCDeformerEditor"

void UAnimGraphNode_WOCDeformer::ValidateAnimNodeDuringCompilation(USkeleton* ForSkeleton, FCompilerResultsLog& MessageLog)
{
	Super::ValidateAnimNodeDuringCompilation(ForSkeleton, MessageLog);
	if (Node.ModelData && Node.BasisAsset && !Node.BasisAsset->CacheModelMetadata(Node.ModelData))
		MessageLog.Error(TEXT("@@ cannot capture cook-safe WOC model metadata. See the WOC log."), this);
}

FText UAnimGraphNode_WOCDeformer::GetNodeTitle(ENodeTitleType::Type /*TitleType*/) const
{
	return LOCTEXT("AnimGraphNode_WOCDeformer_Title", "WOC Deformer");
}

FText UAnimGraphNode_WOCDeformer::GetTooltipText() const
{
	return LOCTEXT("AnimGraphNode_WOCDeformer_Tooltip",
		"WOC Deformer that performs batched ONNX inference with LOD switching. "
		"Supplies GPU coefficient and rotation buffers to the Deformer Graph.");
}

FLinearColor UAnimGraphNode_WOCDeformer::GetNodeTitleColor() const
{
	return FLinearColor(0.0f, 0.6f, 1.0f);
}

FText UAnimGraphNode_WOCDeformer::GetMenuCategory() const
{
	return LOCTEXT("AnimGraphNode_WOCDeformer_Category", "WOC Deformer");
}

#undef LOCTEXT_NAMESPACE
