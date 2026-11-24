#include "HUMAClothBindFactory.h"
#include "HUMAClothBindAsset.h"
#include "HUMAClothBindComponent.h"
#include "HUMAClothBindAssetEditor.h"
#include "AssetToolsModule.h"
#include "AssetTypeActions_Base.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectIterator.h"

UHUMAClothBindFactory::UHUMAClothBindFactory()
{
    SupportedClass = UHUMAClothBindAsset::StaticClass();
    bCreateNew = true;
    bEditAfterNew = true;
}

UObject* UHUMAClothBindFactory::FactoryCreateNew(UClass* Class, UObject* Parent, FName Name, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn)
{
    return NewObject<UHUMAClothBindAsset>(Parent, Class, Name, Flags);
}

class FHUMAClothAssetActions : public FAssetTypeActions_Base
{
public:
    explicit FHUMAClothAssetActions(EAssetTypeCategories::Type InCategory) : Category(InCategory) {}
    virtual FText GetName() const override { return FText::FromString(TEXT("HUMA Cloth Bind")); }
    virtual FColor GetTypeColor() const override { return FColor(52, 156, 116); }
    virtual UClass* GetSupportedClass() const override { return UHUMAClothBindAsset::StaticClass(); }
    virtual uint32 GetCategories() override { return Category; }
    virtual void OpenAssetEditor(const TArray<UObject*>& Objects, TSharedPtr<IToolkitHost> Host) override
    {
        for (UObject* Object : Objects)
            if (auto* Asset = Cast<UHUMAClothBindAsset>(Object))
                MakeShared<FHUMAClothBindAssetEditor>()->Init(Asset,
                    Host.IsValid() ? EToolkitMode::WorldCentric : EToolkitMode::Standalone, Host);
    }
private:
    EAssetTypeCategories::Type Category;
};

class FHUMAClothBindEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
        const auto Category = Tools.RegisterAdvancedAssetCategory(TEXT("HUMA"), FText::FromString(TEXT("HUMA")));
        Actions = MakeShared<FHUMAClothAssetActions>(Category);
        Tools.RegisterAssetTypeActions(Actions.ToSharedRef());
        TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float DeltaTime)
        {
            for (TObjectIterator<UHUMAClothBindComponent> Iterator; Iterator; ++Iterator)
            {
                UHUMAClothBindComponent* Component = *Iterator;
                UWorld* World = Component->GetWorld();
                if (Component->IsTemplate() || !Component->IsRegistered() || !World ||
                    (World->WorldType != EWorldType::Editor && World->WorldType != EWorldType::EditorPreview) ||
                    !Component->bPreviewEnabled) continue;
                Component->TickComponent(DeltaTime, LEVELTICK_ViewportsOnly, nullptr);
            }
            return true;
        }));
    }
    virtual void ShutdownModule() override
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
        if (Actions.IsValid() && FModuleManager::Get().IsModuleLoaded(TEXT("AssetTools")))
            FModuleManager::GetModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().UnregisterAssetTypeActions(Actions.ToSharedRef());
        Actions.Reset();
    }
private:
    FTSTicker::FDelegateHandle TickHandle;
    TSharedPtr<FHUMAClothAssetActions> Actions;
};

IMPLEMENT_MODULE(FHUMAClothBindEditorModule, HUMAClothBindEditor)