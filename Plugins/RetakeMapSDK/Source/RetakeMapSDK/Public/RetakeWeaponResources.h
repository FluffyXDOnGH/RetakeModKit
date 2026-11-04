// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "RetakeWeaponData.h"
#include "RetakeWeaponResources.generated.h"

/** Reusable explicit character animation selection. Loaded separately from weapon balance. */
UCLASS(BlueprintType)
class RETAKEMAPSDK_API URetakeWeaponAnimationProfile : public UPrimaryDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Identity", AssetRegistrySearchable) FName ProfileId;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation") FPlayerAnims FirstPersonAnims;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation") FPlayerAnims ThirdPersonAnims;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation") FMontageData FirstPersonMontages;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation") FMontageData ThirdPersonMontages;
    /** Explicit no-animation draw/holster/drop. All corresponding FP/TP montage fields must be empty. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation") bool bNoHandlingAnimations=false;
    /** FP draw/holster/drop use their montage's authored rate and hold the final pose until the action changes.
     * Server deadlines still interrupt a longer clip. False retains fitting these clips to the authoritative phase. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation") bool bUseAuthoredFirstPersonHandlingRate=false;
    /** Opt-in accepted-shot body cycle. Replaces unrecorded TP fire only; FP and gameplay timing are unchanged. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation") TObjectPtr<UAnimMontage> ThirdPersonBoltCycle;
    /** Joined per-round TP clips retain their pose/gun contact between stages; only action entry/exit fade. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation") bool bContinuousThirdPersonReloadStages=false;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation") FMontageData WeaponMontages;
    /** How the third-person body follows the look while this weapon is held. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation") FRetakeThirdPersonAimPose ThirdPersonAim;
    virtual FPrimaryAssetId GetPrimaryAssetId() const override;
#if WITH_EDITOR
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};

/** Explicit reserve identity/capacity. Mods qualify AmmoId with their pack; category tags never merge reserves. */
UCLASS(BlueprintType)
class RETAKEMAPSDK_API URetakeAmmoDefinition : public UPrimaryDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Ammo", AssetRegistrySearchable) FName AmmoId;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Ammo") FText DisplayName;
    /** Descriptive ammunition category. Reserve ownership and sharing use AmmoId exclusively. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Ammo") FGameplayTag AmmoType;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Ammo", meta=(ClampMin="0")) int32 ReserveCapacity = 0;
    virtual FPrimaryAssetId GetPrimaryAssetId() const override;
    bool ValidateDefinition(TArray<FText>& OutErrors) const;
    /** Creator/native admission ownership checks for a weapon-pack ammunition definition. */
    bool ValidateForPack(const FString& PackId,TArray<FText>& OutErrors) const;
#if WITH_EDITOR
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};

/** Client-only shot tracer look: a bright flare streak that travels from the muzzle to the impact and fades behind the round. */
UCLASS(BlueprintType)
class RETAKEMAPSDK_API URetakeTracerProfile : public UDataAsset
{
    GENERATED_BODY()
public:
    /** Niagara system that draws one shot per activation. It reads the user parameters Start and End (positions),
     *  TravelSeconds and SpawnRate (SpritesPerSecond), runs once and completes on its own; the subsystem only activates and deactivates it. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Tracers") TObjectPtr<UNiagaraSystem> System;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Tracers", meta=(ClampMin="5000", ClampMax="200000", Units="cm/s")) float SpeedCmPerSecond = 30000.f;
    /** Flare sprites spawned per second at the travelling head (the reference flare uses 25 at the same 30000 cm/s). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Tracers", meta=(ClampMin="1", ClampMax="200")) float SpritesPerSecond = 25.f;
    /** How long the component stays active after the round arrives, so the last sprites can finish fading. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Tracers", meta=(ClampMin="0.05", ClampMax="2", Units="s")) float LingerSeconds = 0.5f;
    /** Short paths travel slower than Speed so the streak stays on screen for a few frames instead of under one. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Tracers", meta=(ClampMin="0", ClampMax="0.5", Units="s")) float MinimumVisibleSeconds = 0.06f;
    /** Paths shorter than this draw nothing; a point-blank streak reads as a flash, not a direction. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Tracers", meta=(ClampMin="0", ClampMax="2000", Units="cm")) float MinimumPathCm = 250.f;
    /** Upper bound on drawn path length; sky shots end here instead of at weapon range. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Tracers", meta=(ClampMin="500", ClampMax="50000", Units="cm")) float MaximumPathCm = 15000.f;

#if WITH_EDITOR
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};
