#include "HUMAIdentityEditorLibrary.h"

#include "HUMARuntimePlayback.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "EdGraph/EdGraphNodeUtils.h"
#include "EdGraphSchema_K2.h"
#include "Factories/BlueprintFactory.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"

namespace
{
	template<class T>
	T* Widget(UWidgetBlueprint* Blueprint, const TCHAR* Name)
	{
		T* Result = Blueprint->WidgetTree->ConstructWidget<T>(T::StaticClass(), FName(Name));
		Result->bIsVariable = true;
		return Result;
	}

	void Text(UTextBlock* Block, const TCHAR* Value, int32 Size)
	{
		Block->SetText(FText::FromString(Value));
		FSlateFontInfo Font = Block->GetFont();
		Font.Size = Size;
		Block->SetFont(Font);
		Block->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	}

	void Binding(UWidgetBlueprint* Blueprint, UWidget* Target, const TCHAR* Property, const TCHAR* Function)
	{
		FDelegateEditorBinding Item;
		Item.ObjectName = Target->GetName();
		Item.PropertyName = FName(Property);
		Item.FunctionName = FName(Function);
		Item.Kind = EBindingKind::Function;
		Blueprint->Bindings.Add(Item);
	}

	bool Connect(UEdGraph* Graph, UEdGraphPin* A, UEdGraphPin* B)
	{
		return A && B && Graph->GetSchema()->TryCreateConnection(A, B);
	}

	UK2Node_CallFunction* Call(UEdGraph* Graph, UClass* Class, const TCHAR* Name, int32 X, int32 Y)
	{
		const UFunction* Function = Class->FindFunctionByName(FName(Name));
		if (!Function) { return nullptr; }
		FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
		UK2Node_CallFunction* Node = Creator.CreateNode();
		Node->SetFromFunction(Function);
		Node->NodePosX = X;
		Node->NodePosY = Y;
		Creator.Finalize();
		return Node;
	}

	bool ButtonEvent(UWidgetBlueprint* Blueprint, UWidget* Target, const TCHAR* DelegateName,
		const TCHAR* FunctionName, int32 Y, const TCHAR* FloatParameter = nullptr)
	{
		FObjectProperty* Property = FindFProperty<FObjectProperty>(Blueprint->SkeletonGeneratedClass, Target->GetFName());
		FMulticastDelegateProperty* Delegate = FindFProperty<FMulticastDelegateProperty>(Target->GetClass(), FName(DelegateName));
		UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
		if (!Property || !Delegate || !Graph) { return false; }
		FGraphNodeCreator<UK2Node_ComponentBoundEvent> Creator(*Graph);
		UK2Node_ComponentBoundEvent* Event = Creator.CreateNode();
		Event->InitializeComponentBoundEventParams(Property, Delegate);
		Event->NodePosY = Y;
		Creator.Finalize();
		UK2Node_CallFunction* Function = Call(Graph, UHUMARuntimePlaybackWidget::StaticClass(), FunctionName, 350, Y);
		if (!Function || !Connect(Graph, Event->FindPin(UEdGraphSchema_K2::PN_Then), Function->GetExecPin())) { return false; }
		return !FloatParameter || Connect(Graph, Event->FindPin(FName(FloatParameter)), Function->FindPin(FName(FloatParameter)));
	}

	bool Compile(UBlueprint* Blueprint)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		if (Blueprint->Status == BS_Error)
		{
			UE_LOG(LogHUMAIdentity, Error, TEXT("Runtime Blueprint compilation failed: %s"), *Blueprint->GetPathName());
			return false;
		}
		return true;
	}
}

