#include "HUMARuntimeData.h"

#include "HUMAIdentity.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include <bcrypt.h>
#endif

namespace
{
	using FObject = TSharedPtr<FJsonObject>;
	using FValues = TArray<TSharedPtr<FJsonValue>>;

	class FReader
	{
	public:
		explicit FReader(FString& InError) : Error(InError) { Error.Reset(); }

		bool Check(bool Condition, const FString& Message)
		{
			if (!Condition) { Error = Message; }
			return Condition;
		}

		bool String(const FObject& Object, const TCHAR* Key, FString& Out)
		{
			return Check(Object->TryGetStringField(Key, Out) && !Out.IsEmpty(),
				FString::Printf(TEXT("Missing/invalid string: %s."), Key));
		}

		bool Number(const FObject& Object, const TCHAR* Key, double& Out)
		{
			return Check(Object->TryGetNumberField(Key, Out) && FMath::IsFinite(Out),
				FString::Printf(TEXT("Missing/non-finite number: %s."), Key));
		}

		bool Integer(const FObject& Object, const TCHAR* Key, int32& Out)
		{
			double Value;
			if (!Number(Object, Key, Value)) { return false; }
			if (!Check(Value >= MIN_int32 && Value <= MAX_int32 && FMath::FloorToDouble(Value) == Value,
				FString::Printf(TEXT("Invalid integer: %s."), Key))) { return false; }
			Out = static_cast<int32>(Value);
			return true;
		}

		bool Equals(const FObject& Object, const TCHAR* Key, const FString& Expected)
		{
			FString Value;
			return String(Object, Key, Value) && Check(Value == Expected,
				FString::Printf(TEXT("Incompatible %s: expected '%s', found '%s'."), Key, *Expected, *Value));
		}

		bool Equals(const FObject& Object, const TCHAR* Key, double Expected)
		{
			double Value;
			return Number(Object, Key, Value) && Check(FMath::Abs(Value - Expected) <= 1.e-10 * FMath::Max(1.0, FMath::Abs(Expected)),
				FString::Printf(TEXT("Inconsistent numeric field: %s."), Key));
		}

		bool Object(const FObject& Parent, const TCHAR* Key, FObject& Out)
		{
			const FObject* Value = nullptr;
			if (!Check(Parent->TryGetObjectField(Key, Value) && Value && Value->IsValid(),
				FString::Printf(TEXT("Missing object: %s."), Key))) { return false; }
			Out = *Value;
			return true;
		}

		bool Array(const FObject& Object, const TCHAR* Key, const FValues*& Out)
		{
			return Check(Object->TryGetArrayField(Key, Out) && Out,
				FString::Printf(TEXT("Missing array: %s."), Key));
		}

		bool ValueNumber(const TSharedPtr<FJsonValue>& Value, double& Out)
		{
			return Check(Value && Value->TryGetNumber(Out) && FMath::IsFinite(Out), TEXT("Array element must be a finite number."));
		}

		bool Vector(const TSharedPtr<FJsonValue>& Value, FVector& Out)
		{
			const FValues* Values = nullptr;
			if (!Check(Value && Value->TryGetArray(Values) && Values->Num() == 3, TEXT("Expected a three-number vector."))) { return false; }
			double X, Y, Z;
			if (!ValueNumber((*Values)[0], X) || !ValueNumber((*Values)[1], Y) || !ValueNumber((*Values)[2], Z)) { return false; }
			Out = FVector(X, Y, Z);
			return true;
		}

		bool Resolve(const FString& Root, const FString& Relative, FString& Out)
		{
			if (!Check(FPaths::IsRelative(Relative) && !Relative.Contains(TEXT(":")), TEXT("Dataset references must be relative paths."))) { return false; }
			Out = FPaths::ConvertRelativePathToFull(FPaths::Combine(Root, Relative));
			FPaths::NormalizeFilename(Out);
			return Check(FPaths::IsUnderDirectory(Out, Root), TEXT("Dataset reference escapes its directory."));
		}

		bool Load(const FString& Path, const TCHAR* Schema, FObject& Out)
		{
			FString Text;
			if (!Check(FFileHelper::LoadFileToString(Text, *Path), TEXT("Cannot read JSON: ") + Path)) { return false; }
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			if (!Check(FJsonSerializer::Deserialize(Reader, Out) && Out.IsValid(), TEXT("Invalid JSON: ") + Path)) { return false; }
			return Equals(Out, TEXT("schema"), FString(Schema)) && Equals(Out, TEXT("schema_version"), 1.0);
		}

