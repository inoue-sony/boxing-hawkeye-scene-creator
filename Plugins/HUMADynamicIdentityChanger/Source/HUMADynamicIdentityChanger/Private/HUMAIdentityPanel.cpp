#include "HUMAIdentity.h"

#include "Camera/CameraActor.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpectatorPawn.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"

namespace
{
	struct FCharacterOption
	{
		TWeakObjectPtr<UHUMAIdentityComponent> Identity;
		FString Label;
	};

	struct FBetaOption
	{
		int32 Index;
		FString Name;
	};

	class SHUMAIdentityPanel : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SHUMAIdentityPanel) {}
			SLATE_ARGUMENT(UHUMAIdentityPanel*, Panel)
		SLATE_END_ARGS()

		void Construct(const FArguments& Args)
		{
			Panel = Args._Panel;
			Characters = GatherCharacters();
			ChildSlot
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(12)
				[
					SNew(SBox).WidthOverride(620)
					[
						SNew(SBorder).Padding(12).BorderBackgroundColor(FLinearColor(0.035f, 0.045f, 0.065f, 0.96f))
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
							[
								SNew(STextBlock).Text(NSLOCTEXT("HUMA", "Title", "HUMA Dynamic Identity"))
							]
							+ SVerticalBox::Slot().AutoHeight()
							[
								SAssignNew(CharacterPicker, SComboBox<TSharedPtr<FCharacterOption>>)
								.OptionsSource(&Characters)
								.OnGenerateWidget_Lambda([](TSharedPtr<FCharacterOption> Option)
								{
									return SNew(STextBlock).Text(FText::FromString(Option->Identity.IsValid() ? Option->Label : TEXT("Unavailable")));
								})
								.OnSelectionChanged_Lambda([this](TSharedPtr<FCharacterOption> Option, ESelectInfo::Type)
								{
									Selected = Option;
									if (Panel.IsValid()) { Panel->TargetIdentity = Option.IsValid() ? Option->Identity.Get() : nullptr; }
									RefreshRows();
								})
								[
									SNew(STextBlock).Text_Lambda([this]
									{
										return FText::FromString(Selected.IsValid() ? Selected->Label : TEXT("Select an actor"));
									})
								]
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 10)
							[
								SNew(SSearchBox)
								.HintText(NSLOCTEXT("HUMA", "Search", "Filter by beta number or name"))
								.OnTextChanged_Lambda([this](const FText& Text) { Filter = Text.ToString(); RefreshRows(); })
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
							[
								SNew(STextBlock)
								.AutoWrapText(true)
								.Text(NSLOCTEXT("HUMA", "Range", "Drag: -4 to +4. Type any finite value. Changes affect only the selected character."))
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
							[
								SNew(SButton)
								.Text(NSLOCTEXT("HUMA", "Reset", "Reset selected character to beta = 0"))
								.OnClicked_Lambda([this]
								{
									if (Panel.IsValid() && IsValid(Panel->TargetIdentity)) { Panel->TargetIdentity->ResetIdentity(); }
									return FReply::Handled();
								})
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
							[
								SNew(STextBlock).AutoWrapText(true)
								.Text_Lambda([this]
								{
									if (!Panel.IsValid() || !IsValid(Panel->TargetIdentity)) { return NSLOCTEXT("HUMA", "NoActors", "No actors with HUMA Identity components found."); }
									const UHUMAIdentityComponent* Identity = Panel->TargetIdentity;
									if (!Identity->LastError.IsEmpty()) { return FText::FromString(TEXT("ERROR: ") + Identity->LastError); }
									return FText::FromString(FString::Printf(TEXT("%d independent characters | %d identity betas"),
										Characters.Num(), Identity->Betas.Num()));
								})
							]
							+ SVerticalBox::Slot().FillHeight(1)
							[
								SAssignNew(List, SListView<TSharedPtr<FBetaOption>>)
								.ListItemsSource(&Rows)
								.ScrollbarVisibility(EVisibility::Visible)
								.SelectionMode(ESelectionMode::None)
								.OnGenerateRow(this, &SHUMAIdentityPanel::MakeRow)
							]
						]
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1)
				[
					SNew(SBox)
				]
			];
			if (!Characters.IsEmpty())
			{
				TSharedPtr<FCharacterOption> Initial = Characters[0];
				for (const TSharedPtr<FCharacterOption>& Option : Characters)
				{
					if (Option->Identity == Args._Panel->TargetIdentity) { Initial = Option; break; }
				}
				CharacterPicker->SetSelectedItem(Initial);
			}
		}

		virtual void Tick(const FGeometry& Geometry, double CurrentTime, float DeltaTime) override
		{
			SCompoundWidget::Tick(Geometry, CurrentTime, DeltaTime);
			if (CurrentTime < NextDiscoveryTime) { return; }
			NextDiscoveryTime = CurrentTime + 0.5;
			TArray<TSharedPtr<FCharacterOption>> Available = GatherCharacters();
			bool bChanged = Available.Num() != Characters.Num();
			for (int32 Index = 0; !bChanged && Index < Available.Num(); ++Index)
			{
				bChanged = Available[Index]->Identity != Characters[Index]->Identity || Available[Index]->Label != Characters[Index]->Label;
			}
			if (!bChanged) { return; }
			Characters = MoveTemp(Available);
			CharacterPicker->RefreshOptions();
			TSharedPtr<FCharacterOption> Choice;
			for (const TSharedPtr<FCharacterOption>& Option : Characters)
			{
				if (Panel.IsValid() && Option->Identity == Panel->TargetIdentity) { Choice = Option; break; }
			}
			if (!Choice && !Characters.IsEmpty()) { Choice = Characters[0]; }
			CharacterPicker->SetSelectedItem(Choice);
		}

	private:
		TArray<TSharedPtr<FCharacterOption>> GatherCharacters() const
		{
			TArray<TSharedPtr<FCharacterOption>> Options;
			if (!Panel.IsValid()) { return Options; }
			for (UHUMAIdentityComponent* Identity : Panel->GetAvailableIdentities())
			{
				TSharedPtr<FCharacterOption> Option = MakeShared<FCharacterOption>();
				Option->Identity = Identity;
				AActor* Actor = Identity->GetOwner();
				if (const AHUMAIdentityCharacter* Character = Cast<AHUMAIdentityCharacter>(Actor))
				{
					Option->Label = Character->DisplayName;
				}
				else
				{
					Option->Label = Actor->GetActorNameOrLabel();
				}
				TInlineComponentArray<UHUMAIdentityComponent*> Identities(Actor);
				if (Identities.Num() > 1) { Option->Label += TEXT(" / ") + Identity->GetName(); }
				Options.Add(Option);
			}
			Options.Sort([](const TSharedPtr<FCharacterOption>& A, const TSharedPtr<FCharacterOption>& B)
			{
				return A->Label == B->Label ? A->Identity->GetPathName() < B->Identity->GetPathName() : A->Label < B->Label;
			});
			return Options;
		}

		void RefreshRows()
		{
			Rows.Reset();
			const UHUMAIdentityComponent* Identity = Panel.IsValid() ? Panel->TargetIdentity.Get() : nullptr;
			if (IsValid(Identity) && Identity->IdentityData)
			{
				const TArray<FName>& Names = Identity->IdentityData->BetaNames;
				for (int32 Index = 0; Index < Names.Num(); ++Index)
				{
					const FString Name = Names[Index].ToString();
					if (Filter.IsEmpty() || Name.Contains(Filter))
					{
						TSharedPtr<FBetaOption> Row = MakeShared<FBetaOption>();
						Row->Index = Index;
						Row->Name = Name;
						Rows.Add(Row);
					}
				}
			}
			if (List) { List->RequestListRefresh(); }
		}

		TSharedRef<ITableRow> MakeRow(TSharedPtr<FBetaOption> Row, const TSharedRef<STableViewBase>& Owner)
		{
			return SNew(STableRow<TSharedPtr<FBetaOption>>, Owner)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[
					SNew(STextBlock).Text(FText::FromString(Row->Name)).ToolTipText(FText::FromString(Row->Name))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 12, 0)
				[
					SNew(SBox).WidthOverride(170)
					[
						SNew(SSlider)
						.MinValue(-4.0f).MaxValue(4.0f).StepSize(0.01f)
						.Value_Lambda([this, Row]
						{
							const UHUMAIdentityComponent* Identity = Panel.IsValid() ? Panel->TargetIdentity.Get() : nullptr;
							return IsValid(Identity) && Identity->Betas.IsValidIndex(Row->Index)
								? FMath::Clamp(Identity->Betas[Row->Index], -4.0f, 4.0f) : 0.0f;
						})
						.OnValueChanged_Lambda([this, Row](float Value)
						{
							if (Panel.IsValid()) { Panel->SetSelectedBeta(Row->Index, Value); }
						})
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox).WidthOverride(110)
					[
						SNew(SNumericEntryBox<float>)
						.AllowSpin(true)
						.MinSliderValue(-4.0f).MaxSliderValue(4.0f).Delta(0.01f)
						.Value_Lambda([this, Row]() -> TOptional<float>
						{
							const UHUMAIdentityComponent* Identity = Panel.IsValid() ? Panel->TargetIdentity.Get() : nullptr;
							if (IsValid(Identity) && Identity->Betas.IsValidIndex(Row->Index))
							{
								return Identity->Betas[Row->Index];
							}
							return {};
						})
						.OnValueChanged_Lambda([this, Row](float Value)
						{
							if (Panel.IsValid()) { Panel->SetSelectedBeta(Row->Index, Value); }
						})
					]
				]
			];
		}

		FString Filter;
		TWeakObjectPtr<UHUMAIdentityPanel> Panel;
		TSharedPtr<FCharacterOption> Selected;
		TArray<TSharedPtr<FCharacterOption>> Characters;
		TArray<TSharedPtr<FBetaOption>> Rows;
		TSharedPtr<SComboBox<TSharedPtr<FCharacterOption>>> CharacterPicker;
		TSharedPtr<SListView<TSharedPtr<FBetaOption>>> List;
		double NextDiscoveryTime = 0.0;
	};
}

