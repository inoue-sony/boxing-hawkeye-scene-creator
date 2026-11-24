#include "HUMALiveDistanceOverlay.h"

#include "HUMALiveHawkEyeComponent.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Kismet/GameplayStatics.h"

namespace
{
	enum ESection : int32 { Fill = 0, Outline, Arrow, DotA, DotB, LineA, LineB, Count };
	constexpr int32 CircleSegments = 96;

	void AddQuad(TArray<FVector>& V, TArray<int32>& T, const FVector& P0, const FVector& P1, const FVector& P2, const FVector& P3)
	{
		const int32 Base = V.Num();
		V.Append({P0, P1, P2, P3});
		// 上から見えるように両面に張る
		T.Append({Base, Base + 1, Base + 2, Base, Base + 2, Base + 3, Base, Base + 2, Base + 1, Base, Base + 3, Base + 2});
	}
	void AddTri(TArray<FVector>& V, TArray<int32>& T, const FVector& P0, const FVector& P1, const FVector& P2)
	{
		const int32 Base = V.Num();
		V.Append({P0, P1, P2});
		T.Append({Base, Base + 1, Base + 2, Base, Base + 2, Base + 1});
	}
	void Disc(TArray<FVector>& V, TArray<int32>& T, const FVector& C, float R)
	{
		for (int32 I = 0; I < CircleSegments; ++I)
		{
			const float A0 = 2.f * PI * I / CircleSegments, A1 = 2.f * PI * (I + 1) / CircleSegments;
			AddTri(V, T, C, C + FVector(FMath::Cos(A0) * R, FMath::Sin(A0) * R, 0), C + FVector(FMath::Cos(A1) * R, FMath::Sin(A1) * R, 0));
		}
	}
	void Annulus(TArray<FVector>& V, TArray<int32>& T, const FVector& C, float RIn, float ROut)
	{
		for (int32 I = 0; I < CircleSegments; ++I)
		{
			const float A0 = 2.f * PI * I / CircleSegments, A1 = 2.f * PI * (I + 1) / CircleSegments;
			const FVector D0(FMath::Cos(A0), FMath::Sin(A0), 0), D1(FMath::Cos(A1), FMath::Sin(A1), 0);
			AddQuad(V, T, C + D0 * RIn, C + D0 * ROut, C + D1 * ROut, C + D1 * RIn);
		}
	}
}

AHUMALiveDistanceOverlay::AHUMALiveDistanceOverlay()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;  // ライブの姿勢・時刻の更新（PrePhysics）の後
	Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Mesh"));
	SetRootComponent(Mesh);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);
	Mesh->bUseAsyncCooking = true;
}

void AHUMALiveDistanceOverlay::BeginPlay()
{
	Super::BeginPlay();
	EnsureMaterials();
	Mesh->SetAbsolute(true, true, true);  // 頂点はワールド座標で作る
	Mesh->SetWorldTransform(FTransform::Identity);
}

void AHUMALiveDistanceOverlay::EnsureMaterials()
{
	if (Materials.Num() == ESection::Count || !OverlayMaterial) { return; }
	Materials.Reset();
	for (int32 I = 0; I < ESection::Count; ++I)
	{
		Materials.Add(UMaterialInstanceDynamic::Create(OverlayMaterial, this));
	}
}

void AHUMALiveDistanceOverlay::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Live.IsValid())
	{
		for (TActorIterator<AActor> It(GetWorld()); It; ++It)
		{
			if (UHUMALiveHawkEyeComponent* Found = It->FindComponentByClass<UHUMALiveHawkEyeComponent>()) { Live = Found; break; }
		}
	}
	FVector A, B;
	const bool bHave = bShowOverlay && Live.IsValid() && Live->bRunning &&
		Live->GetTrackCenterOfMass(TrackA, A) && Live->GetTrackCenterOfMass(TrackB, B);
	if (!bHave)
	{
		Mesh->SetVisibility(false);
		return;
	}
	A += ComOffset;
	B += ComOffset;
	DistanceCm = static_cast<float>(FVector::Dist2D(A, B));
	DisplayTimeSeconds = static_cast<float>(Live->GetDisplayTimeSeconds());
	if (bLogDistance)
	{
		UE_LOG(LogHUMALive, Log, TEXT("Overlay: t=%.3f s distance=%.1f cm"), DisplayTimeSeconds, DistanceCm);
	}
	const float Z = (bUseActorZ ? static_cast<float>(GetActorLocation().Z) : FloorZ) + LiftCm;
	EnsureMaterials();
	Rebuild(A, B, Z);
	Mesh->SetVisibility(true);
}

FVector AHUMALiveDistanceOverlay::ViewLocation() const
{
	if (const APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(this, 0))
	{
		return Camera->GetCameraLocation();
	}
	return FVector(-1000.0, 0.0, 300.0);
}

