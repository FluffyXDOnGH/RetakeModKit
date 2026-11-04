// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RetakeDestructiblePlacement.generated.h"

class ARetakeDoor;
class URetakeDestructionDefinition;
class UGeometryCollectionComponent;
class UStaticMeshComponent;
class URetakeDestructibleTraversalComponent;

/**
 * Place this in maps, or a Blueprint child of it as a preset (definition and scale set, no graph needed). Preview
 * components are editor-only; only the server creates the gameplay actor. The runtime stays native: Blueprint children
 * add cosmetics through the events below, which run on clients and listen servers only (never on a dedicated server).
 * A map element (RetakeMapSDK registry): URetakeDestructionSubsystem observes it and spawns the runtime actor.
 */
UCLASS(Blueprintable)
class RETAKEDESTRUCTION_API ARetakeDestructiblePlacement : public AActor
{
    GENERATED_BODY()
public:
    ARetakeDestructiblePlacement();
    /** Enable and add ledge splines to vault/climb this prop while intact. Disabled by default; solo traversal only. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Traversal") TObjectPtr<URetakeDestructibleTraversalComponent> Traversal;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Definitions") TObjectPtr<URetakeDestructionDefinition> Definition;
    /**
     * Makes this destructible the leaf of Door. Place it on the door's closed leaf. It swings with the door and hangs from its
     * hinge edge. Once DoorCollapseFraction of its pieces have broken off, the rest falls away and the door is Broken (an open
     * frame) until the round reset. A door that starts locked has no door gameplay and keeps its own solid leaf: it cannot have one.
     */
    UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category="Retake|Destruction|Doors") TObjectPtr<ARetakeDoor> Door;
    /** Share of the leaf's pieces that must break off before the rest of the leaf falls away. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Doors", meta=(EditCondition="Door", ClampMin="0.05", ClampMax="1")) float DoorCollapseFraction=.4f;
    /** Width of the hinge-edge strip that never breaks and holds the leaf up. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Destruction|Doors", meta=(EditCondition="Door", ClampMin="1", Units="cm")) float DoorEdgeCm=20;
    virtual void OnConstruction(const FTransform& Transform) override;
#if WITH_EDITOR
    /** A definition from this map's own package root (its collection too), passing its own checks. */
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
    /** Cosmetic: a weapon, melee or explosion hit landed on this destructible. Location and surface normal in world space. */
    UFUNCTION(BlueprintImplementableEvent, Category="Retake|Destruction|Events") void OnDestructibleImpact(FVector Location, FVector Normal);
    /** Cosmetic: pieces broke loose at Location (rate-limited like the break sound and effect). */
    UFUNCTION(BlueprintImplementableEvent, Category="Retake|Destruction|Events") void OnDestructibleBreak(FVector Location);
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
#if WITH_EDITORONLY_DATA
    UPROPERTY() TObjectPtr<UGeometryCollectionComponent> Preview;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> FramePreview;
#endif
};
