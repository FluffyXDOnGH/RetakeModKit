// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "RetakeMapSDKIds.generated.h"

/**
 * Built-in resources a map references by ID (RetakeMapSDK seam, MKD-029): the game resolves each ID to its own content, so maps
 * never reference game assets. SDK contract: append only; adding an ID bumps ContractVersion, because an older game cannot
 * resolve it.
 */
namespace RetakeMapSDKIds
{
	/** Objective device presentations (ARetakeModeObjectiveMarker::DevicePresentationId). */
	inline const FName FieldUplink(TEXT("FieldUplink"));
	/** Teammate boost profiles (FRetakeLedgeAccessSettings::BoostProfileId): the standard 2.5-5 m two-player boost. */
	inline const FName StandardBoost(TEXT("Standard"));

	RETAKEMAPSDK_API const TArray<FName>& DevicePresentations();
	/** Weapons a map pickup can hold (ARetakeWeaponPickup::WeaponId): the game's weapon IDs, current generation only. */
	RETAKEMAPSDK_API const TArray<FName>& Weapons();
	RETAKEMAPSDK_API const TArray<FName>& BoostProfiles();
}

/** Details-panel options for ID properties (meta=(GetOptions=...)). */
UCLASS(MinimalAPI)
class URetakeMapSDKIdOptions : public UObject
{
	GENERATED_BODY()
public:
	UFUNCTION()
	static TArray<FName> GetDevicePresentationIds();
	UFUNCTION()
	static TArray<FName> GetWeaponIds();
	UFUNCTION()
	static TArray<FName> GetBoostProfileIds();
};
