// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "RetakeAudioCueDefinition.h"
#include "RetakeAcousticMapData.generated.h"

USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeAcousticMaterial
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio", meta=(ClampMin="0", ClampMax="1")) float Transmission = .1f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio", meta=(ClampMin="20", ClampMax="20000", Units="Hz")) float LowPassHz = 1000.f;
    bool IsValid() const;
};

USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeAcousticRegionData
{
    GENERATED_BODY()
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") FName Id;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") FTransform Transform;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") FVector Extent = FVector(100);
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") int32 Priority = 0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") FRetakeAcousticMaterial InternalTransmission;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") float ReverbDecaySeconds = .5f;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") TArray<int32> Connections;
    bool Contains(const FVector& Position) const;
};

USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeAcousticConnectionData
{
    GENERATED_BODY()
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") FName Id;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") int32 RegionA = INDEX_NONE;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") int32 RegionB = INDEX_NONE;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") FVector Position = FVector::ZeroVector;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") FRetakeAcousticMaterial OpenMaterial;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") FRetakeAcousticMaterial ClosedMaterial;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") bool bInitiallyOpen = true;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") bool bDynamic = false;
};

/** Bake independent of visibility/PVS. Contains only acoustic topology and no sound recordings. */
UCLASS(BlueprintType)
class RETAKEMAPSDK_API URetakeAcousticMapData : public UDataAsset
{
    GENERATED_BODY()
public:
    static constexpr int32 FormatVersion = 1;
    static constexpr int32 MaximumRegions = 512;
    static constexpr int32 MaximumConnections = 2048;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") int32 Version = FormatVersion;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") FString MapPackage;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") FString SourceSignature;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") TArray<FRetakeAcousticRegionData> Regions;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") TArray<FRetakeAcousticConnectionData> Connections;
    int32 FindRegion(const FVector& Position) const;
    bool Validate(FString& Error) const;
#if WITH_EDITOR
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};

/** A bounded pure topology result. Original source position is deliberately never redirected. */
struct RETAKEMAPSDK_API FRetakeAcousticPathResult
{
    float Gain = 0.f;
    float LowPassHz = 20000.f;
    float DistanceCm = 0.f;
    int32 Connections = 0;
    bool bSearchLimitReached = false;
};

namespace RetakeAcousticGraph
{
    constexpr int32 MaximumHops = 8;
    constexpr int32 MaximumAlternatives = 2;
    constexpr int32 MaximumSearchNodes = 256;
    /** Caller owns topology and direct-line policy. OpenStates is indexed by Connections and is snapshotted on the game thread. */
    RETAKEMAPSDK_API FRetakeAcousticPathResult Resolve(const URetakeAcousticMapData& Map, TConstArrayView<uint8> OpenStates,
        const FVector& Source, const FVector& Listener, const FRetakeAudioPropagationProfile& Profile);
}