TArray<UObject*> UHUMAIdentityEditorLibrary::CreateRuntimePlaybackTemplates(const FString& Directory)
{
	const FString WidgetPackage = Directory / TEXT("WBP_HUMARuntimeControls");
	const FString HostPackage = Directory / TEXT("BP_HUMARuntimePlayback");
	for (const FString& Name : {WidgetPackage, HostPackage})
	{
		if (!Name.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(Name)
			|| FPackageName::DoesPackageExist(Name)
			|| FindObject<UObject>(nullptr, *(Name + TEXT(".") + FPackageName::GetLongPackageAssetName(Name))))
		{
			UE_LOG(LogHUMAIdentity, Error, TEXT("Invalid or existing runtime template package: %s"), *Name);
			return {};
		}
	}
	UWidgetBlueprintFactory* WidgetFactory = NewObject<UWidgetBlueprintFactory>();
	WidgetFactory->ParentClass = UHUMARuntimePlaybackWidget::StaticClass();
	UWidgetBlueprint* UI = Cast<UWidgetBlueprint>(WidgetFactory->FactoryCreateNew(
		UWidgetBlueprint::StaticClass(), CreatePackage(*WidgetPackage), TEXT("WBP_HUMARuntimeControls"),
		RF_Public | RF_Standalone, nullptr, GWarn));
	if (!UI || !UI->WidgetTree) { UE_LOG(LogHUMAIdentity, Error, TEXT("Could not create the runtime Widget Blueprint.")); return {}; }
	UCanvasPanel* Canvas = Widget<UCanvasPanel>(UI, TEXT("Canvas"));
	UI->WidgetTree->RootWidget = Canvas;
	UBorder* Border = Widget<UBorder>(UI, TEXT("ControlBackground"));
	Border->SetBrushColor(FLinearColor(0.02f, 0.025f, 0.04f, 0.94f));
	Border->SetPadding(FMargin(18, 12));
	UCanvasPanelSlot* CanvasSlot = Canvas->AddChildToCanvas(Border);
	CanvasSlot->SetAnchors(FAnchors(0, 1, 1, 1));
	CanvasSlot->SetOffsets(FMargin(24, -170, 24, 146));
	UVerticalBox* Rows = Widget<UVerticalBox>(UI, TEXT("ControlRows"));
	Border->SetContent(Rows);
	UTextBlock* Title = Widget<UTextBlock>(UI, TEXT("Title"));
	Text(Title, TEXT("HUMA Runtime Motion"), 20);
	Rows->AddChildToVerticalBox(Title)->SetPadding(FMargin(0, 0, 0, 8));
	UHorizontalBox* Buttons = Widget<UHorizontalBox>(UI, TEXT("TimelineRow"));
	Rows->AddChildToVerticalBox(Buttons);
	UButton* Play = Widget<UButton>(UI, TEXT("PlayPauseButton"));
	UTextBlock* PlayText = Widget<UTextBlock>(UI, TEXT("PlayPauseLabel"));
	Text(PlayText, TEXT("Pause"), 18);
	Play->SetContent(PlayText);
	CastChecked<UButtonSlot>(PlayText->Slot)->SetPadding(FMargin(18, 8));
	Buttons->AddChildToHorizontalBox(Play)->SetPadding(FMargin(0, 0, 10, 0));
	UButton* Restart = Widget<UButton>(UI, TEXT("RestartButton"));
	UTextBlock* RestartText = Widget<UTextBlock>(UI, TEXT("RestartLabel"));
	Text(RestartText, TEXT("Restart"), 18);
	Restart->SetContent(RestartText);
	CastChecked<UButtonSlot>(RestartText->Slot)->SetPadding(FMargin(18, 8));
	Buttons->AddChildToHorizontalBox(Restart)->SetPadding(FMargin(0, 0, 18, 0));
	USlider* Slider = Widget<USlider>(UI, TEXT("SeekBar"));
	Slider->SetMinValue(0);
	Slider->SetMaxValue(1);
	Slider->SetStepSize(0.0001f);
	UHorizontalBoxSlot* SliderSlot = Buttons->AddChildToHorizontalBox(Slider);
	SliderSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	SliderSlot->SetVerticalAlignment(VAlign_Center);
	UTextBlock* Time = Widget<UTextBlock>(UI, TEXT("TimeLabel"));
	Text(Time, TEXT("0.000 / 0.000 s"), 18);
	UHorizontalBoxSlot* TimeSlot = Buttons->AddChildToHorizontalBox(Time);
	TimeSlot->SetPadding(FMargin(18, 0, 0, 0));
	TimeSlot->SetVerticalAlignment(VAlign_Center);
	UTextBlock* Status = Widget<UTextBlock>(UI, TEXT("StatusLabel"));
	Text(Status, TEXT("Loading dataset..."), 14);
	Status->SetAutoWrapText(true);
	Rows->AddChildToVerticalBox(Status)->SetPadding(FMargin(0, 8, 0, 0));
	Binding(UI, PlayText, TEXT("Text"), TEXT("GetPlayButtonLabel"));
	Binding(UI, Time, TEXT("Text"), TEXT("GetTimeLabel"));
	Binding(UI, Status, TEXT("Text"), TEXT("GetStatusLabel"));
	Binding(UI, Slider, TEXT("Value"), TEXT("GetNormalizedTime"));
	for (UWidget* Control : TArray<UWidget*>{Play, Restart, Slider})
	{
		Binding(UI, Control, TEXT("bIsEnabled"), TEXT("CanControlPlayback"));
	}
	if (!Compile(UI)) { return {}; }
	if (!ButtonEvent(UI, Play, TEXT("OnClicked"), TEXT("TogglePlayback"), 0)
		|| !ButtonEvent(UI, Restart, TEXT("OnClicked"), TEXT("RestartPlayback"), 180)
		|| !ButtonEvent(UI, Slider, TEXT("OnValueChanged"), TEXT("SeekNormalized"), 360, TEXT("Value"))
		|| !ButtonEvent(UI, Slider, TEXT("OnMouseCaptureBegin"), TEXT("BeginScrub"), 540)
		|| !ButtonEvent(UI, Slider, TEXT("OnMouseCaptureEnd"), TEXT("EndScrub"), 720)
		|| !ButtonEvent(UI, Slider, TEXT("OnControllerCaptureBegin"), TEXT("BeginScrub"), 900)
		|| !ButtonEvent(UI, Slider, TEXT("OnControllerCaptureEnd"), TEXT("EndScrub"), 1080))
	{
		UE_LOG(LogHUMAIdentity, Error, TEXT("Could not connect runtime UMG events to playback functions."));
		return {};
	}
	if (!Compile(UI)) { return {}; }

	UBlueprintFactory* HostFactory = NewObject<UBlueprintFactory>();
	HostFactory->ParentClass = AHUMARuntimePlaybackHost::StaticClass();
	UBlueprint* Host = Cast<UBlueprint>(HostFactory->FactoryCreateNew(UBlueprint::StaticClass(),
		CreatePackage(*HostPackage), TEXT("BP_HUMARuntimePlayback"), RF_Public | RF_Standalone, nullptr, GWarn));
	if (!Host) { UE_LOG(LogHUMAIdentity, Error, TEXT("Could not create the runtime controller Blueprint.")); return {}; }
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Host);
	if (!Graph) { UE_LOG(LogHUMAIdentity, Error, TEXT("Runtime controller has no EventGraph.")); return {}; }
	UK2Node_Event* Begin = FBlueprintEditorUtils::FindOverrideForFunction(Host, AActor::StaticClass(), TEXT("ReceiveBeginPlay"));
	if (!Begin)
	{
		FGraphNodeCreator<UK2Node_Event> Creator(*Graph);
		Begin = Creator.CreateNode();
		Begin->EventReference.SetExternalMember(TEXT("ReceiveBeginPlay"), AActor::StaticClass());
		Begin->bOverrideFunction = true;
		Creator.Finalize();
	}
	FGraphNodeCreator<UK2Node_VariableGet> GetterCreator(*Graph);
	UK2Node_VariableGet* Getter = GetterCreator.CreateNode();
	Getter->VariableReference.SetSelfMember(TEXT("Playback"));
	Getter->NodePosX = 180;
	Getter->NodePosY = 160;
	GetterCreator.Finalize();
	UK2Node_CallFunction* Start = Call(Graph, UHUMARuntimePlaybackComponent::StaticClass(), TEXT("LoadAndPlay"), 350, 0);
	UK2Node_CallFunction* Show = Call(Graph, AHUMARuntimePlaybackHost::StaticClass(), TEXT("ShowPlaybackControls"), 700, 0);
	if (!Start || !Show || !Connect(Graph, Getter->GetValuePin(), Start->FindPin(UEdGraphSchema_K2::PN_Self))
		|| !Connect(Graph, Begin->FindPin(UEdGraphSchema_K2::PN_Then), Start->GetExecPin())
		|| !Connect(Graph, Start->GetThenPin(), Show->GetExecPin()) || !Compile(Host))
	{
		UE_LOG(LogHUMAIdentity, Error, TEXT("Could not connect BeginPlay -> LoadAndPlay -> ShowPlaybackControls."));
		return {};
	}
	AHUMARuntimePlaybackHost* Default = CastChecked<AHUMARuntimePlaybackHost>(Host->GeneratedClass->GetDefaultObject());
	Default->ControlsClass = UI->GeneratedClass;
	for (UBlueprint* Blueprint : TArray<UBlueprint*>{UI, Host})
	{
		FAssetRegistryModule::AssetCreated(Blueprint);
		Blueprint->MarkPackageDirty();
	}
	return {UI, Host};
}

