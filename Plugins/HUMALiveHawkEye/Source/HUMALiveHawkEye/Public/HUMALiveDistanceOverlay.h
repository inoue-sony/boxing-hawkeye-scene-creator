#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "HUMALiveDistanceOverlay.generated.h"

class UProceduralMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UHUMALiveHawkEyeComponent;

/**
 * 2 人の選手の重心（Hawk-Eye の centerOfMass）を結ぶ円と矢印を、リングの床に描く。
 * 円は 2 人の重心を直径の両端とする（重心が円周に乗る）。自由視点コンテンツ案 3「間合いを消す踏み込み」用。
 */
UCLASS(Blueprintable)
class HUMALIVEHAWKEYE_API AHUMALiveDistanceOverlay : public AActor
{
	GENERATED_BODY()
public:
	AHUMALiveDistanceOverlay();

	/** 表示する。Sequencer でキーを打てる。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, Category = "HUMA|Overlay")
	bool bShowOverlay = true;

	/** 体型データセットの trackId。A = プログレイス（背番号 2）、B = ベン（背番号 1）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Overlay")
	int32 TrackA = 5495;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Overlay")
	int32 TrackB = 5519;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Overlay")
	FLinearColor ColorA = FLinearColor(0.10f, 0.62f, 0.38f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Overlay")
	FLinearColor ColorB = FLinearColor(0.03f, 0.40f, 0.80f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Overlay")
	FLinearColor RingColor = FLinearColor(1.0f, 0.42f, 0.05f);

	/** 円の内側の塗りの濃さ。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, Category = "HUMA|Overlay", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FillOpacity = 0.22f;

	/** 色の明るさ（Unlit の発光の倍率）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, Category = "HUMA|Overlay", meta = (ClampMin = "0.0"))
	float Brightness = 1.5f;

	/** 床の高さ。bUseActorZ がオンなら、このアクターの Z を床として使う。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Overlay")
	bool bUseActorZ = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Overlay", meta = (EditCondition = "!bUseActorZ"))
	float FloorZ = 0.f;

	/** 床からの浮かせ量（ちらつき防止）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Overlay")
	float LiftCm = 1.5f;

	/** 重心の位置に足す補正（データとレベルの位置がずれているとき用）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Overlay")
	FVector ComOffset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Overlay|Shape")
	float OutlineWidthCm = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Overlay|Shape")
	float ArrowWidthCm = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Overlay|Shape")
	float ArrowHeadLengthCm = 16.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Overlay|Shape")
	float ArrowHeadWidthCm = 14.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Overlay|Shape")
	float DotRadiusCm = 7.0f;

	/** 重心の点から重心の高さまで、上に伸びる点線を描く（選手の体に入った部分は隠れる）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, Category = "HUMA|Overlay|Shape")
	bool bShowVerticalLines = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Overlay|Shape")
	float VerticalLineWidthCm = 2.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Overlay|Shape")
	float DashLengthCm = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Overlay|Shape")
	float DashGapCm = 4.0f;

	/** Unlit・Translucent で、ベクトル "Color" と スカラー "Opacity" を持つマテリアル。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Overlay")
	TObjectPtr<UMaterialInterface> OverlayMaterial;

	/** 今の重心間の水平距離（cm）。 */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category = "HUMA|Overlay")
	float DistanceCm = 0.f;

	/** 今の表示時刻（再生バー上の秒）。 */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category = "HUMA|Overlay")
	float DisplayTimeSeconds = 0.f;

	/** 動作確認用: 距離と時刻を毎フレームログに出す。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HUMA|Overlay")
	bool bLogDistance = false;

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	UPROPERTY(VisibleAnywhere, Category = "HUMA|Overlay")
	TObjectPtr<UProceduralMeshComponent> Mesh;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;

	TWeakObjectPtr<UHUMALiveHawkEyeComponent> Live;

	void EnsureMaterials();
	void Rebuild(const FVector& A, const FVector& B, float Z);
	FVector ViewLocation() const;
};
