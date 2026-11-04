// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Math/RandomStream.h"
#include "RetakeAudioCueDefinition.generated.h"

class USoundWave;
class USoundBase;

UENUM(BlueprintType)
enum class ERetakeAudioCategory : uint8 { Gameplay, Ambience, Music, Interface, Announcer, Voice, Count UMETA(Hidden) };

UENUM(BlueprintType)
enum class ERetakeAudioLoading : uint8 { Critical, Streamed };

USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeAudioPropagationProfile
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio", meta=(ClampMin="0", Units="cm")) float RangeCm = 2500.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio", meta=(ClampMin="0", Units="cm")) float FullGainRadiusCm = 100.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio", meta=(ClampMin="1", ClampMax="4")) float FalloffExponent = 2.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio", meta=(ClampMin="0", ClampMax="1")) float ReverbSend = .08f;
    float GainAtDistance(float PathDistanceCm) const;
    bool Validate(FString& Error) const;
};

USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeAudioRecording
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio", meta=(AssetBundles="Client")) TSoftObjectPtr<USoundWave> Wave;
    /** Entry in the checked-in source manifest, including edit recipe and acquisition date. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio") FName SourceId;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio", meta=(ClampMin="-60", ClampMax="12", Units="dB")) float GainDb = 0.f;
    /** Linear recording-specific ranges, applied before the cue's shared semitone variation. Equal endpoints are fixed. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio", meta=(ClampMin="0", ClampMax="4")) float VolumeMinimum = 1.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio", meta=(ClampMin="0", ClampMax="4")) float VolumeMaximum = 1.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio", meta=(ClampMin="0.125", ClampMax="4")) float PitchMinimum = 1.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio", meta=(ClampMin="0.125", ClampMax="4")) float PitchMaximum = 1.f;
    bool HasValidVariation() const;
    /** Default fixed-one ranges consume no randomness, preserving existing cue selection/pitch sequences. */
    void ResolveVariation(FRandomStream& Random, float& OutVolume, float& OutPitch) const;
};

/** Semantic cue. The server loads definitions, never the soft-referenced recordings. IDs are stable within a catalogue revision. */
UCLASS(BlueprintType)
class RETAKEMAPSDK_API URetakeAudioCueDefinition : public UPrimaryDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio", meta=(ClampMin="1", ClampMax="65535")) int32 CueId = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio") FName SemanticName;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio") ERetakeAudioCategory Category = ERetakeAudioCategory::Gameplay;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio") TArray<FRetakeAudioRecording> Recordings;
    /** Optional reusable MetaSound; all recordings are prepared before it is instantiated. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio", meta=(AssetBundles="Client")) TSoftObjectPtr<USoundBase> Generator;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio") ERetakeAudioLoading Loading = ERetakeAudioLoading::Critical;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio") bool bSpatial = true;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio") bool bLooping = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio") FRetakeAudioPropagationProfile Propagation;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio", meta=(ClampMin="1", ClampMax="100")) float Priority = 50.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio", meta=(ClampMin="1", ClampMax="64")) int32 MaximumInstances = 8;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio", meta=(ClampMin="-60", ClampMax="12", Units="dB")) float GainDb = 0.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio", meta=(ClampMin="0", ClampMax="2")) float PitchVariationSemitones = .25f;
    bool Validate(FString& Error) const;
#if WITH_EDITOR
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};

/** Complete, explicit cue catalogue. Missing IDs/recordings are readiness errors, never legacy fallbacks. */
UCLASS(BlueprintType)
class RETAKEMAPSDK_API URetakeAudioLibrary : public UDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio") int32 Revision = 1;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio") TArray<TObjectPtr<URetakeAudioCueDefinition>> Cues;
    bool Validate(FString& Error) const;
#if WITH_EDITOR
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};