bool UHUMAIdentityEditorLibrary::ConnectRuntimeTeamAppearance(UBlueprint* Blueprint, UObject* Appearance)
{
	UClass* Library = LoadClass<UObject>(nullptr, TEXT("/Script/HUMAClothBind.HUMAClothAppearanceLibrary"));
	UEdGraph* Graph = Blueprint ? FBlueprintEditorUtils::FindEventGraph(Blueprint) : nullptr;
	if (!Library || !Appearance || !Graph || !Blueprint->GeneratedClass->IsChildOf(AHUMARuntimePlaybackHost::StaticClass()))
	{
		UE_LOG(LogHUMAIdentity, Error, TEXT("Team appearance wiring requires a runtime host Blueprint, cloth appearance data and the cloth plugin."));
		return false;
	}
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (const auto* Event = Cast<UK2Node_ComponentBoundEvent>(Node))
		{
			if (Event->ComponentPropertyName == TEXT("Playback") && Event->DelegatePropertyName == TEXT("OnPlayerSpawned"))
			{
				UE_LOG(LogHUMAIdentity, Error, TEXT("OnPlayerSpawned is already wired; preserve/edit the existing Blueprint event."));
				return false;
			}
		}
	}
	const FObjectProperty* Property = FindFProperty<FObjectProperty>(Blueprint->GeneratedClass, TEXT("Playback"));
	const FMulticastDelegateProperty* Delegate = FindFProperty<FMulticastDelegateProperty>(
		UHUMARuntimePlaybackComponent::StaticClass(), TEXT("OnPlayerSpawned"));
	if (!Property || !Delegate) return false;
	FGraphNodeCreator<UK2Node_ComponentBoundEvent> EventCreator(*Graph);
	auto* Event = EventCreator.CreateNode();
	Event->InitializeComponentBoundEventParams(Property, Delegate);
	Event->NodePosY = 400;
	EventCreator.Finalize();
	auto* Apply = Call(Graph, Library, TEXT("ApplyTeamAppearance"), 600, 400);
	FGraphNodeCreator<UK2Node_VariableGet> TeamCreator(*Graph);
	auto* Team = TeamCreator.CreateNode();
	Team->VariableReference.SetExternalMember(TEXT("TeamId"), UHUMARuntimePoseComponent::StaticClass());
	Team->NodePosX = 300;
	Team->NodePosY = 600;
	TeamCreator.Finalize();
	if (!Apply || !Apply->FindPin(TEXT("Appearance"))) return false;
	Apply->FindPin(TEXT("Appearance"))->DefaultObject = Appearance;
	if (!Connect(Graph, Event->FindPin(UEdGraphSchema_K2::PN_Then), Apply->GetExecPin())
		|| !Connect(Graph, Event->FindPin(TEXT("Player")), Apply->FindPin(TEXT("Player")))
		|| !Connect(Graph, Event->FindPin(TEXT("RuntimePose")), Team->FindPin(UEdGraphSchema_K2::PN_Self))
		|| !Connect(Graph, Team->GetValuePin(), Apply->FindPin(TEXT("TeamId")))) return false;
	return Compile(Blueprint);
}
