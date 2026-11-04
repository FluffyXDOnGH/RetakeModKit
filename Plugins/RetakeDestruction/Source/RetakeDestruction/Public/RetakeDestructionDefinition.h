// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "RetakeVisibilityGeometry.h"
#include "RetakeDestructionDefinition.generated.h"

class UGeometryCollection;
class UStaticMesh;
class UMaterialInterface;
class UPhysicalMaterial;
class UNiagaraSystem;
class USoundBase;

namespace RetakeDestructionPresentation
{
    inline constexpr uint8 Impact=1, BreakSound=2, BreakEffect=4, DebrisImpact=8;
}

USTRUCT()
struct FRetakeDestructionOccluder
{
    GENERATED_BODY()
    UPROPERTY() int32 TransformIndex=INDEX_NONE;
    UPROPERTY() FRetakeSightPolygon Polygon;
    /** Index into PlaneCovers when this face is part of an exactly tiled flat face; INDEX_NONE otherwise. */
    UPROPERTY() int32 Cover=INDEX_NONE;
};

USTRUCT()
struct FRetakeDestructionCoverCell
{
    GENERATED_BODY()
    /** Pieces whose face bounds overlap the cell. The cell is solid while none of them is released. */
    UPROPERTY() TArray<int32> Transforms;
};

/**
 * A flat face of the collection exactly tiled by its pieces' opaque faces (collection space). While none of Transforms
 * is released it replaces those faces with one convex polygon, so an intact finely fractured wall costs one test
 * instead of one per piece face (a 256-piece wall exceeded the coverage budget and hid nobody, 2026-09-28).
 */
USTRUCT()
struct FRetakeDestructionPlaneCover
{
    GENERATED_BODY()
    UPROPERTY() FRetakeSightPolygon Polygon;
    UPROPERTY() TArray<int32> Transforms;
    /**
     * A damaged face is judged per grid cell (row-major, Columns wide, over [GridMin, GridMax] in the U/V basis):
     * intact cells stay cover, so a hole opens one cell at most larger than itself instead of the whole face.
     */
    UPROPERTY() FVector U=FVector::ZeroVector;
    UPROPERTY() FVector V=FVector::ZeroVector;
    UPROPERTY() double Offset=0;
    UPROPERTY() FVector2D GridMin=FVector2D::ZeroVector;
    UPROPERTY() FVector2D GridMax=FVector2D::ZeroVector;
    UPROPERTY() int32 Columns=0;
    UPROPERTY() TArray<FRetakeDestructionCoverCell> Cells;
    /** Intact cells merged into row strips (collection space), appended to Out. */
    void AppendIntactStrips(TFunctionRef<bool(int32)> IsReleased,TArray<FRetakeSightPolygon>& Out) const;
};

