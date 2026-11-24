#include "HUMAIdentityEditorLibrary.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "HUMAIdentity.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

THIRD_PARTY_INCLUDES_START
#include "hdf5.h"
THIRD_PARTY_INCLUDES_END

bool IsHUMAIdentityH5Available();

namespace
{
	struct FH5Handle
	{
		hid_t Id;
		herr_t (*Close)(hid_t);
		FH5Handle(hid_t InId, herr_t (*InClose)(hid_t)) : Id(InId), Close(InClose) {}
		~FH5Handle() { if (Id >= 0) { Close(Id); } }
		FH5Handle(const FH5Handle&) = delete;
		FH5Handle& operator=(const FH5Handle&) = delete;
	};

	bool ReadNames(hid_t File, const char* Key, TArray<FString>& Values, FString& Error, bool RequireUnique)
	{
		FH5Handle Dataset(H5Dopen2(File, Key, H5P_DEFAULT), H5Dclose);
		if (Dataset.Id < 0) { Error = FString::Printf(TEXT("Missing H5 dataset: %s."), UTF8_TO_TCHAR(Key)); return false; }
		FH5Handle Space(H5Dget_space(Dataset.Id), H5Sclose);
		FH5Handle Type(H5Dget_type(Dataset.Id), H5Tclose);
		hsize_t Count = 0;
		if (Space.Id < 0 || Type.Id < 0 || H5Sget_simple_extent_ndims(Space.Id) != 1
			|| H5Sget_simple_extent_dims(Space.Id, &Count, nullptr) < 0
			|| Count == 0 || Count > 65536 || H5Tget_class(Type.Id) != H5T_STRING)
		{
			Error = FString::Printf(TEXT("Invalid H5 name array: %s."), UTF8_TO_TCHAR(Key)); return false;
		}
		Values.Reset(static_cast<int32>(Count));
		if (H5Tis_variable_str(Type.Id) > 0)
		{
			TArray<char*> Strings;
			Strings.SetNumZeroed(static_cast<int32>(Count));
			const bool Read = H5Dread(Dataset.Id, Type.Id, H5S_ALL, H5S_ALL, H5P_DEFAULT, Strings.GetData()) >= 0;
			if (Read)
			{
				for (const char* String : Strings) { Values.Add(String ? UTF8_TO_TCHAR(String) : TEXT("")); }
			}
			const bool Reclaimed = H5Dvlen_reclaim(Type.Id, Space.Id, H5P_DEFAULT, Strings.GetData()) >= 0;
			if (!Read || !Reclaimed) { Error = TEXT("Could not read/release H5 variable-length strings."); return false; }
		}
		else
		{
			const size_t Width = H5Tget_size(Type.Id);
			if (Width == 0 || Width > 4096 || Width * Count > 16 * 1024 * 1024)
			{
				Error = TEXT("H5 fixed-length names exceed supported bounds."); return false;
			}
			TArray<char> Bytes;
			Bytes.SetNumZeroed(static_cast<int32>(Width * Count));
			if (H5Dread(Dataset.Id, Type.Id, H5S_ALL, H5S_ALL, H5P_DEFAULT, Bytes.GetData()) < 0)
			{
				Error = TEXT("Could not read H5 fixed-length strings."); return false;
			}
			for (int32 Index = 0; Index < static_cast<int32>(Count); ++Index)
			{
				const char* Start = Bytes.GetData() + Index * Width;
				int32 Length = 0;
				while (Length < static_cast<int32>(Width) && Start[Length] != '\0') { ++Length; }
				FUTF8ToTCHAR Converted(Start, Length);
				Values.Add(FString(Converted.Length(), Converted.Get()).TrimEnd());
			}
		}
		TSet<FName> Unique;
		for (const FString& Value : Values)
		{
			const FName Name(*Value);
			if (Value.IsEmpty() || Value != Value.TrimStartAndEnd()
				|| (RequireUnique && (Name.IsNone() || Unique.Contains(Name))))
			{
				Error = TEXT("H5 contains empty, whitespace-padded or duplicate Unreal names."); return false;
			}
			Unique.Add(Name);
		}
		return true;
	}

