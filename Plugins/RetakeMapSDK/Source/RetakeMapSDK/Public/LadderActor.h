// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LadderActor.generated.h"
class UStaticMeshComponent;
class UInstancedStaticMeshComponent;
class UBoxComponent;
class UArrowComponent;
class UStaticMesh;

/** SDK contract (MKD-029): cooked maps store these numbers. Append only; never reorder or renumber. */
UENUM(BlueprintType)
enum class ELadderGeometry : uint8 { Generated = 0, CompleteMesh = 1 };

/** Local +Z climbs, +Y spans the width, +X faces the climber. Distances are centimetres. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FLadderGeometrySettings
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Geometry") ELadderGeometry Type = ELadderGeometry::Generated;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Geometry", meta=(ClampMin="100", Units="cm")) float Height = 366;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Geometry", meta=(ClampMin="30", Units="cm")) float Width = 53.375f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Geometry", meta=(ClampMin="10", Units="cm")) float RungSpacing = 27.7f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Geometry", meta=(ClampMin="0", Units="cm")) float FirstRungOffset = 13.85f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Geometry", meta=(ClampMin="0", Units="cm")) float LastRungOffset = 10;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Geometry", meta=(ClampMin="0", Units="cm")) float RailExtensionBottom = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Geometry", meta=(ClampMin="0", Units="cm")) float RailExtensionTop = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Meshes") TObjectPtr<UStaticMesh> CompleteMesh;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Meshes") TObjectPtr<UStaticMesh> RungMesh;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Meshes") TObjectPtr<UStaticMesh> LeftRailMesh;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Meshes") TObjectPtr<UStaticMesh> RightRailMesh;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Meshes") FTransform CompleteMeshOffset = FTransform(FRotator(0,90,0));
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Meshes") FTransform RungMeshOffset = FTransform(FRotator(0,90,0));
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Meshes") FTransform LeftRailOffset = FTransform::Identity;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Meshes") FTransform RightRailOffset = FTransform::Identity;
    /** Complete mesh rung heights, measured above its lowest point at ReferenceHeight. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Grips") TArray<float> CompleteMeshRungs;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Grips", meta=(ClampMin="1")) float ReferenceHeight = 366;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Movement", meta=(ClampMin="35", Units="cm")) float StandOff = 42;
    /** Retake nominal cadence: 30.5 cm per step in 1/6 second. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Movement", meta=(DisplayName="Ascent Speed", ClampMin="10", Units="cm/s")) float ClimbSpeed = 183;
    /** Stock downward step duration is 0.15 seconds. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Movement", meta=(ClampMin="10", Units="cm/s")) float DescentSpeed = 203.333333f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Movement", meta=(ClampMin="10", Units="cm")) float GrabReach = 90;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Movement", meta=(ClampMin="5", Units="cm")) float EndInteractionHeight = 65;
    /** Offset from the bottom in the ladder's yaw frame; Z is world-up landing adjustment. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Movement") FVector BottomExitOffset = FVector(90,0,0);
    /** Offset from the top in the ladder's yaw frame; horizontal offsets never lower a tilted ladder's landing. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Movement") FVector TopExitOffset = FVector(-80,0,0);
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Ladders|Movement") float TopAttachBelowEnd = 170;
};

/**
 * Static climbable ladder (RetakeMapSDK element, MKD-029): geometry, grips, entry and exit points. The climbing character
 * supplies the movement and animation (MoverLadders ULadderClimberComponent); the ladder holds no animation data, so any
 * ladder that passes GetConfigurationError is climbable by the game's character. Registers with URetakeMapElementSubsystem.
 * Frozen per content generation: UPROPERTY names, order, types and defaults (Geometry is also the live and replay wire
 * layout) and the subobject names LadderRoot, CompleteMesh, Rungs, LeftRail, RightRail, InteractionVolume, BottomExit and
 * TopExit (visibility bakes hash component paths).
 */
UCLASS(Blueprintable)
class RETAKEMAPSDK_API ALadderActor : public AActor
{
    GENERATED_BODY()
public:
    ALadderActor();
    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
#if WITH_EDITOR
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
    UPROPERTY(EditAnywhere, BlueprintReadOnly, ReplicatedUsing=OnRep_Configuration, Category="Retake|Ladders") FLadderGeometrySettings Geometry;
#if WITH_EDITORONLY_DATA
    UPROPERTY(EditAnywhere, Category="Retake|Ladders|Preview") bool bShowPreview = true;
#endif
    /** GetConfigurationError() as of the last geometry rebuild, shown in the Details panel. */
    UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly, Category="Retake|Ladders|Validation") FText ConfigurationError;
    /** SDK contract (MKD-029): every gap between neighbouring rungs lies in this range, in cm. The game's climbing animation
     *  profile must cover it (MoverLadders ULadderAnimationProfile::GetEnvelopeError). Changing it is a contract change. */
    static constexpr float MinimumRungGap = 10.f;
    static constexpr float MaximumRungGap = 55.f;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Components") TObjectPtr<USceneComponent> LadderRoot;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Components") TObjectPtr<UStaticMeshComponent> CompleteMesh;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Components") TObjectPtr<UInstancedStaticMeshComponent> Rungs;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Components") TObjectPtr<UStaticMeshComponent> LeftRail;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Components") TObjectPtr<UStaticMeshComponent> RightRail;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Components") TObjectPtr<UBoxComponent> InteractionVolume;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Components") TObjectPtr<UArrowComponent> BottomAnchor;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Components") TObjectPtr<UArrowComponent> TopAnchor;
    UFUNCTION(BlueprintCallable, CallInEditor, Category="Retake|Ladders") void RebuildGeometry();
    UFUNCTION(BlueprintPure, Category="Retake|Ladders") FVector GetGripPosition(int32 Rung, float Side, FVector Offset) const;
    UFUNCTION(BlueprintPure, Category="Retake|Ladders") TArray<float> GetRungHeights() const;
    UFUNCTION(BlueprintPure, Category="Retake|Ladders") bool IsUsable() const;
    /** Empty when the ladder is climbable: speeds, dimensions, unit scale and incline, a static transform, and rung gaps within
     *  [MinimumRungGap, MaximumRungGap]. */
    UFUNCTION(BlueprintPure, Category="Retake|Ladders|Validation") FText GetConfigurationError() const;
    UFUNCTION(BlueprintPure, Category="Retake|Ladders") float GetClimbPhaseAtDistance(float Distance) const;
    FVector BodyPosition(float Distance, float CapsuleHalfHeight) const;
    float DistanceAtWorldHeight(float WorldZ, float CapsuleHalfHeight) const;
    float ProjectDistance(FVector Location, float CapsuleHalfHeight) const;
    FVector ExitPosition(bool bTop, float CapsuleHalfHeight) const;
    float StepSpacing() const;
    FRotator FacingRotation() const;
private:
    UFUNCTION() void OnRep_Configuration();
    FTransform InitialTransform;
};
