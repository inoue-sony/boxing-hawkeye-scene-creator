#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "HUMAClothAppearance.generated.h"

class UMaterialInterface;

USTRUCT(BlueprintType)
struct FHUMAClothTeamAppearance
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Appearance")
    FString TeamName;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Appearance")
    TMap<FName, TObjectPtr<UMaterialInterface>> MaterialSlots;
};

UCLASS(BlueprintType)
class HUMACLOTHBIND_API UHUMAClothAppearance : public UDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Appearance")
    TMap<int32, FHUMAClothTeamAppearance> Teams;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Appearance")
    FString SourceDescription;
};

UCLASS()
class HUMACLOTHBIND_API UHUMAClothAppearanceLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="HUMA Cloth Bind|Appearance")
    static bool ApplyTeamAppearance(AActor* Player, int32 TeamId, UHUMAClothAppearance* Appearance);
};
