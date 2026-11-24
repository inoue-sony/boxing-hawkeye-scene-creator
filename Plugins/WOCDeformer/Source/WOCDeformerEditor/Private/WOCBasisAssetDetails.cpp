// Copyright (c) 2025 WOCDeformer Plugin. All Rights Reserved.

#include "WOCBasisAssetDetails.h"
#include "WOCBasisAsset.h"

#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailGroup.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Styling/AppStyle.h"
#include "IDesktopPlatform.h"
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/Paths.h"

TSharedRef<IDetailCustomization> FWOCBasisAssetDetails::MakeInstance()
{
	return MakeShareable(new FWOCBasisAssetDetails);
}

void FWOCBasisAssetDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	if (Objects.Num() != 1) { return; }

	UWOCBasisAsset* Asset = Cast<UWOCBasisAsset>(Objects[0].Get());
	if (!Asset) { return; }

	// Hide the default display of LODs property (replaced with custom display)
	DetailBuilder.HideProperty(DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UWOCBasisAsset, LODs)));

	// --- Top section: Add LOD from File button ---
	IDetailCategoryBuilder& ActionCat = DetailBuilder.EditCategory(
		TEXT("WOC Basis Actions"),
		FText::FromString(TEXT("WOC Basis Actions")));
	ActionCat.SetSortOrder(0);

	TWeakObjectPtr<UWOCBasisAsset> WeakAssetForAdd(Asset);
	IDetailLayoutBuilder* BuilderPtr = &DetailBuilder;

	ActionCat.AddCustomRow(FText::GetEmpty())
		.WholeRowContent()
		[
			SNew(SBox)
			.Padding(FMargin(4.f, 6.f))
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "FlatButton.Success")
				.HAlign(HAlign_Center)
				.ContentPadding(FMargin(0.f, 4.f))
				.Text(FText::FromString(TEXT("＋  Add LOD from File")))
				.OnClicked_Lambda([WeakAssetForAdd, BuilderPtr]() -> FReply
				{
					UWOCBasisAsset* A = WeakAssetForAdd.Get();
					if (A)
					{
						A->AddLODFromFile();
						if (BuilderPtr) { BuilderPtr->ForceRefreshDetails(); }
					}
					return FReply::Handled();
				})
			]
		];

	// --- Bottom section: LOD list ---
	IDetailCategoryBuilder& Cat = DetailBuilder.EditCategory(TEXT("WOC Basis"));
	Cat.SetSortOrder(10);

	if (Asset->LODs.Num() == 0)
	{
		Cat.AddCustomRow(FText::FromString(TEXT("No LODs")))
			.WholeRowContent()
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("No LODs. Use the 'Add LOD from File' button to add one.")))
			];
		return;
	}

	for (int32 i = 0; i < Asset->LODs.Num(); ++i)
	{
		const FWOCBasisLOD& LOD = Asset->LODs[i];
		const FString GroupName  = FString::Printf(TEXT("LOD%d"), i);
		const FString GroupLabel = FString::Printf(TEXT("LOD %d"), i);

		IDetailGroup& Group = Cat.AddGroup(FName(*GroupName), FText::FromString(GroupLabel), /*bForAdvanced=*/false, /*bStartExpanded=*/true);

		// Stats (read-only)
		auto AddStatRow = [&](const FString& Label, int32 Value)
		{
			Group.AddWidgetRow()
				.NameContent()
				[
					SNew(STextBlock)
					.Text(FText::FromString(Label))
				]
				.ValueContent()
				[
					SNew(STextBlock)
					.Text(FText::AsNumber(Value))
				];
		};

		AddStatRow(TEXT("Vertices"),         LOD.NumVertices);
		AddStatRow(TEXT("Joints"),            LOD.NumJoints);
		AddStatRow(TEXT("Coeffs (Max N)"),    LOD.NumCoeffs);
		AddStatRow(TEXT("MaxJointsPerVertex"), LOD.MaxJointsPerVertex);

		// --- Per-LOD Active N slider ---
		const int32 LODIndex = i;
		TWeakObjectPtr<UWOCBasisAsset> WeakAsset(Asset);
		const int32 SliderMax = FMath::Max(LOD.NumCoeffs, 1);

		Group.AddWidgetRow()
			.NameContent()
			[
				SNew(STextBlock).Text(FText::FromString(TEXT("Deformation Quality")))
			]
			.ValueContent()
			.MinDesiredWidth(280.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0.f, 2.f, 6.f, 2.f).VAlign(VAlign_Center)
				[
					SNew(SSlider)
					.MinValue(0.f)
					.MaxValue((float)SliderMax)
					.StepSize(1.f)
					.Value_Lambda([WeakAsset, LODIndex, SliderMax]() -> float
					{
						UWOCBasisAsset* A = WeakAsset.Get();
						if (!A || !A->LODs.IsValidIndex(LODIndex)) { return (float)SliderMax; }
						const int32 Val = A->LODs[LODIndex].ActiveN;
						return (float)(Val < 0 ? SliderMax : FMath::Clamp(Val, 0, SliderMax));
					})
					.OnValueChanged_Lambda([WeakAsset, LODIndex, SliderMax](float Val)
					{
						UWOCBasisAsset* A = WeakAsset.Get();
						if (!A || !A->LODs.IsValidIndex(LODIndex)) { return; }
						A->LODs[LODIndex].ActiveN = FMath::Clamp(FMath::RoundToInt(Val), 0, SliderMax);
					})
					.OnMouseCaptureEnd_Lambda([WeakAsset, LODIndex, SliderMax]()
					{
						UWOCBasisAsset* A = WeakAsset.Get();
						if (!A || !A->LODs.IsValidIndex(LODIndex)) { return; }
						A->LODs[LODIndex].ActiveN = FMath::Clamp(A->LODs[LODIndex].ActiveN, 0, SliderMax);
						A->MarkPackageDirty();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SNumericEntryBox<int32>)
					.AllowSpin(false)
					.MinValue(TOptional<int32>(-1))
					.MaxValue(TOptional<int32>(SliderMax))
					.Value_Lambda([WeakAsset, LODIndex]() -> TOptional<int32>
					{
						UWOCBasisAsset* A = WeakAsset.Get();
						if (!A || !A->LODs.IsValidIndex(LODIndex)) { return -1; }
						return A->LODs[LODIndex].ActiveN;
					})
					.OnValueCommitted_Lambda([WeakAsset, LODIndex, SliderMax](int32 Val, ETextCommit::Type)
					{
						UWOCBasisAsset* A = WeakAsset.Get();
						if (!A || !A->LODs.IsValidIndex(LODIndex)) { return; }
						A->LODs[LODIndex].ActiveN = (Val < 0) ? -1 : FMath::Clamp(Val, 0, SliderMax);
						A->MarkPackageDirty();
					})
					.MinDesiredValueWidth(40.f)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(4.f, 0.f, 0.f, 0.f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("Auto")))
					.ToolTipText(FText::FromString(TEXT("Set to -1 to use max N from binary")))
					.OnClicked_Lambda([WeakAsset, LODIndex]() -> FReply
					{
						UWOCBasisAsset* A = WeakAsset.Get();
						if (A && A->LODs.IsValidIndex(LODIndex))
						{
							A->LODs[LODIndex].ActiveN = -1;
							A->MarkPackageDirty();
						}
						return FReply::Handled();
					})
				]
			];

		// Reload button
		IDetailLayoutBuilder* BuilderPtrLocal = BuilderPtr;

		Group.AddWidgetRow()
			.WholeRowContent()
			[
				SNew(SBox)
				.Padding(FMargin(0.f, 4.f))
				[
					SNew(SButton)
					.HAlign(HAlign_Center)
					.Text(FText::FromString(TEXT("Reload from File...")))
					.OnClicked_Lambda([WeakAsset, LODIndex, BuilderPtrLocal]() -> FReply
					{
						UWOCBasisAsset* A = WeakAsset.Get();
						if (!A) { return FReply::Handled(); }

						IDesktopPlatform* DP = FDesktopPlatformModule::Get();
						if (!DP) { return FReply::Handled(); }

						void* ParentHandle = nullptr;
						if (FSlateApplication::IsInitialized())
						{
							TSharedPtr<SWindow> Win = FSlateApplication::Get().GetActiveTopLevelWindow();
							if (Win.IsValid() && Win->GetNativeWindow().IsValid())
							{
								ParentHandle = Win->GetNativeWindow()->GetOSWindowHandle();
							}
						}

						const FString DefaultPath =
							A->LODs.IsValidIndex(LODIndex) && !A->LODs[LODIndex].BinaryFilePath.IsEmpty()
							? FPaths::GetPath(A->LODs[LODIndex].BinaryFilePath)
							: FPaths::ProjectDir();

						TArray<FString> OutFiles;
						const bool bPicked = DP->OpenFileDialog(
							ParentHandle,
							FString::Printf(TEXT("Select WOC Basis binary for LOD %d"), LODIndex),
							DefaultPath,
							TEXT(""),
							TEXT("WOC Basis Binary (*.bin)|*.bin|All Files (*.*)|*.*"),
							EFileDialogFlags::None,
							OutFiles);

						if (bPicked && OutFiles.Num() > 0)
						{
							A->LoadLODFromBinaryFile(LODIndex, OutFiles[0]);
							if (BuilderPtrLocal) { BuilderPtrLocal->ForceRefreshDetails(); }
						}

						return FReply::Handled();
					})
				]
			];
	}
}