		bool NullableId(const FObject& Object, const TCHAR* Key, FString& Out)
		{
			const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Key);
			if (!Check(Value && Value->IsValid(), FString::Printf(TEXT("Missing ID: %s."), Key))) { return false; }
			if ((*Value)->IsNull()) { Out = TEXT("unknown"); return true; }
			int32 Id;
			if (!Integer(Object, Key, Id)) { return false; }
			Out = FString::FromInt(Id);
			return true;
		}

	private:
		FString& Error;
	};

	bool ReadSkeleton(FReader& Reader, const FObject& Manifest, const FString& Root,
		const UHUMAIdentityData* Data, FHUMARuntimeDataset& Result, FString& SkeletonId, FString& BetaId)
	{
		FString Relative, Path;
		if (!Reader.String(Manifest, TEXT("skeleton_id"), SkeletonId) || !Reader.String(Manifest, TEXT("beta_layout_id"), BetaId)
			|| !Reader.String(Manifest, TEXT("skeleton"), Relative) || !Reader.Resolve(Root, Relative, Path)) { return false; }
		FObject Skeleton;
		if (!Reader.Load(Path, TEXT("huma.runtime.skeleton"), Skeleton)
			|| !Reader.Equals(Skeleton, TEXT("model_id"), Result.ModelId) || !Reader.Equals(Skeleton, TEXT("skeleton_id"), SkeletonId)
			|| !Reader.Equals(Skeleton, TEXT("hip_joint_index"), 0.0)) { return false; }
		FObject Coordinates, Rotation, ExtraRoot;
		if (!Reader.Object(Skeleton, TEXT("coordinates"), Coordinates) || !Reader.Object(Skeleton, TEXT("rotation"), Rotation)
			|| !Reader.Object(Skeleton, TEXT("extra_ue_root"), ExtraRoot)
			|| !Reader.Equals(Coordinates, TEXT("space"), FString(TEXT("unreal_component")))
			|| !Reader.Equals(Coordinates, TEXT("handedness"), FString(TEXT("left")))
			|| !Reader.Equals(Coordinates, TEXT("up_axis"), FString(TEXT("Z")))
			|| !Reader.Equals(Coordinates, TEXT("unit"), FString(TEXT("cm")))
			|| !Reader.Equals(Rotation, TEXT("type"), FString(TEXT("quaternion")))
			|| !Reader.Equals(Rotation, TEXT("component_order"), FString(TEXT("xyzw")))
			|| !Reader.Equals(Rotation, TEXT("space"), FString(TEXT("parent_local")))
			|| !Reader.Equals(Rotation, TEXT("meaning"), FString(TEXT("absolute local rotation, not an additive delta")))
			|| !Reader.Equals(ExtraRoot, TEXT("bone_name"), FString(TEXT("root")))) { return false; }
		bool bIncluded = true;
		if (!Reader.Check(ExtraRoot->TryGetBoolField(TEXT("included_in_payload"), bIncluded) && !bIncluded,
			TEXT("The extra UE root must not be part of the runtime payload."))) { return false; }
		const FValues* Names = nullptr;
		const FValues* Parents = nullptr;
		const FValues* Positions = nullptr;
		if (!Reader.Array(Skeleton, TEXT("joint_names"), Names) || !Reader.Array(Skeleton, TEXT("parent_indices"), Parents)
			|| !Reader.Array(Skeleton, TEXT("neutral_joint_positions_cm"), Positions)
			|| !Reader.Check(Names->Num() == Data->JointNames.Num() && Parents->Num() == Names->Num() && Positions->Num() == Names->Num(),
				TEXT("Skeleton joint dimensions do not match the identity data."))) { return false; }
		const FReferenceSkeleton& Ref = Data->ReferenceMesh->GetRefSkeleton();
		const int32 RootBone = Ref.FindBoneIndex(TEXT("root"));
		if (!Reader.Check(RootBone != INDEX_NONE && Ref.GetParentIndex(RootBone) == INDEX_NONE
			&& Ref.GetRefBonePose()[RootBone].GetTranslation().IsNearlyZero(0.0001)
			&& Ref.GetRefBonePose()[RootBone].GetScale3D().Equals(FVector::OneVector, 0.0001),
			TEXT("Runtime motion requires an imported root with zero translation and unit scale."))) { return false; }
		Result.MeshRootRotation = Ref.GetRefBonePose()[RootBone].GetRotation();
		TSet<FName> UniqueNames;
		for (int32 Joint = 0; Joint < Names->Num(); ++Joint)
		{
			FString Name;
			FVector Position;
			double ParentValue = 0;
			if (!Reader.Check((*Names)[Joint]->TryGetString(Name) && !Name.IsEmpty(), TEXT("Invalid skeleton joint name."))
				|| !Reader.ValueNumber((*Parents)[Joint], ParentValue) || !Reader.Vector((*Positions)[Joint], Position)) { return false; }
			const FName BoneName(*Name);
			const int32 Bone = Ref.FindBoneIndex(BoneName);
			const int32 IdentityJoint = Data->JointNames.Find(BoneName);
			const bool bParentValid = ParentValue == -1.0 ? Joint == 0 : (ParentValue >= 0 && ParentValue < Joint && FMath::FloorToDouble(ParentValue) == ParentValue);
			if (!Reader.Check(Bone != INDEX_NONE && IdentityJoint != INDEX_NONE && !UniqueNames.Contains(BoneName) && bParentValid,
				TEXT("Runtime skeleton has duplicate, missing, or invalid-parent joints: ") + Name)) { return false; }
			const int32 Parent = static_cast<int32>(ParentValue);
			const int32 ExpectedParent = Parent == INDEX_NONE ? RootBone : Result.JointToMeshBone[Parent];
			if (!Reader.Check(Ref.GetParentIndex(Bone) == ExpectedParent && Position.Equals(Data->JointTemplates[IdentityJoint], 0.01)
				&& Ref.GetRefBonePose()[Bone].GetRotation().Equals(FQuat::Identity, 0.0001)
				&& Ref.GetRefBonePose()[Bone].GetScale3D().Equals(FVector::OneVector, 0.0001),
				TEXT("Runtime skeleton/rest pose is incompatible with the imported mesh: ") + Name)) { return false; }
			UniqueNames.Add(BoneName);
			Result.JointToMeshBone.Add(Bone);
		}
		FObject Layout;
		if (!Reader.String(Manifest, TEXT("beta_layout"), Relative) || !Reader.Resolve(Root, Relative, Path)
			|| !Reader.Load(Path, TEXT("huma.runtime.beta_layout"), Layout)
			|| !Reader.Equals(Layout, TEXT("model_id"), Result.ModelId) || !Reader.Equals(Layout, TEXT("beta_layout_id"), BetaId)
			|| !Reader.Equals(Layout, TEXT("beta_count"), static_cast<double>(Data->BetaNames.Num()))
			|| !Reader.Array(Layout, TEXT("beta_names"), Names)
			|| !Reader.Check(Names->Num() == Data->BetaNames.Num(), TEXT("Identity beta count does not match the model."))) { return false; }
		for (int32 Beta = 0; Beta < Names->Num(); ++Beta)
		{
			FString Name;
			if (!Reader.Check((*Names)[Beta]->TryGetString(Name) && Name == Data->BetaNames[Beta].ToString(),
				TEXT("Identity beta layout/order does not match the model."))) { return false; }
		}
		return true;
	}

	bool ReadTrack(FReader& Reader, const FObject& Entry, const FString& Root, const UHUMAIdentityData* Data,
		const FString& ModelId, const FString& SkeletonId, const FString& BetaId, FHUMARuntimeTrack& Track)
	{
		FString Relative, IdentityPath, MotionPath;
		FString Role;
		if (!Reader.Integer(Entry, TEXT("track_id"), Track.TrackId) || !Reader.NullableId(Entry, TEXT("player_id"), Track.PlayerId)
			|| !Reader.NullableId(Entry, TEXT("role_id"), Role)
			|| !Reader.Integer(Entry, TEXT("team_id"), Track.TeamId) || !Reader.Integer(Entry, TEXT("jersey_number"), Track.JerseyNumber)
			|| !Reader.String(Entry, TEXT("identity"), Relative) || !Reader.Resolve(Root, Relative, IdentityPath)
			|| !Reader.String(Entry, TEXT("motion"), Relative) || !Reader.Resolve(Root, Relative, MotionPath)) { return false; }
		if (Role != TEXT("unknown")) Track.RoleId = FCString::Atoi(*Role);
		FObject Identity, Motion;
		if (!Reader.Load(IdentityPath, TEXT("huma.runtime.identity"), Identity) || !Reader.Load(MotionPath, TEXT("huma.runtime.motion"), Motion)
			|| !Reader.Equals(Identity, TEXT("model_id"), ModelId) || !Reader.Equals(Motion, TEXT("model_id"), ModelId)
			|| !Reader.Equals(Identity, TEXT("beta_layout_id"), BetaId) || !Reader.Equals(Motion, TEXT("skeleton_id"), SkeletonId)) { return false; }
		for (const FObject& Object : {Identity, Motion})
		{
			FObject Info;
			FString PlayerId;
			FString RoleId;
			if (!Reader.Object(Object, TEXT("track"), Info) || !Reader.NullableId(Info, TEXT("player_id"), PlayerId)
				|| !Reader.NullableId(Info, TEXT("role_id"), RoleId) || !Reader.Check(RoleId == Role, TEXT("Track role ID disagrees with the manifest."))
				|| !Reader.Check(PlayerId == Track.PlayerId, TEXT("Track/player IDs disagree with the manifest."))
				|| !Reader.Equals(Info, TEXT("track_id"), static_cast<double>(Track.TrackId))
				|| !Reader.Equals(Info, TEXT("team_id"), static_cast<double>(Track.TeamId))
				|| !Reader.Equals(Info, TEXT("jersey_number"), static_cast<double>(Track.JerseyNumber))) { return false; }
		}
		const FValues* Betas = nullptr;
		if (!Reader.Array(Identity, TEXT("betas"), Betas) || !Reader.Check(Betas->Num() == Data->BetaNames.Num(), TEXT("Invalid beta count."))) { return false; }
		for (const TSharedPtr<FJsonValue>& Value : *Betas)
		{
			double Beta;
			if (!Reader.ValueNumber(Value, Beta) || !Reader.Check(FMath::Abs(Beta) <= MAX_flt, TEXT("Beta exceeds float32 range."))) { return false; }
			Track.Betas.Add(static_cast<float>(Beta));
		}
		FObject Timing, Payload;
		if (!Reader.Integer(Motion, TEXT("joint_count"), Track.JointCount) || !Reader.Check(Track.JointCount == Data->JointNames.Num(), TEXT("Invalid motion joint count."))
			|| !Reader.Equals(Motion, TEXT("hip_translation_semantics"), FString(TEXT("offset added to beta-shaped hip rest position")))
			|| !Reader.Object(Motion, TEXT("timing"), Timing) || !Reader.Object(Motion, TEXT("payload"), Payload)
			|| !Reader.Number(Timing, TEXT("fps"), Track.FPS) || !Reader.Integer(Timing, TEXT("frame_count"), Track.FrameCount)
			|| !Reader.Check(Track.FPS > 0 && Track.FrameCount > 0, TEXT("Motion fps/frame count must be positive."))
			|| !Reader.Equals(Timing, TEXT("start_time_seconds"), 0.0)
			|| !Reader.Equals(Timing, TEXT("sample_interval_seconds"), 1.0 / Track.FPS)
			|| !Reader.Equals(Timing, TEXT("last_sample_time_seconds"), (Track.FrameCount - 1) / Track.FPS)
			|| !Reader.Equals(Timing, TEXT("end_time_seconds_exclusive"), Track.GetDuration())
			|| !Reader.Equals(Entry, TEXT("start_time_seconds"), 0.0)
			|| !Reader.Equals(Entry, TEXT("fps"), Track.FPS) || !Reader.Equals(Entry, TEXT("frame_count"), static_cast<double>(Track.FrameCount))
			|| !Reader.Equals(Entry, TEXT("end_time_seconds_exclusive"), Track.GetDuration())) { return false; }
		const FValues* Ranges = nullptr;
		if (!Reader.Array(Timing, TEXT("valid_frame_ranges"), Ranges) || !Reader.Check(!Ranges->IsEmpty(), TEXT("No valid motion frame ranges."))) { return false; }
		int32 PreviousEnd = 0;
		for (const TSharedPtr<FJsonValue>& Value : *Ranges)
		{
			const FValues* Pair = nullptr;
			double Start = 0, End = 0;
			if (!Reader.Check(Value->TryGetArray(Pair) && Pair->Num() == 2, TEXT("Invalid frame-range pair."))
				|| !Reader.ValueNumber((*Pair)[0], Start) || !Reader.ValueNumber((*Pair)[1], End)
				|| !Reader.Check(Start >= PreviousEnd && Start < End && End <= Track.FrameCount && FMath::FloorToDouble(Start) == Start && FMath::FloorToDouble(End) == End,
					TEXT("Invalid, overlapping, or out-of-bounds frame ranges."))) { return false; }
			Track.ValidRanges.Add(FIntPoint(static_cast<int32>(Start), static_cast<int32>(End)));
			PreviousEnd = static_cast<int32>(End);
		}
		const int64 Stride = (Track.JointCount * 4LL + 3) * 4;
		const int64 Length = Stride * Track.FrameCount;
		if (!Reader.Check(Length > 0 && Length <= MAX_int32, TEXT("Motion payload exceeds the supported resident-buffer size."))
			|| !Reader.Equals(Payload, TEXT("dtype"), FString(TEXT("float32"))) || !Reader.Equals(Payload, TEXT("byte_order"), FString(TEXT("little")))
			|| !Reader.Equals(Payload, TEXT("data_offset_bytes"), 0.0) || !Reader.Equals(Payload, TEXT("rotation_offset_bytes"), 0.0)
			|| !Reader.Equals(Payload, TEXT("hip_displacement_offset_bytes"), Track.JointCount * 16.0)
			|| !Reader.Equals(Payload, TEXT("frame_stride_bytes"), static_cast<double>(Stride))
			|| !Reader.Equals(Payload, TEXT("byte_length"), static_cast<double>(Length))
			|| !Reader.Equals(Entry, TEXT("payload_bytes"), static_cast<double>(Length))) { return false; }
		FString Filename, BinaryPath, ExpectedHash;
		if (!Reader.String(Payload, TEXT("file"), Filename)
			|| !Reader.Check(FPaths::GetCleanFilename(Filename) == Filename && !Filename.Contains(TEXT("\\")) && !Filename.Contains(TEXT("/")), TEXT("Motion payload must be a filename next to its header."))
			|| !Reader.Resolve(FPaths::GetPath(MotionPath), Filename, BinaryPath) || !Reader.String(Payload, TEXT("sha256"), ExpectedHash)) { return false; }
		TArray<uint8> Bytes;
		if (!Reader.Check(FFileHelper::LoadFileToArray(Bytes, *BinaryPath), TEXT("Cannot read motion payload: ") + BinaryPath)
			|| !Reader.Check(Bytes.Num() == Length, TEXT("Motion payload size mismatch: ") + BinaryPath)) { return false; }
#if PLATFORM_WINDOWS
		uint8 Digest[32];
		const NTSTATUS HashStatus = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, Bytes.GetData(), Bytes.Num(), Digest, sizeof(Digest));
		if (!Reader.Check(HashStatus >= 0, TEXT("Windows SHA256 calculation failed."))
			|| !Reader.Check(BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).Equals(ExpectedHash, ESearchCase::IgnoreCase),
				TEXT("Motion payload SHA256 mismatch: ") + BinaryPath)) { return false; }