void AHUMALiveDistanceOverlay::Rebuild(const FVector& InA, const FVector& InB, float Z)
{
	const FVector A(InA.X, InA.Y, Z), B(InB.X, InB.Y, Z);
	const FVector Center = (A + B) * 0.5f;
	const float Radius = static_cast<float>(FVector::Dist(A, B)) * 0.5f;
	const TArray<FVector2D> NoUV;
	const TArray<FLinearColor> NoColor;
	const TArray<FProcMeshTangent> NoTangent;

	auto Emit = [&](int32 Section, const TArray<FVector>& V, const TArray<int32>& T, const FLinearColor& Color, float Opacity, float Lift)
	{
		TArray<FVector> Lifted = V;
		for (FVector& P : Lifted) { P.Z += Lift; }  // 重なる面の前後を決める
		TArray<FVector> Normals;
		Normals.Init(FVector::UpVector, Lifted.Num());
		Mesh->CreateMeshSection_LinearColor(Section, Lifted, T, Normals, NoUV, NoColor, NoTangent, false);
		if (Materials.IsValidIndex(Section) && Materials[Section])
		{
			Materials[Section]->SetVectorParameterValue(TEXT("Color"), Color * Brightness);
			Materials[Section]->SetScalarParameterValue(TEXT("Opacity"), Opacity);
			Mesh->SetMaterial(Section, Materials[Section]);
		}
	};

	TArray<FVector> V;
	TArray<int32> T;
	// 塗り
	Disc(V, T, Center, Radius);
	Emit(ESection::Fill, V, T, FLinearColor(0.02f, 0.02f, 0.02f) + RingColor * 0.15f, FillOpacity, 0.0f);
	// 円周
	V.Reset(); T.Reset();
	Annulus(V, T, Center, FMath::Max(0.f, Radius - OutlineWidthCm * 0.5f), Radius + OutlineWidthCm * 0.5f);
	Emit(ESection::Outline, V, T, RingColor, 1.0f, 0.2f);
	// 両向きの矢印（重心の点の手前で止める）
	V.Reset(); T.Reset();
	const FVector Dir = (B - A).GetSafeNormal();
	const FVector Side = FVector::CrossProduct(FVector::UpVector, Dir) * 0.5f;
	const float Len = static_cast<float>(FVector::Dist(A, B));
	const float Inset = DotRadiusCm + 2.f;
	if (Len > 2.f * (Inset + ArrowHeadLengthCm))
	{
		const FVector TipA = A + Dir * Inset, TipB = B - Dir * Inset;
		const FVector BaseA = TipA + Dir * ArrowHeadLengthCm, BaseB = TipB - Dir * ArrowHeadLengthCm;
		AddQuad(V, T, BaseA - Side * ArrowWidthCm, BaseA + Side * ArrowWidthCm, BaseB + Side * ArrowWidthCm, BaseB - Side * ArrowWidthCm);
		AddTri(V, T, TipA, BaseA + Side * ArrowHeadWidthCm, BaseA - Side * ArrowHeadWidthCm);
		AddTri(V, T, TipB, BaseB - Side * ArrowHeadWidthCm, BaseB + Side * ArrowHeadWidthCm);
	}
	Emit(ESection::Arrow, V, T, RingColor, 1.0f, 0.4f);
	// 重心の点（選手の色）
	V.Reset(); T.Reset();
	Disc(V, T, A, DotRadiusCm);
	Emit(ESection::DotA, V, T, ColorA, 1.0f, 0.6f);
	V.Reset(); T.Reset();
	Disc(V, T, B, DotRadiusCm);
	Emit(ESection::DotB, V, T, ColorB, 1.0f, 0.6f);

	// 重心の点から重心の高さまでの点線。カメラの方を向けた縦長の帯を、間隔をあけて並べる
	const FVector Eye = ViewLocation();
	auto DashedLine = [&](int32 Section, const FVector& Foot, float TopZ, const FLinearColor& Color)
	{
		TArray<FVector> LV;
		TArray<int32> LT;
		if (bShowVerticalLines && TopZ > Foot.Z + DashLengthCm)
		{
			FVector ToEye = Eye - Foot;
			ToEye.Z = 0.0;
			const FVector Across = FVector::CrossProduct(FVector::UpVector, ToEye.GetSafeNormal()) * (VerticalLineWidthCm * 0.5f);
			for (float Z0 = Foot.Z + DotRadiusCm * 0.5f; Z0 < TopZ; Z0 += DashLengthCm + DashGapCm)
			{
				const float Z1 = FMath::Min(Z0 + DashLengthCm, TopZ);
				const FVector P0(Foot.X, Foot.Y, Z0), P1(Foot.X, Foot.Y, Z1);
				AddQuad(LV, LT, P0 - Across, P0 + Across, P1 + Across, P1 - Across);
			}
		}
		Emit(Section, LV, LT, Color, 1.0f, 0.0f);
	};
	DashedLine(ESection::LineA, A, static_cast<float>(InA.Z), ColorA);
	DashedLine(ESection::LineB, B, static_cast<float>(InB.Z), ColorB);
}
