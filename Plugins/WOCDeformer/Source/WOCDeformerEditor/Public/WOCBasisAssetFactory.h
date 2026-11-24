// Copyright (c) 2025 WOCDeformer Plugin. All Rights Reserved.

#pragma once

#include "Factories/Factory.h"
#include "WOCBasisAssetFactory.generated.h"

UCLASS()
class UWOCBasisAssetFactory : public UFactory
{
	GENERATED_BODY()

public:
	UWOCBasisAssetFactory();

	virtual UObject* FactoryCreateNew(UClass* Class, UObject* InParent,
		FName Name, EObjectFlags Flags,
		UObject* Context, FFeedbackContext* Warn) override;

	virtual FText GetDisplayName() const override;
};
