// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "RetakeTraversalObstacleComponent.generated.h"
class UPrimitiveComponent;
class USplineComponent;

/** Who may use a ledge. SDK contract (MKD-029): cooked maps store these numbers. Append only; never reorder or renumber. */
UENUM(BlueprintType)
enum class ERetakeLedgeAccess : uint8
{
    /** Players vault over or climb onto it on their own. */
    SoloOnly = 0,
    /** Two players boost up it; nobody climbs it alone. */
    BoostOnly = 1,
    Either = 2,
    /** Not a ledge: the spline is ignored. */
    Disabled = 3
};

USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeAuthoredLedgeQuery
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly,Category="Retake|Traversal") FName Ledge;
    UPROPERTY(BlueprintReadOnly,Category="Retake|Traversal") float Length=0.f;
    UPROPERTY(BlueprintReadOnly,Category="Retake|Traversal") float Along=0.f;
    UPROPERTY(BlueprintReadOnly,Category="Retake|Traversal") FVector FrontLocation=FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly,Category="Retake|Traversal") FVector FrontNormal=FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly,Category="Retake|Traversal") bool bHasBack=false;
    UPROPERTY(BlueprintReadOnly,Category="Retake|Traversal") FVector BackLocation=FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly,Category="Retake|Traversal") FVector BackNormal=FVector::ZeroVector;
};

USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeLedgeAccessSettings
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Retake|Traversal") ERetakeLedgeAccess Access=ERetakeLedgeAccess::SoloOnly;
    /** Built-in boost profile of a BoostOnly or Either ledge (RetakeMapSDKIds::BoostProfiles). */
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Retake|Traversal",meta=(GetOptions="RetakeMapSDK.RetakeMapSDKIdOptions.GetBoostProfileIds")) FName BoostProfileId;
};

/**
 * Makes its owner a traversal obstacle (RetakeMapSDK element, MKD-029). Every spline component on the owner is a ledge: the
 * spline runs along the top edge and its up vector points out of the face, away from the obstacle. LedgeAccess sets who may
 * use each ledge by spline name (none: SoloOnly), and OppositeLedges names the ledge across the obstacle that a vault lands
 * past. Works the same on a Blueprint and on a placed native actor with added components; it never creates or moves geometry.
 * The game finds obstacles by collision (the Traversable channel for solo ledges) and keeps the rules; Mover state carries only
 * the owner actor and the ledge name. At most one per actor.
 * Frozen per content generation: UPROPERTY names, order, types and defaults. Retired names never to be reused with another type:
 * Ledges, Opposites, bDrawBoostDebug, BoostDebugDuration, bBoostGeometryValid, BoostDebugReport, BoostProfile.
 */
UCLASS(ClassGroup=(Retake),meta=(BlueprintSpawnableComponent))
class RETAKEMAPSDK_API URetakeTraversalObstacleComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    URetakeTraversalObstacleComponent();
#if WITH_EDITOR
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
    UPROPERTY(EditAnywhere,BlueprintReadWrite,Category="Retake|Traversal") bool bTraversalEnabled=true;
    /** Shortest usable ledge. A query near either end is pulled half of this inwards. */
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Retake|Traversal",meta=(ClampMin="1",Units="cm")) float MinimumWidth=60.f;
    /** Access per ledge, keyed by spline name. A ledge without an entry is SoloOnly. */
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Retake|Traversal") TMap<FName,FRetakeLedgeAccessSettings> LedgeAccess;
    /** The ledge across the obstacle from each ledge, both by spline name: a vault from the key lands past the value. */
    UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="Retake|Traversal") TMap<FName,FName> OppositeLedges;

    /** The enabled ledge closest to ActorLocation, at the point nearest HitLocation, with its opposite ledge if any. */
    UFUNCTION(BlueprintCallable,Category="Retake|Traversal")
    bool QueryAuthoredLedges(FVector HitLocation,FVector ActorLocation,FRetakeAuthoredLedgeQuery& Result) const;
    /** Why this obstacle is misconfigured (ledges, opposites, boost IDs, up vectors, collision), or empty. */
    UFUNCTION(BlueprintPure,Category="Retake|Traversal") FText GetConfigurationError() const;
    /** The owner's ledges: every spline component except those whose access is Disabled. */
    void GetLedges(TArray<USplineComponent*>& OutLedges) const;
    /** The owner's static components that block pawns; false while the owner moves or simulates. */
    virtual bool GetBlockingComponents(TArray<UPrimitiveComponent*>& Result) const;
    /** Only for escaping an accepted traversal: owner collision remains identifiable after its ledges become unavailable. */
    virtual void GetRecoveryBlockingComponents(TArray<UPrimitiveComponent*>& Result) const;
    /** Enabled, at rest and with at least one ledge. */
    virtual bool IsUsable() const;
    bool AllowsSolo(FName Ledge) const;
    /** The boost profile ID of a BoostOnly or Either ledge; None otherwise or while traversal is disabled. */
    virtual FName GetBoostProfileId(FName Ledge) const;
    /** One-line diagnostics of the ledges for debug logs. */
    FString DescribeAuthoredLedges() const;

protected:
    /** Native runtime obstacles may read splines from their persistent map placement. */
    virtual const AActor* GetLedgeOwner() const;
    /** Native placements may provide runtime collision instead of saving a second blocking preview body. */
    virtual void GetCollisionForValidation(FBox& Body,bool& bBlocksPawn,bool& bBlocksTraversable) const;
    virtual void CollectIssues(TArray<FText>& Errors,TArray<FText>& Warnings) const;

private:
    bool IsLedge(const USplineComponent& Spline) const;
};
