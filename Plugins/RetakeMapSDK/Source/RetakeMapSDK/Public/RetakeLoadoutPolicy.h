// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "RetakeLoadoutPolicy.generated.h"

/** Authoring slots, independent of the private game's inventory implementation. */
UENUM(BlueprintType)
enum class ERetakeLoadoutPolicySlot : uint8 { Primary, Secondary, Grenade, Equipment };

UENUM(BlueprintType)
enum class ERetakeLoadoutSlotRule : uint8 { Inherit, Empty, Choice, Fixed };

UENUM(BlueprintType)
enum class ERetakeLoadoutDistribution : uint8 { PlayerSelected, FixedLoadout, MapPickups };

UENUM(BlueprintType)
enum class ERetakeLoadoutRestriction : uint8 { DeploymentOnly, MatchWide };

/** An empty allowed list does not mean unrestricted: Inherit is the explicit existing-armory path. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeLoadoutSlotPolicy
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts") ERetakeLoadoutSlotRule Rule = ERetakeLoadoutSlotRule::Inherit;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts", meta=(GetOptions="RetakeMapSDK.RetakeLoadoutPolicy.GetWeaponIds")) TArray<FName> AllowedWeapons;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts", meta=(GetOptions="RetakeMapSDK.RetakeLoadoutPolicy.GetWeaponIds")) FName DefaultWeapon;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts") bool bAllowEmpty = false;
};

/** -1 inherits the game's starting grant; zero is a deliberate empty magazine/reserve/quantity. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeLoadoutGrantPolicy
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts", meta=(GetOptions="RetakeMapSDK.RetakeLoadoutPolicy.GetWeaponIds")) FName WeaponId;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts", meta=(ClampMin="-1")) int32 LoadedAmmo = -1;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts", meta=(ClampMin="-1")) int32 ReserveAmmo = -1;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts", meta=(ClampMin="-1")) int32 SecondaryReserveAmmo = -1;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts", meta=(ClampMin="-1")) int32 Quantity = -1;
};

/** Faction overrides replace explicit slots; Inherit retains the policy's shared slot. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeLoadoutFactionPolicy
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts") FRetakeLoadoutSlotPolicy Primary;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts") FRetakeLoadoutSlotPolicy Secondary;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts") FRetakeLoadoutSlotPolicy Grenade;
    /** Optional independent carried equipment; inherited policies grant none unless their armory does. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts") FRetakeLoadoutSlotPolicy Equipment;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts") TArray<FRetakeLoadoutGrantPolicy> Grants;
    /** Ammo pool -> item used when a starting-ammo grant has no selected representative (for example faction smoke variants). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts") TMap<FName,FName> AmmoRepresentatives;

    const FRetakeLoadoutSlotPolicy& GetSlot(ERetakeLoadoutPolicySlot Slot) const;
    FRetakeLoadoutSlotPolicy& GetSlot(ERetakeLoadoutPolicySlot Slot);
};

/** Pure ID-based definition shared by creator assets and server INIs. Contains no private game asset paths. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeLoadoutPolicyDefinition
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts") FName PolicyId;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts") ERetakeLoadoutDistribution Distribution = ERetakeLoadoutDistribution::PlayerSelected;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts") ERetakeLoadoutRestriction Restrictions = ERetakeLoadoutRestriction::DeploymentOnly;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts") bool bAllowMapPickups = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts") bool bAllowDroppedWeapons = true;
    /** False preserves normal grenade-mix grants; true grants only the resolved grenade slot. Empty then means no grenades. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts") bool bExplicitGrenadeGrants = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts") FRetakeLoadoutFactionPolicy Shared;
    /** Faction IDs, never the OBJ match-team IDs that switch factions at halftime. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts") TMap<int32, FRetakeLoadoutFactionPolicy> Factions;

    bool Validate(FString& Error) const;
    FRetakeLoadoutFactionPolicy ResolveFaction(int32 FactionId) const;
    FString Fingerprint() const;
    static bool IsValidId(FName Id);
    static bool IsValidIdText(const FString& Text);
};

/** Map-owned recommendation. Server policy assignments take precedence. */
UCLASS(BlueprintType)
class RETAKEMAPSDK_API URetakeLoadoutPolicy : public UDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts") FRetakeLoadoutPolicyDefinition Definition;
    UFUNCTION(Category="Retake|Loadouts") static TArray<FName> GetWeaponIds();
#if WITH_EDITOR
    bool ValidateAuthoringWeapons(FString& Error) const;
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};
