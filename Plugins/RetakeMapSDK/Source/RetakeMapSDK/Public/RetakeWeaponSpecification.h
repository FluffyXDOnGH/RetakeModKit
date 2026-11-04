// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "RetakeWeaponResources.h"
#include "RetakeWeaponSpecification.generated.h"

/** Supported weapon authoring data. Game execution and runtime-class selection are not part of this contract. */
UCLASS(BlueprintType, NotBlueprintable)
class RETAKEMAPSDK_API URetakeWeaponSpecification : public UPrimaryDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Identity", AssetRegistrySearchable) FName WeaponId;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Identity") FText DisplayName;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Identity", AssetRegistrySearchable) ERetakeWeaponSlot Slot = ERetakeWeaponSlot::Primary;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Identity") FGameplayTag Category;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Selection") FRetakeWeaponSelectionSettings Selection;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Equipment") FRetakeWeaponEquipmentSettings Equipment;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Movement", meta=(ClampMin="0")) float MovementSpeedMultiplier = 1;
    /** Standing-jump takeoff lock, matched to this weapon's character jump animation. Simulated by Mover. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Movement", meta=(ClampMin="0", ClampMax="2")) float JumpTakeoffSeconds = 0.16666667f;
    /** Landing lock after a jump, matched to this weapon's character landing animation. Simulated by Mover. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Movement", meta=(ClampMin="0", ClampMax="2")) float JumpLandingSeconds = 0.3f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Aiming") FRetakeWeaponAimSettings Aim;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Provenance") FString SourceDefinition;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Provenance") FString SourceRevision;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Provenance") bool bProvisional = true;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Provenance", meta=(MultiLine="true")) FString ProvisionalNotes;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions") FRetakeWeaponActionSettings PrimaryAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions") FRetakeWeaponActionSettings SecondaryAction;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Ammo", meta=(AssetBundles="Game")) TSoftObjectPtr<URetakeAmmoDefinition> Ammo;
    /** Default deployment total: fill the primary magazine first, then reserve. Consumable grenades use this as their stack count.
     * Server/map grants may override it. Picking up or restoring a weapon never reapplies this grant. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Ammo", meta=(ClampMin="0")) int32 StartingAmmo = 0;
    /** Applied only while resolving a new deployment, never by pickup, weapon switching or inventory restoration. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Loadouts") TArray<FRetakeWeaponStartingItem> StartingItems;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Loadouts") TArray<FRetakeWeaponStartingAmmoGrant> StartingAmmoGrants;
    /** Optional independent secondary-fire reserve (for example rifle grenades). Empty uses the primary magazine. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Ammo", meta=(AssetBundles="Game")) TSoftObjectPtr<URetakeAmmoDefinition> SecondaryAmmo;
    /** Default deployment grant for SecondaryAmmo; server/map grant policies can override it. Never granted on a swap/drop. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Ammo", meta=(ClampMin="0")) int32 SecondaryStartingAmmo = 0;
    /** Zero for stack-based consumable grenades. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Ammo", meta=(ClampMin="0")) int32 MagazineCapacity = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions") FRetakeWeaponReloadSettings Reload;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio") FRetakeWeaponReloadAudio ReloadAudio;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions") FRetakeWeaponEquipSettings Equip;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions") FRetakeWeaponPutAwaySettings Holster;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions") FRetakeWeaponPutAwaySettings Drop;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Recoil") FRetakeWeaponViewKickSettings ViewKick;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile") FRetakeWeaponProjectileSettings Projectile;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Grenades") FRetakeGrenadeRules Grenade;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Presentation") FRetakeWeaponPresentationSettings Presentation;

    /** Resource identity is asset-specific: two stock profiles may define the same logical WeaponId. */
    virtual FPrimaryAssetId GetPrimaryAssetId() const override;
    bool ValidateSpecification(TArray<FText>& OutErrors) const;
    /** Loaded selected-profile references and starting-item cycles; shared by creator validation and game admission. */
    static bool ValidateStartingReferences(TConstArrayView<const URetakeWeaponSpecification*> Definitions,FString& Error);
    /** Runtime residency subset for the selected platform. Private catalogues are appended by the game adapter. */
    void GetReferencedSoftPaths(TArray<FSoftObjectPath>& OutPaths, bool bServer) const;
    /** Complete declared SDK authoring references, including inactive variants/editor presets. No derived game fields. */
    void GetAllReferencedSoftPaths(TArray<FSoftObjectPath>& OutPaths) const;
    /** Copies this contract's declared fields, never derived private implementation properties. */
    void CopySpecificationTo(URetakeWeaponSpecification& Destination) const;
#if WITH_EDITOR
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
#if WITH_EDITORONLY_DATA
    UPROPERTY(EditDefaultsOnly, Category="Retake|Weapons|Grips") ERetakeWeaponGripStyle GripStyle=ERetakeWeaponGripStyle::OneHand;
    UPROPERTY(EditDefaultsOnly, Category="Retake|Weapons|Grips") TMap<ERetakeWeaponGripStyle,FRetakeWeaponGripPreset> GripPresets;
#endif
};