/** Immutable, cookable destruction authoring. Runtime actors never modify the shared collection. */
UCLASS(BlueprintType)
class RETAKEDESTRUCTION_API URetakeDestructionDefinition : public UPrimaryDataAsset
{
    GENERATED_BODY()
public:
    virtual FPrimaryAssetId GetPrimaryAssetId() const override { return FPrimaryAssetId(TEXT("RetakeDestruction"),GetFName()); }
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Meshes") TObjectPtr<UGeometryCollection> Collection;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Meshes") TObjectPtr<UStaticMesh> FrameMesh;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Meshes") TArray<TObjectPtr<UMaterialInterface>> Materials;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Meshes") TArray<int32> BreakMaterialIndices;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Meshes") TArray<TObjectPtr<UMaterialInterface>> BreakMaterials;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction") TObjectPtr<UPhysicalMaterial> PhysicalMaterial;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction") TArray<float> DamageThresholds;
    /** Preserve the authored collection's per-piece removal, collision-driven fracture and field response.
     * Gameplay authority and nonblocking debris remain Retake policies. Disable for bounded cleanup tuning. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction") bool bUseAuthoredResponse=false;
    /** Authored point-hit wake branch: collections without kinematic pieces wake within 150 cm. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction", meta=(EditCondition="bUseAuthoredResponse")) bool bHasKinematicPieces=true;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction", meta=(EditCondition="bUseAuthoredResponse")) bool bStartAwake=true;
    /** Authored behavior: after its first contact above 0.5 cm/s the intact root stops blocking pawns (the reference IgnoreCharChaos switch).
     * Weapons, explosions and visibility still use it. Disable for props that must remain player-blocking cover. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction", meta=(EditCondition="bUseAuthoredResponse")) bool bRootYieldsToPawnsAfterContact=true;
    /** Authored behavior: weapon shots kick loose shards along their path with the per-hit field. Shards never stop the shot. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction", meta=(EditCondition="bUseAuthoredResponse")) bool bShotsKickDebris=true;
    /** Existing collection anchors are preserved. Additional boxes are in placement-local space. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction") TArray<FBox> AnchorBoxes;
    /** Editor preparation anchors fragments whose bounds touch the bottom band, and updates them after refracturing. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Anchors") bool bAnchorBase=false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Anchors", meta=(EditCondition="bAnchorBase",ClampMin=".1",Units="cm")) float BaseAnchorDepthCm=5;
    /** Also anchor fragments touching the two ends of the longest horizontal axis: a wall joined to walls or posts on both sides. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Anchors") bool bAnchorEnds=false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Anchors", meta=(EditCondition="bAnchorEnds",ClampMin=".1",Units="cm")) float EndAnchorDepthCm=5;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction", meta=(ClampMin="1")) float PointRadiusCm=60;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction", meta=(ClampMin="0")) float StrainPerDamage=62500;
    /** Spatial variation of fragment impulses. Does not vary the authoritative strain field. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction") FVector2D ImpulseNoiseRange=FVector2D(0,3);
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction", meta=(ClampMin="0")) float ImpulseSpeed=250;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction", meta=(ClampMin="0")) float RadialImpulseSpeed=200;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction", meta=(ClampMin="0")) float ExplosionImpulseSpeed=1500;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction", meta=(ClampMin="0")) float AngularImpulseSpeed=10;
    /**
     * Grenade contact damage needed to break this destructible and fly on through it instead of bouncing. 0: grenades always
     * bounce (and only chip it). Server and predicting clients evaluate the same rule from replicated state.
     * Blasts limited to fragile destructibles (smoke and flash activations) compare against it too.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Projectiles", meta=(ClampMin="0")) float GrenadeBreakDamage=0;
    /** Share of its speed a grenade keeps after passing through. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Projectiles", meta=(ClampMin="0",ClampMax="1")) float GrenadePassSpeedScale=.85f;
    /** Slower contacts (a grenade rolling into the pane) bounce instead. Measured along the surface normal. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Projectiles", meta=(ClampMin="0",Units="cm/s")) float GrenadePassMinimumSpeed=300;
    /** Radius of the guaranteed break a passing grenade opens around its contact point. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Projectiles", meta=(ClampMin="5",Units="cm")) float GrenadeBreakRadiusCm=35;
    /** Weapon hits on this destructible and its loose shards use the shared surface effects' destructible responses instead of the
     * weapon impact presentation. Set to make them silent (no effect, sound or decal). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Effects") bool bSilentSurfaceImpacts=false;
    /** Spawned through the shared effect budget, which tracks it until it ends: a mod's break effect must not loop forever. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Effects") TObjectPtr<UNiagaraSystem> BreakEffect;
    /** Level-wide Chaos-data systems (break/trailing data interface). One client instance per system per world, created with the first user.
     * They run for the whole level, outside the effect budget, so only built-in definitions may list them. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Effects") TArray<TObjectPtr<UNiagaraSystem>> WorldChaosEffects;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Effects") TObjectPtr<USoundBase> BreakSound;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Effects") TObjectPtr<USoundBase> ImpactSound;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Effects") TObjectPtr<USoundBase> CollisionSound;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Effects") FVector2D BreakPitch=FVector2D(1,1);
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Effects") FVector2D CollisionPitch=FVector2D(1,1);
    /** Frequency is events per second, independently of pitch. Reversed endpoints preserve authored random ranges. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Effects") FVector2D BreakSoundFrequencyHz=FVector2D(1,2);
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Effects") FVector2D CollisionSoundFrequencyHz=FVector2D(1,4);
    /** Collision audio is cosmetic and only enabled briefly after a fresh authoritative impact. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Effects", meta=(ClampMin="0",ClampMax="30")) float CollisionSoundWindowSeconds=10;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Effects", meta=(ClampMin=".2",ClampMax="10")) float CollisionSoundMaxDurationSeconds=3;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Effects", meta=(ClampMin="1")) float CollisionSoundMinimumImpactSpeed=200;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Effects", meta=(ClampMin="0")) float CollisionSoundVolume=1.8f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Effects", meta=(ClampMin="1",ClampMax="4")) int32 MaximumCollisionVoices=4;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Effects") FVector2D BreakEffectCooldownSeconds=FVector2D(.2,.3);
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Effects", meta=(ClampMin="0")) float MinimumEffectSpeed=300;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Effects", meta=(ClampMin="0")) float BreakSoundVolume=1.8f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Cleanup") FVector2D SleepSeconds=FVector2D(2,4);
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Cleanup") bool bRemoveDebrisOnSleep=true;
    /** Disable along with sleep removal for cosmetic rubble that remains until round reset. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Cleanup") bool bLimitDebrisLifetime=true;
    /** Force remaining active dynamic debris asleep after this long without a substantial collision. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Cleanup", meta=(ClampMin=".1")) float DebrisQuietSeconds=4;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Cleanup", meta=(ClampMin="1")) float DebrisSleepSpeed=200;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Cleanup", meta=(ClampMin=".1")) float FadeSeconds=1;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Cleanup", meta=(ClampMin="1")) float MaximumDebrisSeconds=10;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Network", meta=(ClampMin="1000")) float RelevanceDistanceCm=30000;
    /** Conservative proof budget: omitted fine surfaces can never conceal a player. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Performance", meta=(ClampMin="0",ClampMax="8192")) int32 MaximumOcclusionPolygons=4096;
    /** Editor-generated opaque exterior geometry; no render buffers are needed on a dedicated server. */
    UPROPERTY() TArray<FRetakeDestructionOccluder> Occluders;
    UPROPERTY() TArray<FRetakeDestructionPlaneCover> PlaneCovers;
    UPROPERTY() FGuid PreparedCollectionGuid;
    UPROPERTY() int32 PreparedTransformCount=0;
    UFUNCTION(BlueprintCallable, CallInEditor, Category="Retake|Destruction|Asset Repair") bool PrepareCollection();
    /**
     * Whether a grenade contact dealing ContactDamage, arriving at IntoSurfaceSpeed along the surface normal, breaks through
     * instead of bouncing. OutSpeedScale is the share of speed it keeps.
     */
    bool AllowsGrenadePass(float ContactDamage,float IntoSurfaceSpeed,float& OutSpeedScale) const;
    /** Whether a grenade dealing ContactDamage breaks this destructible: GrenadeBreakDamage is set and ContactDamage reaches it. */
    bool IsBrokenByGrenade(float ContactDamage) const;
    UFUNCTION(BlueprintPure, Category="Retake|Destruction|Diagnostics") FString GetPreparationSummary() const;
    UFUNCTION(BlueprintPure, Category="Retake|Destruction|Diagnostics") FString GetRemovalSummary() const;
    void FindOccluders(const FVector& Start,const FVector& End,FRetakeVisibilityCandidates& Out) const;
    void FindOccludersInBounds(const FBox& Bounds,FRetakeVisibilityCandidates& Out) const;
#if WITH_EDITOR
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
private:
    mutable TSharedPtr<FRetakeSightPolygonIndex> OccluderIndex;
    const FRetakeSightPolygonIndex& GetOccluderIndex() const;
};
