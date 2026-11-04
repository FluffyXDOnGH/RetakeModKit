// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "RetakeAmbienceProfile.generated.h"

class USoundBase;
class USoundAttenuation;
class USoundConcurrency;

/** A looping, non-spatial layer (wind, distant war, room tone). A layer shared by two profiles keeps playing across a change. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeAmbienceBed
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio|Ambience") TObjectPtr<USoundBase> Sound;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio|Ambience", meta=(ClampMin="0", ClampMax="2")) float Volume=1.f;
};

/** A one-shot spawned at a random point around the listener (distant artillery, gusts, metal stress). */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeAmbienceElement
{
    GENERATED_BODY()
    /** One is picked at random per spawn, never the same one twice in a row when there is a choice. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio|Ambience") TArray<TObjectPtr<USoundBase>> Sounds;
    /** Seconds between spawns. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio|Ambience", meta=(Units="s")) FFloatInterval Interval=FFloatInterval(8.f,20.f);
    /** Horizontal distance from the listener. Distant-war elements stay at 6000 cm or more so they never read as a nearby fight. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio|Ambience", meta=(Units="cm")) FFloatInterval Distance=FFloatInterval(6000.f,12000.f);
    /** Height relative to the listener. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio|Ambience", meta=(Units="cm")) FFloatInterval Height=FFloatInterval(0.f,1500.f);
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio|Ambience") FFloatInterval Volume=FFloatInterval(.8f,1.f);
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio|Ambience") FFloatInterval Pitch=FFloatInterval(.95f,1.05f);
    /** Voices of this element alive at once; further spawns wait for the next interval. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio|Ambience", meta=(ClampMin="1", ClampMax="8")) int32 MaxActive=1;
    /** Overrides the sound's own attenuation when set. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio|Ambience") TObjectPtr<USoundAttenuation> Attenuation;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio|Ambience") TObjectPtr<USoundConcurrency> Concurrency;
};

/**
 * What a space sounds like: beds plus randomized elements (RetakeMapSDK asset). A map names one as its default (exterior)
 * profile; ambience zones override it while the listener is inside. Point emitters are placed AmbientSound actors. The game
 * plays every profile sound and every AmbientSound through its ambience sound class, whatever class the sound names.
 * Frozen per content generation: UPROPERTY names, order, types and defaults of the profile and both structs.
 */
UCLASS(BlueprintType)
class RETAKEMAPSDK_API URetakeAmbienceProfile : public UPrimaryDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio|Ambience") TArray<FRetakeAmbienceBed> Beds;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio|Ambience") TArray<FRetakeAmbienceElement> Elements;
    /** Crossfade when this profile starts or stops. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio|Ambience", meta=(ClampMin="0", ClampMax="10", Units="s")) float FadeSeconds=2.f;
    /** Structural checks that would break playback; the game runs them on every profile it loads. */
    bool ValidateProfile(TArray<FString>& Errors) const;
#if WITH_EDITOR
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};
