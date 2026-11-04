// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RetakeDetonationTarget.h"
#include "RetakeMapSDKIds.h"
#include "RetakeModeObjectiveMarker.generated.h"

class UArrowComponent;
class UBoxComponent;
class UStaticMeshComponent;
class UTextRenderComponent;

/** SDK contract (MKD-029): cooked maps store these numbers. Append only; never reorder or renumber. */
UENUM(BlueprintType)
enum class ERetakeModeMarkerKind : uint8 { BombSite = 0, TeamFlagHome = 1, NeutralFlagHome = 2, CaptureZone = 3, ControlPoint = 4 };

/**
 * Objective placement (RetakeMapSDK element, MKD-029): a bomb site, flag home, capture zone or control point. The game's objectives
 * component owns the rules and the token. A Field Uplink site's device is a built-in presentation the game attaches under
 * UplinkAnchor by DevicePresentationId; the marker keeps the gameplay geometry (Zone, UplinkBody, UplinkWreck) so collision never
 * depends on game content. Registers with URetakeMapElementSubsystem and reports StateChannel whenever its state changes.
 * Frozen per content generation: UPROPERTY names, order, types and defaults, the replicated members (live and replay wire layout)
 * and the subobject names. Retired names never to be reused with another type: FieldUplink, Uplink, TokenClass.
 */
UCLASS(Blueprintable)
class RETAKEMAPSDK_API ARetakeModeObjectiveMarker : public AActor
{
    GENERATED_BODY()
public:
    ARetakeModeObjectiveMarker();
    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void PostRegisterAllComponents() override;
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
#if WITH_EDITOR
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
    bool ContainsPawn(const APawn* Pawn) const;
    /** Why this placement cannot serve as an objective (zone size, interaction and pickup ranges), or empty. */
    FString GetPlacementError() const;
    bool UsesFieldUplink() const { return Kind == ERetakeModeMarkerKind::BombSite && bUseFieldUplink; }
    bool UsesFixedBombPlacement() const { return bFixedBombPlacement || UsesFieldUplink(); }
    /** A fixed bomb site without a Field Uplink: the charge is planted at the marker transform (the editor previews where), so the
     *  marker shows no pad or label and nothing marks the spot in game until a charge is planted. */
    bool UsesPlantedCharge() const { return Kind == ERetakeModeMarkerKind::BombSite && bFixedBombPlacement && !bUseFieldUplink; }
    bool IsObjectiveEnabled() const { return bObjectiveEnabled; }
    bool IsUplinkDestroyed() const { return bUplinkDestroyed; }
    /** A Field Uplink site whose device has not been destroyed: the game shows the device presentation. */
    bool ShouldShowDevice() const { return UsesFieldUplink() && !bUplinkDestroyed; }
    void SetObjectiveEnabled(bool bEnabled);
    /** Whether this site's charge is planted somewhere other than its Detonation Target: a Field Uplink that triggers its target
     *  remotely. Every other site's objective explosion (effect, sound and damage) starts at the planted charge, like MOHAA's
     *  global/obj_dm.scr, which plays the effect and radius damage at the bomb's origin. */
    bool DetonatesRemotely() const { return UsesFieldUplink() && DetonationMode == ERetakeUplinkDetonationMode::TriggerTarget; }
    /** Where the objective explosion starts for a charge planted at PlantedLocation: there, or the target's ExplosionPoint for a
     *  remote detonation. */
    FVector GetBlastOrigin(const FVector& PlantedLocation) const;
    /** Destroys the uplink or swaps the Detonation Target to its wreck. OutSource is the actor that is the objective itself, which the
     *  blast's cover traces ignore. */
    bool ExecuteDetonation(AActor*& OutSource);
    void ResetDetonation();
    /** How far a charge point (a planted-charge marker, or a remote target's ExplosionPoint) may sit from the surface it is mounted
     *  on. The charge's pivot is its base centre, it is 21 x 16 x 19 cm, and it stands 0.5-1 cm off its surface (DemolitionObjectives
     *  D13): resting on a floor its pivot is about 1 cm from it, hanging against a wall up to its half width plus that, about 11.5 cm
     *  (obj_team1's charges are 8.5 cm from the Flak 88 shield). */
    static constexpr float ChargeSeatToleranceCm = 12.f;
    /** Whether anything that blocks the Visibility channel, complex collision included, lies within ChargeSeatToleranceCm of
     *  Location, ignoring this marker. A charge point that fails floats, and so does its explosion. */
    bool IsChargeSeated(const FVector& Location) const;
    /** The validation finding for this site's charge point, or empty: a planted-charge site's marker, or a remote target's
     *  ExplosionPoint, with nothing within ChargeSeatToleranceCm. Needs the marker's world. */
    FString GetChargeSeatWarning() const;

