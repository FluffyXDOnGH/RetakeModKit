// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RetakeSpectatorOverview.generated.h"

class UArrowComponent;
class USceneComponent;
class UStaticMeshComponent;

/**
 * The view spectators get of the whole map (RetakeMapSDK element, MKD-029): the actor's location and rotation are the camera.
 * Place one per map. Only the server reads it, when it places a spectator, and sends the view to the client, so it never
 * replicates. Registers with URetakeMapElementSubsystem; the game uses the first one registered.
 * Frozen per content generation: the subobject names View, ViewProxy and ViewDirection.
 */
UCLASS(meta=(DisplayName="Retake Spectator Overview"))
class RETAKEMAPSDK_API ARetakeSpectatorOverview : public AActor
{
    GENERATED_BODY()
public:
    ARetakeSpectatorOverview();

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Spectating") TObjectPtr<USceneComponent> View;
#if WITH_EDITORONLY_DATA
    /** Engine camera mesh that shows where the view is and where it looks. */
    UPROPERTY() TObjectPtr<UStaticMeshComponent> ViewProxy;
    UPROPERTY() TObjectPtr<UArrowComponent> ViewDirection;
#endif

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
};
