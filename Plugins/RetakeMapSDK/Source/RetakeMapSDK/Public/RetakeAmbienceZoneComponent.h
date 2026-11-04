// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Interfaces/IAudioGameplayVolumeInteraction.h"
#include "RetakeAmbienceZoneComponent.generated.h"

class URetakeAmbienceProfile;

/**
 * Add one to an Audio Gameplay Volume (RetakeMapSDK element component, MKD-029). The volume's own component tracks the listener
 * (audio thread, no overlaps or traces) and calls OnListenerEnter/Exit here; the game then plays this zone's profile instead of
 * the map default. Pair it with the stock Reverb and Filter volume components for an interior. Registers its owner with
 * URetakeMapElementSubsystem and reports StateChannel when the listener enters or leaves. Local only: nothing replicates, and a
 * dedicated server never sees a listener.
 * Frozen per content generation: UPROPERTY names, order, types and defaults.
 */
UCLASS(ClassGroup=(Retake), meta=(BlueprintSpawnableComponent, DisplayName="Retake Ambience Zone"))
class RETAKEMAPSDK_API URetakeAmbienceZoneComponent : public UActorComponent, public IAudioGameplayVolumeInteraction
{
    GENERATED_BODY()
public:
    URetakeAmbienceZoneComponent();
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio|Ambience") TObjectPtr<URetakeAmbienceProfile> Profile;
    /** The highest priority among the zones the listener is inside wins; ties go to the zone entered last. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio|Ambience") int32 Priority=0;
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void OnListenerEnter_Implementation() override;
    virtual void OnListenerExit_Implementation() override;
    /** Kept from the volume's last report, including one made before begin play: the volume reports a change once and never repeats it. */
    bool IsListenerInside() const { return bListenerInside; }
    /** URetakeMapElementSubsystem state channel reported on the owner whenever IsListenerInside changes. */
    static const FName StateChannel;
#if WITH_EDITOR
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
private:
    bool bListenerInside=false;
    void SetListenerInside(bool bInside);
};
