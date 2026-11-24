#include "HUMAIdentityEditorLibrary.h"

#include "AnimGraphNode_HUMAIdentity.h"
#include "AnimGraphNode_LinkedInputPose.h"
#include "AnimGraphNode_Root.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNodeUtils.h"
#include "Engine/SkeletalMesh.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"

namespace
{
	UEdGraphPin* PosePin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == Direction && Pin->PinType.PinSubCategoryObject == FPoseLink::StaticStruct()) { return Pin; }
		}
		return nullptr;
	}

	bool ConnectPose(UEdGraph* Graph, UEdGraphNode* From, UEdGraphNode* To)
	{
		UEdGraphPin* Output = PosePin(From, EGPD_Output);
		UEdGraphPin* Input = PosePin(To, EGPD_Input);
		return Output && Input && Graph->GetSchema()->TryCreateConnection(Output, Input);
	}
}

UAnimBlueprint* UHUMAIdentityEditorLibrary::CreatePostProcessBlueprint(
	USkeletalMesh* Mesh, UObject* ModelData, UObject* BasisAsset, const FString& PackageName)
{
	if (!Mesh || !ModelData || !BasisAsset || !PackageName.StartsWith(TEXT("/Game/"))
		|| !FPackageName::IsValidLongPackageName(PackageName))
	{
		UE_LOG(LogHUMAIdentity, Error, TEXT("Post-process template requires a mesh, WOC assets and a valid project package name."));
		return nullptr;
	}
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	if (FPackageName::DoesPackageExist(PackageName) || FindObject<UObject>(nullptr, *ObjectPath))
	{
		UE_LOG(LogHUMAIdentity, Error, TEXT("Refusing to replace an existing asset: %s"), *ObjectPath);
		return nullptr;
	}
	UClass* WOCClass = LoadClass<UAnimGraphNode_Base>(nullptr, TEXT("/Script/WOCDeformerEditor.AnimGraphNode_WOCDeformer"));
	if (!WOCClass)
	{
		UE_LOG(LogHUMAIdentity, Error, TEXT("Enable the existing WOCDeformer plugin before creating this template."));
		return nullptr;
	}
	UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
	Factory->ParentClass = UAnimInstance::StaticClass();
	Factory->TargetSkeleton = Mesh->GetSkeleton();
	UPackage* Package = CreatePackage(*PackageName);
	UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(Factory->FactoryCreateNew(
		UAnimBlueprint::StaticClass(), Package, *AssetName, RF_Public | RF_Standalone, nullptr, GWarn));
	if (!Blueprint) { UE_LOG(LogHUMAIdentity, Error, TEXT("Cannot create animation blueprint.")); return nullptr; }
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	UEdGraph* Graph = nullptr;
	UAnimGraphNode_Root* Root = nullptr;
	for (UEdGraph* Candidate : Graphs)
	{
		if (Candidate->GetFName() == TEXT("AnimGraph"))
		{
			Graph = Candidate;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UAnimGraphNode_Root* CandidateRoot = Cast<UAnimGraphNode_Root>(Node)) { Root = CandidateRoot; }
			}
		}
	}
	if (!Graph || !Root) { UE_LOG(LogHUMAIdentity, Error, TEXT("Animation factory did not create an AnimGraph root.")); return nullptr; }
	FGraphNodeCreator<UAnimGraphNode_LinkedInputPose> InputCreator(*Graph);
	UAnimGraphNode_LinkedInputPose* Input = InputCreator.CreateNode();
	InputCreator.Finalize();
	FGraphNodeCreator<UAnimGraphNode_HUMAIdentity> IdentityCreator(*Graph);
	UAnimGraphNode_HUMAIdentity* Identity = IdentityCreator.CreateNode();
	IdentityCreator.Finalize();
	UAnimGraphNode_Base* WOC = NewObject<UAnimGraphNode_Base>(Graph, WOCClass, NAME_None, RF_Transactional);
	Graph->AddNode(WOC, false, false);
	WOC->CreateNewGuid();
	WOC->PostPlacedNewNode();
	FStructProperty* NodeProperty = FindFProperty<FStructProperty>(WOCClass, TEXT("Node"));
	if (!NodeProperty) { UE_LOG(LogHUMAIdentity, Error, TEXT("WOC graph node has no Node struct.")); return nullptr; }
	void* NodeData = NodeProperty->ContainerPtrToValuePtr<void>(WOC);
	FObjectPropertyBase* ModelProperty = FindFProperty<FObjectPropertyBase>(NodeProperty->Struct, TEXT("ModelData"));
	FObjectPropertyBase* BasisProperty = FindFProperty<FObjectPropertyBase>(NodeProperty->Struct, TEXT("BasisAsset"));
	if (!ModelProperty || !BasisProperty || !ModelData->IsA(ModelProperty->PropertyClass) || !BasisAsset->IsA(BasisProperty->PropertyClass))
	{
		UE_LOG(LogHUMAIdentity, Error, TEXT("WOC ModelData/BasisAsset property types do not match the supplied assets."));
		return nullptr;
	}
	ModelProperty->SetObjectPropertyValue_InContainer(NodeData, ModelData);
	BasisProperty->SetObjectPropertyValue_InContainer(NodeData, BasisAsset);
	WOC->AllocateDefaultPins();
	if (UEdGraphPin* Pin = WOC->FindPin(TEXT("ModelData"))) { Pin->DefaultObject = ModelData; }
	if (UEdGraphPin* Pin = WOC->FindPin(TEXT("BasisAsset"))) { Pin->DefaultObject = BasisAsset; }
	Input->NodePosX = -600;
	Identity->NodePosX = -350;
	WOC->NodePosX = -100;
	Root->NodePosX = 180;
	if (!ConnectPose(Graph, Input, Identity) || !ConnectPose(Graph, Identity, WOC) || !ConnectPose(Graph, WOC, Root))
	{
		UE_LOG(LogHUMAIdentity, Error, TEXT("Cannot connect Input Pose -> HUMA Identity -> WOC -> Output Pose."));
		return nullptr;
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (Blueprint->Status == BS_Error)
	{
		UE_LOG(LogHUMAIdentity, Error, TEXT("Post-process animation blueprint failed compilation."));
		return nullptr;
	}
	FAssetRegistryModule::AssetCreated(Blueprint);
	Blueprint->MarkPackageDirty();
	return Blueprint;
}