	bool ReadNumbers(hid_t File, const char* Key, const TArray<hsize_t>& Dimensions, TArray<double>& Values, FString& Error)
	{
		FH5Handle Dataset(H5Dopen2(File, Key, H5P_DEFAULT), H5Dclose);
		if (Dataset.Id < 0) { Error = FString::Printf(TEXT("Missing H5 dataset: %s."), UTF8_TO_TCHAR(Key)); return false; }
		FH5Handle Space(H5Dget_space(Dataset.Id), H5Sclose);
		hsize_t Actual[3] = {};
		if (Space.Id < 0 || H5Sget_simple_extent_ndims(Space.Id) != Dimensions.Num()
			|| H5Sget_simple_extent_dims(Space.Id, Actual, nullptr) < 0)
		{
			Error = FString::Printf(TEXT("Invalid rank for H5 dataset: %s."), UTF8_TO_TCHAR(Key)); return false;
		}
		int64 Count = 1;
		for (int32 Axis = 0; Axis < Dimensions.Num(); ++Axis)
		{
			if (Actual[Axis] != Dimensions[Axis] || Actual[Axis] == 0
				|| Actual[Axis] > static_cast<hsize_t>(16 * 1024 * 1024 / Count))
			{
				Error = FString::Printf(TEXT("Invalid/oversized dimensions for H5 dataset: %s."), UTF8_TO_TCHAR(Key)); return false;
			}
			Count *= static_cast<int64>(Actual[Axis]);
		}
		Values.SetNumUninitialized(static_cast<int32>(Count));
		FH5Handle StoredType(H5Dget_type(Dataset.Id), H5Tclose);
		FH5Handle NativeType(StoredType.Id >= 0 ? H5Tget_native_type(StoredType.Id, H5T_DIR_ASCEND) : -1, H5Tclose);
		if (NativeType.Id < 0 || H5Tget_class(NativeType.Id) != H5T_FLOAT)
		{
			Error = TEXT("H5 regressor datasets must contain float32 or float64 numbers."); return false;
		}
		herr_t Read = -1;
		if (H5Tget_size(NativeType.Id) == sizeof(double))
		{
			Read = H5Dread(Dataset.Id, NativeType.Id, H5S_ALL, H5S_ALL, H5P_DEFAULT, Values.GetData());
		}
		else if (H5Tget_size(NativeType.Id) == sizeof(float))
		{
			TArray<float> Floats;
			Floats.SetNumUninitialized(static_cast<int32>(Count));
			Read = H5Dread(Dataset.Id, NativeType.Id, H5S_ALL, H5S_ALL, H5P_DEFAULT, Floats.GetData());
			if (Read >= 0)
			{
				for (int32 Index = 0; Index < Floats.Num(); ++Index) { Values[Index] = Floats[Index]; }
			}
		}
		if (Read < 0)
		{
			Error = FString::Printf(TEXT("Could not read numeric H5 dataset: %s."), UTF8_TO_TCHAR(Key)); return false;
		}
		for (double Value : Values)
		{
			if (!FMath::IsFinite(Value)) { Error = TEXT("H5 regressor contains non-finite numbers."); return false; }
		}
		return true;
	}

	struct FCoordinateMap
	{
		int32 Axes[3] = {0, 1, 2};
		FVector Signs = FVector::OneVector;
		double Scale = 1;
		FVector Transform(const FVector& Point) const
		{
			return FVector(Point[Axes[0]], Point[Axes[1]], Point[Axes[2]]) * Signs * Scale;
		}
	};

