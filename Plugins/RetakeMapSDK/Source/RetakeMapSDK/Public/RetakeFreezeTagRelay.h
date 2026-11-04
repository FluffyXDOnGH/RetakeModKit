// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RetakeFreezeTagRelay.generated.h"

class USceneComponent;
class UBillboardComponent;
class UDrawSphereComponent;

/**
 * Freeze Tag relay point (RetakeMapSDK element, MKD-029; FT-D39). Gameplay reads only its transform, ID and rotation order;
 * capture state lives in the game's public match view and the game presents it. Placed on every machine, never replicated;
 * registers with URetakeMapElementSubsystem.
 * Frozen per content generation: UPROPERTY names, order, types and defaults and the subobject names.
 */
UCLASS(Blueprintable)
class RETAKEMAPSDK_API ARetakeFreezeTagRelay : public AActor
{
    GENERATED_BODY()
public:
    ARetakeFreezeTagRelay();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    /** Stable, unique ID used by rotation, events and replay records. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|FreezeTag") FName RelayId = TEXT("Central");
    /** Relays rotate in ascending order, one per round. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|FreezeTag", meta=(ClampMin="0")) int32 RotationOrder = 0;

#if WITH_EDITORONLY_DATA
    UPROPERTY(VisibleAnywhere, Category="Retake|FreezeTag") TObjectPtr<UDrawSphereComponent> RadiusPreview;
    UPROPERTY(VisibleAnywhere, Category="Retake|FreezeTag") TObjectPtr<UBillboardComponent> Sprite;
#endif

private:
    UPROPERTY(VisibleAnywhere, Category="Retake|FreezeTag") TObjectPtr<USceneComponent> Root;
};
