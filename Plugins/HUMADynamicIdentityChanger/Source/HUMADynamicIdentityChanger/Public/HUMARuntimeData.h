#pragma once

#include "CoreMinimal.h"

class UHUMAIdentityData;

struct FHUMARuntimeSample
{
	TArray<FQuat> LocalRotations;
	FVector HipDisplacement = FVector::ZeroVector;
};

struct FHUMARuntimeTrack
{
	int32 TrackId = INDEX_NONE;
	FString PlayerId;
	int32 TeamId = INDEX_NONE;
	int32 JerseyNumber = INDEX_NONE;
	int32 RoleId = INDEX_NONE;
	TArray<float> Betas;
	double FPS = 0.0;
	int32 FrameCount = 0;
	int32 JointCount = 0;
	TArray<FIntPoint> ValidRanges;
	TArray<FQuat4f> Rotations;
	TArray<FVector3f> HipDisplacements;

	double GetDuration() const { return FrameCount / FPS; }
	bool Sample(double Time, FHUMARuntimeSample& Output) const;
};

struct FHUMARuntimeDataset
{
	FString ModelId;
	TArray<int32> JointToMeshBone;
	FQuat MeshRootRotation = FQuat::Identity;
	TArray<FHUMARuntimeTrack> Tracks;
	double Duration = 0.0;
};

HUMADYNAMICIDENTITYCHANGER_API bool LoadHUMARuntimeDataset(
	const FString& ManifestPath, const UHUMAIdentityData* IdentityData,
	const FString& ExpectedModelId, FHUMARuntimeDataset& Output, FString& Error);