#else
		return Reader.Check(false, TEXT("Runtime payload SHA256 verification currently supports Windows only."));
#endif
		auto ReadFloat = [&Bytes](int64 Offset)
		{
			uint32 Bits = static_cast<uint32>(Bytes[Offset]) | (static_cast<uint32>(Bytes[Offset + 1]) << 8)
				| (static_cast<uint32>(Bytes[Offset + 2]) << 16) | (static_cast<uint32>(Bytes[Offset + 3]) << 24);
			float Value;
			FMemory::Memcpy(&Value, &Bits, sizeof(float));
			return Value;
		};
		Track.Rotations.Reserve(Track.FrameCount * Track.JointCount);
		Track.HipDisplacements.Reserve(Track.FrameCount);
		for (int32 Frame = 0; Frame < Track.FrameCount; ++Frame)
		{
			int64 Offset = Frame * Stride;
			for (int32 Joint = 0; Joint < Track.JointCount; ++Joint, Offset += 16)
			{
				FQuat4f Rotation(ReadFloat(Offset), ReadFloat(Offset + 4), ReadFloat(Offset + 8), ReadFloat(Offset + 12));
				if (!Reader.Check(!Rotation.ContainsNaN() && FMath::Abs(Rotation.SizeSquared() - 1.0f) < 0.00002f,
					TEXT("Motion contains a non-finite or non-unit quaternion."))) { return false; }
				Track.Rotations.Add(Rotation.GetNormalized());
			}
			FVector3f Position(ReadFloat(Offset), ReadFloat(Offset + 4), ReadFloat(Offset + 8));
			if (!Reader.Check(!Position.ContainsNaN(), TEXT("Motion contains non-finite hip displacement."))) { return false; }
			Track.HipDisplacements.Add(Position);
		}
		return true;
	}
}

