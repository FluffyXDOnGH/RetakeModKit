// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RetakeDetonationTarget.generated.h"

class UStaticMeshComponent;

/** SDK contract (MKD-029): cooked maps store these numbers. Append only; never reorder or renumber. */
UENUM(BlueprintType)
enum class ERetakeUplinkDetonationMode : uint8
{
    DestroyUplink = 0 UMETA(DisplayName="Destroy Uplink"),
    TriggerTarget = 1 UMETA(DisplayName="Trigger Target")
};

/**
 * What a Trigger Target bomb site destroys (RetakeMapSDK element, MKD-029): an intact mesh and an optional wreck. Destruction swaps
 * visibility and collision on every machine and survives late joins; the explosion itself is the game's objective event. A site
 * with a planted charge explodes where the charge was planted; only a Field Uplink's remote detonation explodes at ExplosionPoint.
 * Registers with URetakeMapElementSubsystem and reports StateChannel when destroyed or restored.
 * Frozen per content generation: UPROPERTY names, order, types and defaults, the replicated members and the subobject names.
 * Retired names never to be reused with another type: Explosion.
 */
UCLASS(Blueprintable, meta=(DisplayName="Retake Detonation Target"))
class RETAKEMAPSDK_API ARetakeDetonationTarget : public AActor
{
    GENERATED_BODY()
public:
    ARetakeDetonationTarget();
    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Objectives|Destruction") TObjectPtr<UStaticMeshComponent> IntactMesh;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Objectives|Destruction") TObjectPtr<UStaticMeshComponent> DestroyedMesh;
    /** Where this target's own charge sits when a Field Uplink detonates it remotely: place it on the target's surface, where the
     *  explosion should start. Defaults to the actor's pivot. Validation warns when it floats (ARetakeModeObjectiveMarker). */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Objectives|Destruction") TObjectPtr<USceneComponent> ExplosionPoint;
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Retake|Objectives|Destruction") void SetDestroyed(bool bNewDestroyed);
    UFUNCTION(BlueprintPure, Category="Retake|Objectives|Destruction") bool IsDestroyed() const { return bDestroyed; }
    /** Local-only representation for preview fixtures; never replicated. */
    void SetPreviewDestroyed(bool bNewDestroyed);
    /** An inactive alternative placement (its marker's PlacementGroup) shows neither mesh and has no collision. Local and never
     *  replicated: every machine derives it from the replicated objective state, so it changes no wire layout. */
    void SetAvailable(bool bNewAvailable);
    bool IsAvailable() const { return bAvailable; }
    /** World location of ExplosionPoint (the actor's pivot without one). */
    FVector GetExplosionLocation() const;

    /** URetakeMapElementSubsystem state channel reported whenever the destroyed state changes. */
    static const FName StateChannel;
private:
    UPROPERTY(ReplicatedUsing=OnRep_Destroyed) bool bDestroyed = false;
    bool bAvailable = true;
    UFUNCTION() void OnRep_Destroyed();
    void ApplyRepresentation();
};
