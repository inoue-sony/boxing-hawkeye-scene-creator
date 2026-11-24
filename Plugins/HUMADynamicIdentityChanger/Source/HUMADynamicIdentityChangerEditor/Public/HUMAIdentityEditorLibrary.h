#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "HUMAIdentityEditorLibrary.generated.h"

class UAnimBlueprint;
class USkeletalMesh;
class UBlueprint;
class UHUMAIdentityData;

UCLASS()
class HUMADYNAMICIDENTITYCHANGEREDITOR_API UHUMAIdentityEditorLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "HUMA|Editor")
	static UHUMAIdentityData* ReadIdentityDataH5(const FString& Filename, USkeletalMesh* ReferenceMesh, FString& OutError);

	UFUNCTION(BlueprintCallable, Category = "HUMA|Editor")
	static UAnimBlueprint* CreatePostProcessBlueprint(USkeletalMesh* Mesh, UObject* ModelData, UObject* BasisAsset, const FString& PackageName);

	UFUNCTION(BlueprintCallable, Category = "HUMA|Editor")
	static TArray<UObject*> CreateRuntimePlaybackTemplates(const FString& Directory);

	UFUNCTION(BlueprintCallable, Category = "HUMA|Editor")
	static bool ConnectRuntimeTeamAppearance(UBlueprint* Blueprint, UObject* Appearance);
};