bool LoadHUMARuntimeDataset(const FString& ManifestPath, const UHUMAIdentityData* IdentityData,
	const FString& ExpectedModelId, FHUMARuntimeDataset& Output, FString& Error)
{
	FReader Reader(Error);
	if (!Reader.Check(IdentityData && IdentityData->ReferenceMesh, TEXT("Assign identity data with a reference mesh before loading motion."))) { return false; }
	if (!IdentityData->Validate(IdentityData->ReferenceMesh, Error)) { return false; }
	FObject Manifest;
	FHUMARuntimeDataset Result;
	if (!Reader.Load(ManifestPath, TEXT("huma.runtime.dataset"), Manifest) || !Reader.String(Manifest, TEXT("model_id"), Result.ModelId)
		|| !Reader.Check(!ExpectedModelId.IsEmpty() && Result.ModelId == ExpectedModelId, TEXT("Dataset model_id is incompatible with this playback controller."))) { return false; }
	FString Root = FPaths::ConvertRelativePathToFull(FPaths::GetPath(ManifestPath));
	FPaths::NormalizeDirectoryName(Root);
	FString SkeletonId, BetaId;
	if (!ReadSkeleton(Reader, Manifest, Root, IdentityData, Result, SkeletonId, BetaId)) { return false; }
	const FValues* Tracks = nullptr;
	int32 Count;
	if (!Reader.Array(Manifest, TEXT("tracks"), Tracks) || !Reader.Integer(Manifest, TEXT("track_count"), Count)
		|| !Reader.Check(Count > 0 && Count == Tracks->Num(), TEXT("Invalid dataset track count."))) { return false; }
	TSet<int32> TrackIds;
	for (const TSharedPtr<FJsonValue>& Value : *Tracks)
	{
		const FObject* Entry = nullptr;
		if (!Reader.Check(Value->TryGetObject(Entry) && Entry && Entry->IsValid(), TEXT("Invalid track manifest entry."))) { return false; }
		FHUMARuntimeTrack Track;
		if (!ReadTrack(Reader, *Entry, Root, IdentityData, Result.ModelId, SkeletonId, BetaId, Track)
			|| !Reader.Check(!TrackIds.Contains(Track.TrackId), TEXT("Duplicate dataset track_id."))) { return false; }
		TrackIds.Add(Track.TrackId);
		Result.Duration = FMath::Max(Result.Duration, Track.GetDuration());
		Result.Tracks.Add(MoveTemp(Track));
	}
	Output = MoveTemp(Result);
	return true;
}

bool FHUMARuntimeTrack::Sample(double Time, FHUMARuntimeSample& Output) const
{
	if (Time < 0 || Time > GetDuration() + 1.e-9) { return false; }
	const double Frame = FMath::Clamp(Time * FPS, 0.0, static_cast<double>(FrameCount - 1));
	const int32 First = FMath::FloorToInt(Frame);
	const FIntPoint* Range = ValidRanges.FindByPredicate([First](const FIntPoint& Item) { return Item.X <= First && First < Item.Y; });
	if (!Range) { return false; }
	const int32 Second = FMath::Min(First + 1, Range->Y - 1);
	const double Alpha = Frame - First;
	Output.LocalRotations.SetNumUninitialized(JointCount);
	for (int32 Joint = 0; Joint < JointCount; ++Joint)
	{
		Output.LocalRotations[Joint] = FQuat::Slerp(
			FQuat(Rotations[First * JointCount + Joint]), FQuat(Rotations[Second * JointCount + Joint]), Alpha).GetNormalized();
	}
	Output.HipDisplacement = FMath::Lerp(FVector(HipDisplacements[First]), FVector(HipDisplacements[Second]), Alpha);
	return true;
}
