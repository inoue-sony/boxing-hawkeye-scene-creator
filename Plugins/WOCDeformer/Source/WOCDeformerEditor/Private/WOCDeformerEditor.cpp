// Copyright (c) 2025 WOCDeformer Plugin. All Rights Reserved.

#include "WOCBasisAssetFactory.h"
#include "WOCBasisAsset.h"
#include "WOCBasisAssetDetails.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetTypeActions_Base.h"
#include "PropertyEditorModule.h"
#include "Modules/ModuleManager.h"

// ===========================================================================
// UWOCBasisAssetFactory
// ===========================================================================

UWOCBasisAssetFactory::UWOCBasisAssetFactory()
{
	SupportedClass = UWOCBasisAsset::StaticClass();
	bCreateNew     = true;
	bEditAfterNew  = true;
}

UObject* UWOCBasisAssetFactory::FactoryCreateNew(
	UClass* Class, UObject* InParent, FName Name,
	EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn)
{
	return NewObject<UWOCBasisAsset>(InParent, Class, Name, Flags);
}

FText UWOCBasisAssetFactory::GetDisplayName() const
{
	return FText::FromString(TEXT("WOC Basis Asset"));
}

// ===========================================================================
// Asset Type Actions
// ===========================================================================

class FWOCBasisAssetTypeActions : public FAssetTypeActions_Base
{
public:
	virtual FText    GetName()           const override { return FText::FromString(TEXT("WOC Basis Asset")); }
	virtual FColor   GetTypeColor()      const override { return FColor(0, 160, 255); }
	virtual UClass*  GetSupportedClass() const override { return UWOCBasisAsset::StaticClass(); }
	virtual uint32   GetCategories()           override { return EAssetTypeCategories::Animation; }
};

// ===========================================================================
// Module
// ===========================================================================

class FWOCDeformerEditorModule : public IModuleInterface
{
	TSharedPtr<FWOCBasisAssetTypeActions> AssetTypeActions;

	virtual void StartupModule() override
	{
		if (!GIsEditor) return;
		IAssetTools& AssetTools =
			FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		AssetTypeActions = MakeShared<FWOCBasisAssetTypeActions>();
		AssetTools.RegisterAssetTypeActions(AssetTypeActions.ToSharedRef());

		FPropertyEditorModule& PEM = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PEM.RegisterCustomClassLayout(
			UWOCBasisAsset::StaticClass()->GetFName(),
			FOnGetDetailCustomizationInstance::CreateStatic(&FWOCBasisAssetDetails::MakeInstance));
	}

	virtual void ShutdownModule() override
	{
		if (AssetTypeActions.IsValid() && FModuleManager::Get().IsModuleLoaded("AssetTools"))
		{
			IAssetTools& AssetTools =
				FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools").Get();
			AssetTools.UnregisterAssetTypeActions(AssetTypeActions.ToSharedRef());
		}

		if (GIsEditor && FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
		{
			FPropertyEditorModule& PEM = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
			PEM.UnregisterCustomClassLayout(UWOCBasisAsset::StaticClass()->GetFName());
		}
	}
};

IMPLEMENT_MODULE(FWOCDeformerEditorModule, WOCDeformerEditor)
