// © 2026 Codnyx. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RetakeWeaponPickup.generated.h"

class USkeletalMeshComponent;
class USphereComponent;
class UStaticMeshComponent;
class UTextRenderComponent;

/**
 * A weapon placed in a map (RetakeMapSDK element, MKD-029): which built-in weapon it holds (WeaponId, RetakeMapSDKIds::Weapons)
 * and how much. The game owns every rule (availability, reach, swaps, ammunition, respawn) and the presentation: it resolves
 * WeaponId to its weapon, fills WorldWeapon on every machine and publishes availability with SetAvailable on the server.
 * Registers with URetakeMapElementSubsystem and reports StateChannel whenever its availability changes. WorldWeapon is never
 * saved with a mesh, so a map never references game content; editors show the engine-only WeaponProxy and WeaponLabel.
 * Frozen per content generation: UPROPERTY names, order, types and defaults, the replicated member (live and replay wire layout)
 * and the subobject names DropCollision, WorldWeapon, InteractionRange, WeaponProxy and WeaponLabel. Retired names never to be
 * reused with another type: Definition, bMapAuthored, Payload, AttachmentParts, UpdatePickupPresentation.
 */
UCLASS(Blueprintable)
class RETAKEMAPSDK_API ARetakeWeaponPickup : public AActor
{
	GENERATED_BODY()
public:
	ARetakeWeaponPickup();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void OnConstruction(const FTransform& Transform) override;
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif

	/** True while the pickup holds something that can be taken. Server and client safe (replicated). */
	UFUNCTION(BlueprintPure, Category="Retake|Pickup") bool IsAvailable() const { return bAvailable; }
	/** Server only. The game publishes availability on every round reset, pickup and respawn. */
	void SetAvailable(bool bInAvailable);
	/** Why this placement cannot hold a weapon (unknown weapon, negative counts, invalid respawn delay), or empty. */
	UFUNCTION(BlueprintPure, Category="Retake|Pickup") FText GetConfigurationError() const;

	/** URetakeMapElementSubsystem state channel reported whenever availability changes (server and clients). */
	static const FName StateChannel;

	/** The built-in weapon this pickup holds (RetakeMapSDKIds::Weapons). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Pickup", meta=(GetOptions="RetakeMapSDK.RetakeMapSDKIdOptions.GetWeaponIds")) FName WeaponId;
	/** Rounds loaded in the weapon; the game caps them at its magazine capacity. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Pickup", meta=(ClampMin="0")) int32 MagazineRounds = 0;
	/** Spare rounds handed over with a firearm. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Pickup", meta=(ClampMin="0")) int32 SpareRounds = 0;
    /** Extra rounds for the weapon's independent secondary reserve; never added to its magazine. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Pickup", meta=(ClampMin="0")) int32 SecondarySpareRounds = 0;
	/** Grenades in the stack; the game caps them at the carry limit. Unused by firearms. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Pickup", meta=(ClampMin="0")) int32 Quantity = 1;
	UPROPERTY(EditDefaultsOnly, Category="Retake|Pickup", meta=(ClampMin="1", Units="cm")) float InteractionDistance = 200.f;
	/** Per-pickup respawn delay. Negative uses the active rules' map weapon respawn delay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Retake|Pickup", meta=(ClampMin="-1", Units="s")) float RespawnSecondsOverride = -1.f;
	/** False leaves a taken pickup unavailable until the next round reset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Retake|Pickup") bool bRespawnWhenTaken = true;
	/** True skips the game's availability rules (weapon distribution, disabled weapons, side selection). The weapon must still be
	 *  in the active armory. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Retake|Pickup") bool bAlwaysAvailable = false;
	/** Draws the interaction sphere while playing; the editor viewport always shows it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Pickup|Debug") bool bShowInteractionRange = false;
	/** The weapon as the game presents it (subobject WorldWeapon). Never saved with a mesh. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Pickup") TObjectPtr<USkeletalMeshComponent> Mesh;
	/** Root. A small sphere without collision; the name is historical. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Pickup") TObjectPtr<USphereComponent> DropCollision;
	/** Editor and debug visualizer for InteractionDistance. No collision, never affects gameplay. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Pickup") TObjectPtr<USphereComponent> InteractionRange;
#if WITH_EDITORONLY_DATA
	/** Engine-only stand-in for the weapon (about 80 x 6 x 20 cm) in editors that cannot show the game's presentation. */
	UPROPERTY() TObjectPtr<UStaticMeshComponent> WeaponProxy;
	/** Shows WeaponId above the proxy. */
	UPROPERTY() TObjectPtr<UTextRenderComponent> WeaponLabel;
#endif

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY(Transient, ReplicatedUsing=OnRep_Available) bool bAvailable = false;
	UFUNCTION() void OnRep_Available();
	void UpdateInteractionRange() const;
};