TSharedRef<SWidget> UHUMAIdentityPanel::RebuildWidget()
{
	return SNew(SHUMAIdentityPanel).Panel(this);
}

TArray<UHUMAIdentityComponent*> UHUMAIdentityPanel::GetAvailableIdentities() const
{
	TArray<UHUMAIdentityComponent*> Result;
	if (!GetWorld()) { return Result; }
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		if (It->IsActorBeingDestroyed()) { continue; }
		TInlineComponentArray<UHUMAIdentityComponent*> Components(*It);
		for (UHUMAIdentityComponent* Component : Components)
		{
			if (Component->IdentityData && Component->TargetMesh) { Result.Add(Component); }
		}
	}
	return Result;
}

bool UHUMAIdentityPanel::SetSelectedBeta(int32 Index, float Value)
{
	if (!IsValid(TargetIdentity))
	{
		UE_LOG(LogHUMAIdentity, Error, TEXT("Choose an Identity component before editing beta."));
		return false;
	}
	return TargetIdentity->SetBeta(Index, Value);
}

void AHUMAIdentityHUD::BeginPlay()
{
	Super::BeginPlay();
	APlayerController* Controller = GetOwningPlayerController();
	if (!Controller) { UE_LOG(LogHUMAIdentity, Error, TEXT("HUMA HUD requires a player controller.")); return; }
	Panel = CreateWidget<UHUMAIdentityPanel>(Controller, UHUMAIdentityPanel::StaticClass());
	if (!Panel) { UE_LOG(LogHUMAIdentity, Error, TEXT("Cannot create the HUMA identity panel.")); return; }
	Panel->AddToViewport(10);
	Controller->bShowMouseCursor = true;
	FInputModeGameAndUI Input;
	Input.SetHideCursorDuringCapture(false);
	Controller->SetInputMode(Input);
	for (TActorIterator<ACameraActor> It(GetWorld()); It; ++It)
	{
		if (It->ActorHasTag(TEXT("HUMAIdentityDemoCamera"))) { Controller->SetViewTarget(*It); break; }
	}
}

AHUMAIdentityGameMode::AHUMAIdentityGameMode()
{
	HUDClass = AHUMAIdentityHUD::StaticClass();
	DefaultPawnClass = ASpectatorPawn::StaticClass();
}
