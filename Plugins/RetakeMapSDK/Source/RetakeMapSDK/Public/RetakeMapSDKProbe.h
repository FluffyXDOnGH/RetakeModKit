// Copyright 2026 Codnyx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RetakeMapSDKProbe.generated.h"

class UStaticMeshComponent;
class UMaterialInterface;

UENUM()
enum class ERetakeMapSDKProbeValue : uint8
{
    None,
    First,
    Second,
};

/**
 * Studio validation actor for the Map Kit proof (MK-043/MK-047). Every property defaults to zero; a probe map sets
 * known non-default values, and each process logs what it read from its own cooked package. It does not replicate,
 * so a client's log proves the client's deserialization rather than replicated state.
 */
UCLASS(NotBlueprintable)
class RETAKEMAPSDK_API ARetakeMapSDKProbe : public AActor
{
    GENERATED_BODY()
public:
    ARetakeMapSDKProbe();

    UPROPERTY(EditAnywhere, Category="Retake|Mods|Probe") int32 ProbeInteger = 0;
    UPROPERTY(EditAnywhere, Category="Retake|Mods|Probe") float ProbeFloat = 0.0f;
    UPROPERTY(EditAnywhere, Category="Retake|Mods|Probe") bool bProbeFlag = false;
    UPROPERTY(EditAnywhere, Category="Retake|Mods|Probe") FName ProbeName;
    UPROPERTY(EditAnywhere, Category="Retake|Mods|Probe") FString ProbeString;
    UPROPERTY(EditAnywhere, Category="Retake|Mods|Probe") FVector ProbeVector = FVector::ZeroVector;
    UPROPERTY(EditAnywhere, Category="Retake|Mods|Probe") FLinearColor ProbeColor = FLinearColor::Transparent;
    UPROPERTY(EditAnywhere, Category="Retake|Mods|Probe") TArray<int32> ProbeArray;
    UPROPERTY(EditAnywhere, Category="Retake|Mods|Probe") ERetakeMapSDKProbeValue ProbeEnum = ERetakeMapSDKProbeValue::None;
    /** Mod-owned material applied at BeginPlay, so a client proves the mod's own shader library. */
    UPROPERTY(EditAnywhere, Category="Retake|Mods|Probe") TSoftObjectPtr<UMaterialInterface> ProbeMaterial;

    /** One greppable line: event=MapSDKProbe ... */
    FString Describe() const;

protected:
    virtual void BeginPlay() override;

private:
    UPROPERTY(VisibleAnywhere, Category="Retake|Mods|Probe") TObjectPtr<UStaticMeshComponent> Mesh;
};