	UHUMAIdentityData* MatchMesh(USkeletalMesh* Mesh, const TArray<FString>& Identities,
		const TArray<FString>& Joints, const TArray<double>& Template, const TArray<double>& Coefficients, FString& Error)
	{
		TArray<FName> BetaNames;
		for (int32 Beta = 0; Beta < Identities.Num(); ++Beta)
		{
			BetaNames.Add(*FString::Printf(TEXT("%04d_%s_shapedirs_BS"), Beta, *Identities[Beta]));
			if (!Mesh->FindMorphTarget(BetaNames.Last()))
			{
				Error = FString::Printf(TEXT("Mesh %s is missing identity morph %s."), *Mesh->GetPathName(), *BetaNames.Last().ToString());
				return nullptr;
			}
		}
		const TArray<FVector> Reference = UHUMAIdentityComponent::GetReferenceBonePositions(Mesh);
		TArray<FVector> Targets;
		for (const FString& Joint : Joints)
		{
			const int32 Bone = Mesh->GetRefSkeleton().FindBoneIndex(*Joint);
			if (!Reference.IsValidIndex(Bone)) { Error = TEXT("Selected mesh is missing H5 regressor joints."); return nullptr; }
			Targets.Add(Reference[Bone]);
		}
		const int32 Permutations[6][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
		FCoordinateMap Best;
		double BestError = TNumericLimits<double>::Max();
		for (const auto& Axes : Permutations)
		{
			for (int32 Mask = 0; Mask < 8; ++Mask)
			{
				for (double Scale : {0.01, 1.0, 100.0})
				{
					FCoordinateMap Map;
					for (int32 Axis = 0; Axis < 3; ++Axis)
					{
						Map.Axes[Axis] = Axes[Axis];
						Map.Signs[Axis] = (Mask & (1 << Axis)) ? 1.0 : -1.0;
					}
					Map.Scale = Scale;
					FVector Offset = FVector::ZeroVector;
					for (int32 Joint = 0; Joint < Joints.Num(); ++Joint)
					{
						Offset += Targets[Joint] - Map.Transform(FVector(Template[Joint*3], Template[Joint*3+1], Template[Joint*3+2]));
					}
					Offset /= Joints.Num();
					double Maximum = 0;
					for (int32 Joint = 0; Joint < Joints.Num(); ++Joint)
					{
						Maximum = FMath::Max(Maximum, FVector::Dist(Targets[Joint],
							Map.Transform(FVector(Template[Joint*3], Template[Joint*3+1], Template[Joint*3+2])) + Offset));
					}
					if (Maximum < BestError) { BestError = Maximum; Best = Map; }
				}
			}
		}
		if (BestError > 0.01)
		{
			Error = FString::Printf(TEXT("H5/mesh neutral joints mismatch by %.9g cm (limit 0.01 cm). Use the matching imported mesh."), BestError);
			return nullptr;
		}
		UHUMAIdentityData* Data = NewObject<UHUMAIdentityData>();
		Data->ReferenceMesh = Mesh;
		Data->BetaNames = MoveTemp(BetaNames);
		Data->JointTemplates = MoveTemp(Targets);
		for (const FString& Joint : Joints) { Data->JointNames.Add(*Joint); }
		Data->JointCoefficients.Reserve(Joints.Num() * Identities.Num());
		for (int32 Joint = 0; Joint < Joints.Num(); ++Joint)
		{
			for (int32 Beta = 0; Beta < Identities.Num(); ++Beta)
			{
				const FVector Delta(Coefficients[(Joint*3)*Identities.Num()+Beta],
					Coefficients[(Joint*3+1)*Identities.Num()+Beta], Coefficients[(Joint*3+2)*Identities.Num()+Beta]);
				Data->JointCoefficients.Add(Best.Transform(Delta));
			}
		}
		if (!Data->Validate(Mesh, Error)) { return nullptr; }
		Data->SourceDescription = FString::Printf(TEXT("HDF5 regressor; neutral fit max %.9g cm; scale %.9g; axes %d,%d,%d; signs %.0f,%.0f,%.0f"),
			BestError, Best.Scale, Best.Axes[0], Best.Axes[1], Best.Axes[2], Best.Signs.X, Best.Signs.Y, Best.Signs.Z);
		return Data;
	}
}

UHUMAIdentityData* UHUMAIdentityEditorLibrary::ReadIdentityDataH5(const FString& Filename, USkeletalMesh* ReferenceMesh, FString& OutError)
{
	OutError.Reset();
	auto Fail = [&OutError](const FString& Message) -> UHUMAIdentityData*
	{
		OutError = Message;
		UE_LOG(LogHUMAIdentity, Warning, TEXT("H5 identity import failed: %s"), *Message);
		return nullptr;
	};
	if (!IsHUMAIdentityH5Available()) { return Fail(TEXT("HDF5 editor library is unavailable; restore the plugin ThirdParty/HDF5 files.")); }
	if (!FPaths::FileExists(Filename)) { return Fail(TEXT("H5 model file does not exist: ") + Filename); }
	if (H5Fis_hdf5(TCHAR_TO_UTF8(*Filename)) != 1) { return Fail(TEXT("The selected file is not a readable HDF5 model.")); }
	FH5Handle File(H5Fopen(TCHAR_TO_UTF8(*Filename), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose);
	if (File.Id < 0) { return Fail(TEXT("Could not open H5 model read-only.")); }
	TArray<FString> Identities, Joints;
	TArray<double> Template, Coefficients;
	if (!ReadNames(File.Id, "identity_name_list", Identities, OutError, false)
		|| !ReadNames(File.Id, "joint_name_list", Joints, OutError, true)
		|| !ReadNumbers(File.Id, "j_template", {static_cast<hsize_t>(Joints.Num()), 3}, Template, OutError)
		|| !ReadNumbers(File.Id, "j_offset_regressor", {static_cast<hsize_t>(Joints.Num()), 3, static_cast<hsize_t>(Identities.Num())}, Coefficients, OutError))
	{
		return Fail(OutError);
	}
	UHUMAIdentityData* Data = nullptr;
	TStrongObjectPtr<UHUMAIdentityData> AutomaticMatch;
	if (ReferenceMesh)
	{
		Data = MatchMesh(ReferenceMesh, Identities, Joints, Template, Coefficients, OutError);
		if (!Data) { return Fail(OutError); }
	}
	else
	{
		TArray<FAssetData> Assets;
		FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		Registry.Get().SearchAllAssets(true);
		Registry.Get().GetAssetsByClass(USkeletalMesh::StaticClass()->GetClassPathName(), Assets);
		TSet<USkeletalMesh*> Visited;
		for (const FAssetData& Asset : Assets)
		{
			if (!Asset.PackageName.ToString().StartsWith(TEXT("/Game/"))) { continue; }
			USkeletalMesh* Candidate = Cast<USkeletalMesh>(Asset.GetAsset());
			if (!Candidate || Visited.Contains(Candidate)) { continue; }
			Visited.Add(Candidate);
			FString CandidateError;
			UHUMAIdentityData* Matched = MatchMesh(Candidate, Identities, Joints, Template, Coefficients, CandidateError);
			if (!Matched) { continue; }
			if (Data)
			{
				return Fail(FString::Printf(TEXT("More than one imported mesh matches H5: %s and %s. Select the intended Existing Skeletal Mesh explicitly."),
					*Data->ReferenceMesh->GetPathName(), *Candidate->GetPathName()));
			}
			AutomaticMatch.Reset(Matched);
			Data = AutomaticMatch.Get();
		}
		if (!Data) { return Fail(TEXT("No existing /Game Skeletal Mesh matches the H5 neutral joints and identity morphs. Import/select the matching body mesh first.")); }
	}
	Data->SourceDescription = FPaths::GetCleanFilename(Filename) + TEXT("; ") + Data->SourceDescription;
	return Data;
}
