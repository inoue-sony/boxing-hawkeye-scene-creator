#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "HUMAClothBindFactory.generated.h"

UCLASS()
class UHUMAClothBindFactory : public UFactory
{
    GENERATED_BODY()
public:
    UHUMAClothBindFactory();
    virtual UObject* FactoryCreateNew(UClass* Class, UObject* Parent, FName Name, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn) override;
};