    /** URetakeMapElementSubsystem state channel reported whenever the metadata, enabled or destroyed state changes. */
    static const FName StateChannel;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Replicated, Category="Retake|Objectives|Destruction") ERetakeUplinkDetonationMode DetonationMode = ERetakeUplinkDetonationMode::DestroyUplink;
    UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category="Retake|Objectives|Destruction", meta=(EditCondition="DetonationMode == ERetakeUplinkDetonationMode::TriggerTarget")) TObjectPtr<ARetakeDetonationTarget> DetonationTarget;
    // Authored before spawning and immutable for this marker's lifetime, including generated arenas.
    UPROPERTY(EditAnywhere, BlueprintReadOnly, ReplicatedUsing=OnRep_MarkerMetadata, Category="Retake|Objectives") FName ObjectiveId;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, ReplicatedUsing=OnRep_MarkerMetadata, Category="Retake|Objectives") ERetakeModeMarkerKind Kind = ERetakeModeMarkerKind::ControlPoint;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, ReplicatedUsing=OnRep_MarkerMetadata, Category="Retake|Objectives") int32 TeamIndex = INDEX_NONE;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Objectives") bool bFixedBombPlacement = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, ReplicatedUsing=OnRep_MarkerMetadata, Category="Retake|Objectives") bool bUseFieldUplink = true;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Objectives", meta=(ClampMin="1")) float InteractionRange = 200.f;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Objectives", meta=(ClampMin="1")) float PickupRadius = 65.f;
    /** Built-in device the game presents on a Field Uplink site (RetakeMapSDKIds). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, ReplicatedUsing=OnRep_MarkerMetadata, Category="Retake|Objectives", meta=(EditCondition="bUseFieldUplink", GetOptions="RetakeMapSDK.RetakeMapSDKIdOptions.GetDevicePresentationIds")) FName DevicePresentationId = RetakeMapSDKIds::FieldUplink;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Objectives") TObjectPtr<UBoxComponent> Zone;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Objectives") TObjectPtr<UStaticMeshComponent> Visual;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Objectives") TObjectPtr<UTextRenderComponent> Label;
    /** Frame of the device presentation. Imported device fronts face +Y; marker authoring uses +X forward, hence yaw -90. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Objectives") TObjectPtr<USceneComponent> UplinkAnchor;
    /** The device's collider: blocks pawns and shots while the device stands. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Objectives") TObjectPtr<UBoxComponent> UplinkBody;
    /** Shown and solid after the device is destroyed; the game assigns its mesh on every machine (server collision included). */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Objectives|Destruction") TObjectPtr<UStaticMeshComponent> UplinkWreck;
    /** Bomb sites sharing a group are alternative placements of one objective: the game enables one member per round and hides
     *  the other members' detonation targets. Empty: always active. Not replicated; every machine reads it from the map. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Objectives") FName PlacementGroup;
#if WITH_EDITORONLY_DATA
    /** Engine-only footprint of the device in editors that cannot show the game's presentation (Map Kit). */
    UPROPERTY() TObjectPtr<UStaticMeshComponent> DeviceProxy;
    UPROPERTY() TObjectPtr<UArrowComponent> DeviceFront;
#endif
private:
    UPROPERTY(ReplicatedUsing=OnRep_MarkerMetadata) bool bUplinkDestroyed = false;
    UPROPERTY(ReplicatedUsing=OnRep_MarkerMetadata) bool bObjectiveEnabled = true;
    UFUNCTION() void OnRep_MarkerMetadata();
};

/** Ready-to-place native bombsite preset; authors still assign a unique ObjectiveId. */
UCLASS(Blueprintable, meta=(DisplayName="Retake Field Uplink Bomb Site"))
class RETAKEMAPSDK_API ARetakeFieldUplinkBombSite : public ARetakeModeObjectiveMarker
{
    GENERATED_BODY()
public:
    ARetakeFieldUplinkBombSite();
};
