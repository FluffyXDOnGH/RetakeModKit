// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "RetakeWeaponHitRegions.h"
#include "RetakeWeaponData.generated.h"

class UAnimSequence;
class UAnimMontage;
class UNiagaraSystem;
class USkeletalMesh;
class UStaticMesh;
class USoundBase;
class URetakeAudioCueDefinition;
class USoundAttenuation;
class USoundConcurrency;
class URetakeTracerProfile;
class UTexture2D;
class UMaterialInterface;
class URetakeWeaponAnimationProfile;

UENUM(BlueprintType)
// Keep the first two serialized values stable for existing assets and snapshots.
enum class ERetakeWeaponSlot : uint8 { Primary = 0, Secondary = 1, Grenade = 2, Equipment = 3 };

namespace RetakeWeaponSlots
{
    inline constexpr ERetakeWeaponSlot All[] = {ERetakeWeaponSlot::Primary, ERetakeWeaponSlot::Secondary,
        ERetakeWeaponSlot::Grenade, ERetakeWeaponSlot::Equipment};
    inline bool IsOptional(ERetakeWeaponSlot Slot)
    { return Slot==ERetakeWeaponSlot::Grenade || Slot==ERetakeWeaponSlot::Equipment; }
}

/** Deployment-only extra item. The target uses its own starting totals; shared targets are granted once. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponStartingItem
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Loadouts") FName WeaponId;
};

/** Deployment-only ammunition addition. Prefer an already selected item using this pool; otherwise use an explicit representative. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponStartingAmmoGrant
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Loadouts") FName AmmoId;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Loadouts", meta=(ClampMin="1", ClampMax="1000000")) int32 Amount=1;
    /** Used only when the inventory and the resolved policy provide no representative for this pool. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Loadouts") FName FallbackWeaponId;
};

/** Which impact response a weapon or projectile uses. Every firearm shares Bullet; grenades bounce as fragments or canisters. */
UENUM(BlueprintType)
enum class ERetakeSurfaceImpactFamily : uint8
{
    None,
    Bullet,
    FragmentBounce,
    CanisterBounce
};

/** Trigger behaviour of a firing action (Docs/Proposals/WeaponFireModes.md). Burst follows CoD4/Promod (bg_weapons.cpp ShotLimitReached). */
UENUM(BlueprintType)
enum class ERetakeFireMode : uint8 { Single, Burst, Automatic };

/**
 * The fire modes a player may select for an action, and the burst rules. Every leaf is scalar so a preset INI can override it
 * (RetakeGameplayRules.cpp accepts no arrays). One allowed mode means no selector. Replaced bAutomatic in rules format 17.
 */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeFireModeSettings
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions") bool Single = true;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions") bool Burst = false;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions") bool Automatic = false;
    /** The mode a newly spawned weapon starts in; must be allowed. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions") ERetakeFireMode Default = ERetakeFireMode::Single;
    /** Rounds per trigger pull in Burst (CoD4 BURSTFIRE2-4). */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions", meta=(ClampMin="2", ClampMax="4")) int32 BurstShots = 3;
    /** Wait after a burst's last interval before the next pull (CoD4 player_burstFireCooldown, 0.2 s; Promod keeps it). */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions", meta=(ClampMin="0", ClampMax="5", Units="s")) float BurstCooldownSeconds = 0.2f;
    /** Spacing of a burst's rounds; 0 uses the action's IntervalSeconds. The server's cadence check uses the same value. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions", meta=(ClampMin="0", ClampMax="60", Units="s")) float BurstIntervalSeconds = 0.f;
    /** Multiplies a burst round's camera and gun kick impulses (caps and recovery unchanged). 1 kicks like the other modes. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions", meta=(ClampMin="0", ClampMax="4")) float BurstKickScale = 1.f;
    /** Multiplies a burst round's server-resolved spread cone and classic deviations. 1 spreads like the other modes. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions", meta=(ClampMin="0", ClampMax="4")) float BurstSpreadScale = 1.f;

    bool Allows(ERetakeFireMode Mode) const
    { return Mode == ERetakeFireMode::Single ? Single : Mode == ERetakeFireMode::Burst ? Burst : Mode == ERetakeFireMode::Automatic && Automatic; }
    int32 Count() const { return int32(Single) + int32(Burst) + int32(Automatic); }
    /** Mode when allowed, else Default. */
    ERetakeFireMode Resolve(ERetakeFireMode Mode) const { return Allows(Mode) ? Mode : Default; }
    /** The next allowed mode after Mode in Single, Burst, Automatic order, wrapping; Mode itself when no other is allowed. */
    ERetakeFireMode Next(ERetakeFireMode Mode) const
    {
        for (int32 Step = 1; Step <= 3; ++Step)
            if (const ERetakeFireMode Candidate = ERetakeFireMode((uint8(Mode) + Step) % 3); Allows(Candidate)) return Candidate;
        return Mode;
    }
    /** The spacing a shot fired in Mode keeps: a burst's own interval when set, else the action's. */
    double ShotInterval(ERetakeFireMode Mode, double ActionIntervalSeconds) const
    { return Mode == ERetakeFireMode::Burst && BurstIntervalSeconds > 0.f ? double(BurstIntervalSeconds) : ActionIntervalSeconds; }
    float KickScale(ERetakeFireMode Mode) const { return Mode == ERetakeFireMode::Burst ? BurstKickScale : 1.f; }
    float SpreadScale(ERetakeFireMode Mode) const { return Mode == ERetakeFireMode::Burst ? BurstSpreadScale : 1.f; }
    /**
     * Shared by asset and preset validation. Burst and Automatic need a firearm (hitscan, pellets, rocket); secondary actions
     * (bash, grenade, none) have no selector, so they allow Single only.
     */
    bool Validate(bool bFirearm, bool bSecondary, FString& OutProblem) const
    {
        const auto Scale = [](float Value) { return FMath::IsFinite(Value) && Value >= 0.f && Value <= 4.f; };
        if (uint8(Default) > uint8(ERetakeFireMode::Automatic)) OutProblem = TEXT("Default is not a fire mode.");
        else if (Count() == 0) OutProblem = TEXT("at least one mode must be allowed.");
        else if (!Allows(Default)) OutProblem = TEXT("Default must be an allowed mode.");
        else if ((Burst || Automatic) && (bSecondary || !bFirearm)) OutProblem = TEXT("only a primary firearm may allow Burst or Automatic.");
        else if (BurstShots < 2 || BurstShots > 4) OutProblem = TEXT("BurstShots must be in [2,4].");
        else if (!FMath::IsFinite(BurstCooldownSeconds) || BurstCooldownSeconds < 0 || BurstCooldownSeconds > 5)
            OutProblem = TEXT("BurstCooldownSeconds must be in [0,5].");
        else if (!FMath::IsFinite(BurstIntervalSeconds) || (BurstIntervalSeconds != 0.f && (BurstIntervalSeconds < .001f || BurstIntervalSeconds > 60.f)))
            OutProblem = TEXT("BurstIntervalSeconds must be 0 (the action interval) or in [0.001,60].");
        else if (!Scale(BurstKickScale) || !Scale(BurstSpreadScale)) OutProblem = TEXT("BurstKickScale and BurstSpreadScale must be in [0,4].");
        else return true;
        return false;
    }
    static FRetakeFireModeSettings Only(ERetakeFireMode Mode)
    {
        FRetakeFireModeSettings Settings;
        Settings.Single = Mode == ERetakeFireMode::Single; Settings.Burst = Mode == ERetakeFireMode::Burst;
        Settings.Automatic = Mode == ERetakeFireMode::Automatic; Settings.Default = Mode;
        return Settings;
    }
};

/**
 * A fire-selector lever modelled as its own weapon part (Presentation.Attachments), turned per selected mode. Weapon meshes play
 * clips without an animation Blueprint, so a lever bone cannot hold a per-mode pose; a part on a socket keeps its rotation under
 * every clip at no per-frame cost. Author the part with its pivot on the lever axis; each rotation is applied in the part's own
 * frame after its attachment Offset. An empty PartSlot leaves the weapon without a moving lever.
 */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeFireSelectorPresentation
{
    GENERATED_BODY()
    /** Attachment slot of the lever part. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Fire Modes") FName PartSlot;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Fire Modes") FRotator SingleRotation = FRotator::ZeroRotator;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Fire Modes") FRotator BurstRotation = FRotator::ZeroRotator;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Fire Modes") FRotator AutomaticRotation = FRotator::ZeroRotator;
    bool IsSet() const { return !PartSlot.IsNone(); }
    const FRotator& RotationFor(ERetakeFireMode Mode) const
    { return Mode == ERetakeFireMode::Burst ? BurstRotation : Mode == ERetakeFireMode::Automatic ? AutomaticRotation : SingleRotation; }
};

UENUM(BlueprintType)
enum class ERetakeAimMode : uint8 { None, IronSights, Scope };

/** Sight alignment reference on a view mesh: a socket or bone whose origin is the aperture or reticle centre, +X along the
 *  sight line toward the target, +Z up. Presentation only; shots keep the gameplay aim. At full aim the arms rig is solved
 *  so this point sits on the camera axis at EyeReliefCm, and third person converges this axis on the owner's aim. Parts
 *  (attachments) that set one beat the weapon mesh; among parts the last listed provider wins, so optics are listed after
 *  irons. None keeps the authored Aim.FirstPersonRigTransform. Docs/Systems/Combat/ADSAndBash.md. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponAimPoint
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Aiming|Presentation") FName Socket;
    /** Authoring nudge in socket space; scale is ignored. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Aiming|Presentation") FTransform Offset;
    /** Eye to aim point along the view axis. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Aiming|Presentation", meta=(ClampMin="3", ClampMax="60", Units="cm")) float EyeReliefCm = 12.f;
    bool IsSet() const { return !Socket.IsNone(); }
};

/** Static authored handling and viewmodel configuration. Live progress belongs to Mover. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponAimSettings
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Aiming") ERetakeAimMode Mode = ERetakeAimMode::None;
    /** False denies iron-sight ADS on this weapon whatever the server's ADS rules allow. Scopes ignore it (Actions.AllowScopes). */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Aiming") bool bAllowAimDownSights = true;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Aiming", meta=(ClampMin="1", ClampMax="170")) float FieldOfView = 60.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Aiming", meta=(ClampMin="0.01", ClampMax="5", Units="s")) float InSeconds = 0.15f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Aiming", meta=(ClampMin="0.01", ClampMax="5", Units="s")) float OutSeconds = 0.10f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Aiming", meta=(ClampMin="0.01", ClampMax="1")) float MovementMultiplier = 0.75f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Aiming", meta=(ClampMin="0", ClampMax="1")) float SpreadMultiplier = 0.5f;
    /** Full-aim arms component transform relative to its camera parent; the weapon remains attached to the hand. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Aiming|Presentation") FTransform FirstPersonRigTransform;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Aiming|Presentation") bool bAlignmentAuthored = false;
    /** The weapon mesh's own sight (its irons). The world mesh should carry the same socket so third person uses the same axis. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Aiming|Presentation") FRetakeWeaponAimPoint AimPoint;
};

UENUM(BlueprintType)
enum class ERetakeWeaponSelectionRole : uint8 { Firearm, Melee, Equipment };

UENUM(BlueprintType)
enum class ERetakeEquipmentUse : uint8 { None, ActivateTarget };

/** Native, non-damaging equipment capability. It never enters weapon attack/damage resolution. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponEquipmentSettings
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Equipment") ERetakeEquipmentUse Use = ERetakeEquipmentUse::None;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Equipment", meta=(ClampMin="0.01", ClampMax="60", Units="s")) float UseIntervalSeconds = .75f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Equipment", meta=(ClampMin="0", ClampMax="100000", Units="cm")) float UseRangeCm = 0.f;
    /** Enters the authored Scope view while equipped/ready; still follows the server's AllowScopes. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Equipment") bool bAutomaticScope = false;
    /** Secondary input selects the previously held item using the existing authored put-away/equip clocks. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Equipment") bool bSecondaryReturnsToPrevious = false;
    /** Wait before requesting the previous item. The current optic stays active during this delay. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Equipment", meta=(ClampMin="0", ClampMax="2", Units="s")) float SecondaryReturnDelaySeconds = 0.f;
    /** Hide the carried item's meshes in both views. Pickup/dropped presentation is separate. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Equipment") bool bHideHeldMeshes = false;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Equipment") bool bAllowDrop = true;
    /** Explicit intentional silence; prevents a forgotten recording from masquerading as a complete use binding. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio") bool bSilentUse = false;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio", meta=(AssetBundles="Game")) TSoftObjectPtr<URetakeAudioCueDefinition> UseCue;
    bool IsEnabled() const { return Use==ERetakeEquipmentUse::ActivateTarget; }
};

/** Selection metadata is independent of damage, equip timing and inventory position. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponSelectionSettings
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Selection")
    ERetakeWeaponSelectionRole Role = ERetakeWeaponSelectionRole::Firearm;
    /** Ascending within the input slot (within a grenade family for manual selection); ties use stable IDs. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Selection", meta=(ClampMin="0"))
    int32 CycleOrder = 0;
    /** Higher values win within an automatic role after the remembered firearm preference. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Selection")
    int32 AutomaticPriority = 0;
    /** Explicit opt-in. Manual selection is independent of this flag. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Selection")
    bool bAllowAutomaticSelection = false;
};

/** Third-person weapon presentation near walls and the aim. The authored wall poses (driven by the clearance stage) keep
 *  the weapon out of walls; first person draws with the camera's first-person scale and needs no pose change. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponClearanceProfile
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Clearance") bool bEnabled = true;
    /** Third person: largest turn about the grip that lines the barrel up with the aim (where the crosshair points). 0 disables. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Clearance", meta=(ClampMin="0", ClampMax="45")) float ThirdPersonAimConvergenceDegrees = 20.f;
    /** Right-lean elbow preference toward gravity, with a small outward bias. 0 retains the authored bend.
     *  Uses the recorded lean and existing action/sprint gates; no extra clock, wrist movement or bone stretching. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Clearance", meta=(ClampMin="0", ClampMax="1")) float ThirdPersonLeanElbowDownWeight = 0.f;
    /** First person: largest turn of the arms about the grip that lines the barrel up with the view (so it runs into the crosshair). 0 disables. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Clearance", meta=(ClampMin="0", ClampMax="45")) float FirstPersonAimConvergenceDegrees = 20.f;
};

/** One part fitted on a weapon: a static mesh on a socket of the weapon mesh, or of another part (a front sight on a handguard). */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponAttachment
{
    GENERATED_BODY()
    /** Stable slot name such as Handguard, SightFront, SightRear, Magazine, Scope, Muzzle, Grip, Laser. One part per slot. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Attachments") FName Slot;
    /** Socket on the weapon mesh (both view meshes need it), or on the parent part's static mesh when ParentSlot is set. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Attachments") FName Socket;
    /** Slot of the part this one sits on; None attaches to the weapon mesh itself. Parents must be listed first. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Attachments") FName ParentSlot;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Attachments", meta=(AssetBundles="Client")) TSoftObjectPtr<UStaticMesh> Mesh;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Attachments") FTransform Offset;
    /** Set when this part is a sight (irons, an optic): the aim point socket on this part's static mesh. Beats the weapon
     *  mesh's own aim point; the last listed provider wins. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Attachments") FRetakeWeaponAimPoint AimPoint;
};

USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FPlayerAnims
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation")
	TObjectPtr<UAnimSequence> IdleAnim = nullptr;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation")
	TObjectPtr<UAnimSequence> AimIdleAnim = nullptr;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation")
	TObjectPtr<UAnimSequence> CrouchIdleAnim = nullptr;

	/** Optional authored running loop. First person samples it by recorded gait phase; unset retains procedural running. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation")
	TObjectPtr<UAnimSequence> RunAnim = nullptr;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation")
	TObjectPtr<UAnimSequence> SprintAnim = nullptr;

};

/** Third-person look pose, applied after the animation graph: the look pitch and yaw relative to the displayed body are
 *  spread over the spine, neck and head, each joint turning about itself, and part of the look pitch raises or lowers both
 *  arms at the clavicles (as the reference first-person pack's third-person graph does). The spine shares plus Arms is how far the held weapon follows the look pitch (1 = exactly);
 *  the spine shares plus Neck and Head is how far the head follows pitch. Root-to-view yaw is owned once by the shared spine solve. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeThirdPersonAimPose
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation|Aim", meta=(ClampMin="0", ClampMax="1")) float Spine01 = 0.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation|Aim", meta=(ClampMin="0", ClampMax="1")) float Spine02 = 0.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation|Aim", meta=(ClampMin="0", ClampMax="1")) float Spine03 = .03f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation|Aim", meta=(ClampMin="0", ClampMax="1")) float Spine04 = .03f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation|Aim", meta=(ClampMin="0", ClampMax="1")) float Spine05 = .04f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation|Aim", meta=(ClampMin="0", ClampMax="1")) float Arms = .9f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation|Aim", meta=(ClampMin="0", ClampMax="1")) float Neck = .25f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation|Aim", meta=(ClampMin="0", ClampMax="1")) float Head = .25f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation|Aim", meta=(ClampMin="0", ClampMax="89", Units="deg")) float MaxPitchUpDegrees = 80.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation|Aim", meta=(ClampMin="0", ClampMax="89", Units="deg")) float MaxPitchDownDegrees = 80.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation|Aim", meta=(ClampMin="0", ClampMax="180", Units="deg")) float MaxYawDegrees = 90.f;
};

USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FMontageData
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation")
	TObjectPtr<UAnimMontage> EquipMontage = nullptr;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation")
	TObjectPtr<UAnimMontage> HolsterMontage = nullptr;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation")
	TObjectPtr<UAnimMontage> DropMontage = nullptr;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation")
	TObjectPtr<UAnimMontage> ReloadMontage = nullptr;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation")
	TObjectPtr<UAnimMontage> FireMontage = nullptr;

	/** Cosmetic inspection; gameplay actions can interrupt it immediately. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation")
	TObjectPtr<UAnimMontage> InspectMontage = nullptr;

	/**
	 * Cosmetic fire-selector flick (WEAPON-11), first person only: the owner plays it locally when switching, with a HipFire and an
	 * Aiming track of the same additive; a shot or an action replaces it. Not a hit-rig bake input; leave it empty in ThirdPersonMontages.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation")
	TObjectPtr<UAnimMontage> FireModeSwitchMontage = nullptr;
};

/** Cosmetic camera-space movement of the arms and their attached weapon. Never applied to the view or hit body. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeFirstPersonMotionSettings
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation|FirstPerson", meta=(ClampMin="0",ClampMax="2")) float Strength=1.f;
    /** X forward, Y right, Z up, in centimetres. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation|FirstPerson") FVector StrideTranslation=FVector(.15f,.5f,1.f);
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation|FirstPerson", meta=(ClampMin="0",ClampMax="5")) float StrideRollDegrees=1.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation|FirstPerson", meta=(ClampMin="0",ClampMax="5")) float SwayDegrees=1.5f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation|FirstPerson", meta=(ClampMin="0",ClampMax="3")) float SettleDistanceCm=.65f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation|FirstPerson", meta=(ClampMin="1",ClampMax="40")) float Response=12.f;
};

USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FReticleParams
{
	GENERATED_BODY()

	// Shape Cut Factor


	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category="Retake|Reticle")
	float ShapeCutFactor_RoundFired = 0.f;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category="Retake|Reticle")
	float ShapeCutFactor_Aiming = 0.f;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category="Retake|Reticle")
	float ShapeCutFactor_NotAiming = 0.f;

	// Scale Factor


	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category="Retake|Reticle")
	float ScaleFactor_RoundFired = 0.f;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category="Retake|Reticle")
	float ScaleFactor_Aiming = 0.f;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category="Retake|Reticle")
	float ScaleFactor_NotAiming = 0.f;




	// Interp Speeds

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category="Retake|Reticle")
	float RoundFiredInterpSpeed = 20.f;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category="Retake|Reticle")
	float AimingInterpSpeed = 15.f;



};

UENUM(BlueprintType)
enum class ERetakeGrenadeEffect : uint8 { Explosion, Smoke, Incendiary, Flash };

/**
 * What a non-explosive grenade's activation does to destructibles: the explosion field an HE blast of this damage and radius
 * applies (collections with the authored response take the fixed explosion field, as for HE). It never damages players.
 */
struct RETAKEMAPSDK_API FRetakeGrenadeDestructionBlast
{
    float Damage = 0.f;
    float RadiusCm = 0.f;
    /** Above zero, only destructibles a grenade contact of this damage breaks through (their GrenadeBreakDamage); zero, every one. */
    float MaxGrenadeBreakDamage = 0.f;
    bool IsEnabled() const { return Damage>0 && RadiusCm>0; }
};

/** Molotov fire patch. A floor disc that damages bodies standing in it at a fixed cadence; no lingering burn. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeFireRules
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Fire") float RadiusCm = 150.f;
    /** Feet within this height above the burning floor are standing in the fire. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Fire") float HeightCm = 120.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Fire") float DurationSeconds = 7.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Fire") float TickIntervalSeconds = .5f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Fire") float DamagePerTick = 8.f;
    /** A wall or ceiling impact puts the fire on the floor below, at most this far down; none found means no fire. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Fire") float MaxFloorDropCm = 300.f;
    /** Ignition breaks destructibles as the HE grenade's blast does (200 damage, 762 cm). Zero damage disables it. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Fire") float DestructionDamage = 200.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Fire") float DestructionRadiusCm = 762.f;
    /** Zero: every destructible in reach. Above zero: only those whose GrenadeBreakDamage is at most this. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Fire") float DestructionMaxGrenadeBreakDamage = 0.f;
};

/** Flashbang exposure. Distance and facing are parametric so every value stays an INI-tunable scalar. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeFlashRules
{
    GENERATED_BODY()
    // 1500/300 Source-engine units converted at the project's established 1.905 cm/unit scale
    // (see Scripts/Weapons/prepare_gameplay_presets.py SCALE); the un-converted raw values left the
    // flash only effective at point-blank range.
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Flash") float RadiusCm = 2857.5f;
    /** Full distance strength inside this radius, then (1-t)^DistanceExponent out to RadiusCm. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Flash") float FullStrengthRadiusCm = 571.5f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Flash") float DistanceExponent = 1.f;
    /** Full facing strength while the burst is within this angle of the view direction. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Flash") float FullFacingDegrees = 45.f;
    /** Facing strength with the burst directly behind; blends linearly from FullFacingDegrees to 180. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Flash") float BehindFacingScale = .15f;
    /** White-out hold at full exposure, then a linear fade. Both scale with exposure. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Flash") float FullBlindSeconds = 2.5f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Flash") float FadeSeconds = 1.5f;
    /** Overlapping flashes extend the hold, never past this many seconds from now. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Flash") float MaxBlindSeconds = 5.f;
    /** Ringing and world-audio ducking. Scales with distance only; cover reduces it by OccludedDeafenScale. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Flash") float DeafenSeconds = 3.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Flash") float OccludedDeafenScale = .5f;
    /** Exposure below this is ignored. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Flash") float MinimumExposure = .05f;
    /** The bang breaks fragile destructibles in reach as an HE blast would: those a thrown grenade breaks through
     *  (GrenadeBreakDamage up to DestructionMaxGrenadeBreakDamage: glass 5, thin wood and plaster walls 10). Zero damage disables it. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Flash") float DestructionDamage = 200.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Flash") float DestructionRadiusCm = 762.f;
    /** Zero: every destructible in reach. Above zero: only those whose GrenadeBreakDamage is at most this. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Flash") float DestructionMaxGrenadeBreakDamage = 10.f;
};

/** Immutable cloud parameters. Distances are centimetres; rates are equation coefficients. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeSmokeRules
{
    GENERATED_BODY()
    /** True preserves Retake's landing gate. False activates at fuse expiry, including while airborne. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke") bool bWaitForLanding = true;
    /** Opt into authoritative puff birth origins and the matching Niagara emission-history interface. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke") bool bTrackEmissionOrigins = false;
    /** Tracked emitters begin with zero velocity; this is their world-space falling acceleration. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke", meta=(ClampMin="0", ClampMax="20000")) float EmitterGravityCmPerSecondSquared = 0;
    /** Zero leaves speed uncapped. Tracked emitters may set a source-specific falling-speed limit. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke", meta=(ClampMin="0", ClampMax="50000")) float EmitterMaximumSpeedCmPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke", meta=(ClampMin="0.1", ClampMax="100")) float EmitterCollisionRadiusCm = 3.81f;
    /** Six preserves the original Retake burst. Other counts require tracked emission and its Niagara interface. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke", meta=(ClampMin="0", ClampMax="512")) int32 InitialPuffCount = 6;
    /** Keep the cloud intact when hit by bullets/blasts. False preserves Retake's interactive smoke. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke") bool bDisableDisruptions = false;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke") float EmissionSeconds = 30.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke") float PuffLifetimeSeconds = 10.f;
    /** Sustained replacement rate; tracked systems use InitialPuffCount for the opening burst. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke") float PuffsPerSecond = 2.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke") float InitialRadiusCm = 150.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke") float GrowthRate = .12f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke") float DriftSpeedCmPerSecond = 45.72f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke") float RiseSpeedCmPerSecond = 7.62f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke") float FadeInSeconds = .15f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke") float FadeDelaySeconds = 5.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke") float MaximumOpacity = 1.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke", meta=(ClampMin="0.1", ClampMax="200")) float BulletHoleRadiusCm = 20.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke", meta=(ClampMin="0", ClampMax="10")) float BulletHoleHoldSeconds = .1f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke", meta=(ClampMin="0.01", ClampMax="30")) float BulletHoleCloseSeconds = 1.4f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke", meta=(ClampMin="0", ClampMax="10")) float BlastHoleHoldSeconds = .25f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke", meta=(ClampMin="0.01", ClampMax="30")) float BlastHoleCloseSeconds = 2.75f;
    /** Activation breaks fragile destructibles in reach as an HE blast would: those a thrown grenade breaks through
     *  (GrenadeBreakDamage up to DestructionMaxGrenadeBreakDamage: glass 5, thin wood and plaster walls 10). Zero damage disables it. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke") float DestructionDamage = 200.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke") float DestructionRadiusCm = 762.f;
    /** Zero: every destructible in reach. Above zero: only those whose GrenadeBreakDamage is at most this. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke") float DestructionMaxGrenadeBreakDamage = 10.f;
    static constexpr int32 MaximumTrackedPuffs = 512;
    bool ValidateEmission(FString& Error) const;
    bool ValidateEmissionPresentation(const UNiagaraSystem* System,FString& Error) const;
};

/** Long/short speed and charge endpoints remain in Projectile; cooking is explicitly independent. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeGrenadeRules
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Grenades") bool Enabled = false;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Grenades") ERetakeGrenadeEffect Effect = ERetakeGrenadeEffect::Explosion;
    /** Long throw: holding starts the HeldOvercookSeconds deadline. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Grenades") bool CookWhileHeld = false;
    /** Long throw: charge shortens the flight fuse down to Projectile.MinLifeSeconds. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Grenades") bool ChargeAffectsFuse = false;
    /** Short throw counterpart of CookWhileHeld (Competitive: CKR long throw, Spearhead short throw). */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Grenades") bool ShortThrowCookWhileHeld = false;
    /** Short throw counterpart of ChargeAffectsFuse, down to Projectile.ShortThrowMinLifeSeconds. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Grenades") bool ShortThrowChargeAffectsFuse = false;
    /** Both throws add the positive projection of the thrower's release-time velocity along the aim direction.
     *  Backward motion cannot weaken the throw; sideways motion adds no drift. Forced drops never inherit it. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Grenades") bool AddOwnerVelocity = false;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Grenades") float ReleasePhaseSeconds = .1f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Grenades") float GravityCmPerSecondSquared = 1219.2f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Grenades") float StopSpeedCmPerSecond = 76.2f;
    /** Exponential velocity decay while sliding on a floor, independent of collision callback rate. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Grenades", meta=(ClampMin="0", ClampMax="100")) float GroundDragRatePerSecond = 1.8313439f;
    /** Damage from the flying grenade striking a head; independent of its later explosion. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Grenades", meta=(ClampMin="0", ClampMax="1000000")) float ContactHeadDamage = 10.f;
    /** Damage from any other body contact. Zero disables that contact damage, but still bounces. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Grenades", meta=(ClampMin="0", ClampMax="1000000")) float ContactBodyDamage = 5.f;
    /** Damage from the flying/bouncing grenade striking a destructible (e.g. glass); independent of pawn contact and its later blast. Zero disables it, but still bounces. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Grenades", meta=(ClampMin="0", ClampMax="1000000")) float ContactDestructibleDamage = 10.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke") FRetakeSmokeRules Smoke;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Fire") FRetakeFireRules Fire;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Flash") FRetakeFlashRules Flash;
    /** The activation blast of this effect against destructibles. Explosives use their Projectile explosion instead. */
    FRetakeGrenadeDestructionBlast ActivationDestruction() const
    {
        switch (Effect)
        {
            case ERetakeGrenadeEffect::Incendiary: return {Fire.DestructionDamage,Fire.DestructionRadiusCm,Fire.DestructionMaxGrenadeBreakDamage};
            case ERetakeGrenadeEffect::Smoke: return {Smoke.DestructionDamage,Smoke.DestructionRadiusCm,Smoke.DestructionMaxGrenadeBreakDamage};
            case ERetakeGrenadeEffect::Flash: return {Flash.DestructionDamage,Flash.DestructionRadiusCm,Flash.DestructionMaxGrenadeBreakDamage};
            default: return {};
        }
    }
    bool CooksWhileHeld(bool bShortThrow) const { return bShortThrow ? ShortThrowCookWhileHeld : CookWhileHeld; }
    bool ChargeShortensFuse(bool bShortThrow) const { return bShortThrow ? ShortThrowChargeAffectsFuse : ChargeAffectsFuse; }
};

UENUM(BlueprintType)
enum class ERetakeWeaponAction : uint8 { None, Hitscan, Pellets, Melee, Rocket, Grenade };

UENUM(BlueprintType)
enum class ERetakeReloadPolicy : uint8 { None, Magazine, PerRound };

/** T/V: per-shot displacement impulses (reference MOH viewkick). Velocity: per-shot angular velocity brought back by a
 *  centring acceleration (reference CoD4 BG_WeaponFireRecoil and CG_KickAngles). */
UENUM(BlueprintType)
enum class ERetakeViewKickMode : uint8 { None, T, V, Velocity };

/** All distances are centimetres; angles are degrees. No live accuracy state belongs here. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponSpreadSettings
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Accuracy", meta=(ClampMin="0", ClampMax="89")) float MinDegrees = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Accuracy", meta=(ClampMin="0", ClampMax="89")) float MaxDegrees = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Accuracy", meta=(ClampMin="0")) float PerShotDegrees = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Accuracy", meta=(ClampMin="0")) float RecoveryDegreesPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Accuracy", meta=(ClampMin="0")) float MovingMultiplier = 1;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Accuracy", meta=(ClampMin="0")) float CrouchedMultiplier = 1;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Accuracy", meta=(ClampMin="0")) float ProneMultiplier = 1;
    /** Independent Gaussian deviations at ReferenceRangeCm, not a bounded angular cone. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Accuracy") bool UseClassicDistribution = false;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Accuracy") float ReferenceRangeCm = 7620;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Accuracy") float StandingHorizontalCm = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Accuracy") float StandingVerticalCm = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Accuracy") float MovingHorizontalCm = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Accuracy") float MovingVerticalCm = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Accuracy") float FireSpreadAdd = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Accuracy") float FireSpreadRecoveryPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Accuracy") float FireSpreadCap = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Accuracy") float FireSpreadResetSeconds = 0;
};

USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponDamageSettings
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Damage", meta=(ClampMin="0")) float BaseDamage = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Damage", meta=(ClampMin="0")) float HeadMultiplier = 1;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Damage", meta=(ClampMin="0")) float BodyMultiplier = 1;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Damage", meta=(ClampMin="0")) float LimbMultiplier = 1;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Damage") bool UseDetailedHitLocations = false;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Damage") float NeckMultiplier = 2;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Damage") float MidTorsoMultiplier = .95f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Damage") float LowerTorsoMultiplier = .9f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Damage") float PelvisMultiplier = .85f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Damage") float UpperLimbMultiplier = .8f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Damage") float LowerLimbMultiplier = .6f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Damage") float ExtremityMultiplier = .5f;
    /** Bullet range falloff: BaseDamage up to FullDamageRangeCm, linear to MinimumDamage at MinimumDamageRangeCm, MinimumDamage beyond.
     *  Disabled while MinimumDamageRangeCm is not beyond FullDamageRangeCm (the default). */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Damage", meta=(ClampMin="0")) float MinimumDamage = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Damage", meta=(ClampMin="0", Units="cm")) float FullDamageRangeCm = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Damage", meta=(ClampMin="0", Units="cm")) float MinimumDamageRangeCm = 0;
    float MaximumMultiplier() const;
    bool HasRangeFalloff() const { return MinimumDamageRangeCm>FullDamageRangeCm && MinimumDamage!=BaseDamage; }
    /** Largest base damage at any distance; the ledger ceiling for a direct hit. */
    float MaximumBaseDamage() const { return HasRangeFalloff() ? FMath::Max(BaseDamage,MinimumDamage) : BaseDamage; }
};

USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponActionSettings
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions") ERetakeWeaponAction Type = ERetakeWeaponAction::None;
    /** Selectable trigger behaviour; the owner's selected mode lives on the weapon instance (AWeapon::GetFireMode). */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions") FRetakeFireModeSettings FireModes;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions", meta=(ClampMin="0")) float IntervalSeconds = 0.1f;
    /** Grenades debit this on arming, not again on release. Melee must use zero. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions", meta=(ClampMin="0")) int32 AmmoCost = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions") FRetakeWeaponDamageSettings Damage;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions", meta=(ClampMin="0", Units="cm")) float RangeCm = 10000;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions", meta=(ClampMin="1", ClampMax="64")) int32 PelletCount = 1;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions") FRetakeWeaponSpreadSettings Spread;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions", meta=(ClampMin="0")) float ImpactDelaySeconds = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions") float MaxFireMovementMultiplier = 1;
    /**
     * Bullet strength against the preset's penetration table: 1 pistol/SMG, 2 rifle/shotgun, 3 sniper/LMG.
     * Scales each surface's thickest crossing and gates surfaces by MinimumPower. 0 never penetrates.
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Penetration", meta=(ClampMin="0", ClampMax="10")) float PenetrationPower = 2;
};

USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponReloadSettings
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Reload") ERetakeReloadPolicy Policy = ERetakeReloadPolicy::None;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Reload") bool bCanReloadPartiallyUsedMagazine = true;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Reload", meta=(ClampMin="0")) float CommitSeconds = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Reload", meta=(ClampMin="0")) float DurationSeconds = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Reload", meta=(ClampMin="0")) float PerRoundStartSeconds = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Reload", meta=(ClampMin="0")) float PerRoundInsertSeconds = 0;
    /** Seconds before insertion completion at which ammo is granted. Zero preserves cycle-end grants; a lead equal to insertion duration grants at its start. Animation still completes. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Reload", meta=(ClampMin="0")) float PerRoundCommitLeadSeconds = 0.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Reload", meta=(ClampMin="0")) float PerRoundEndSeconds = 0;
};

/** One cosmetic reload sound on the authoritative phase clock; never grants ammunition. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponTimedAudioCue
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio", meta=(ClampMin="0", Units="s")) float Seconds = 0.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio", meta=(AssetBundles="Game")) TSoftObjectPtr<URetakeAudioCueDefinition> Cue;
};

/** Opt-in reload audio schedule. Empty/disabled retains the existing action/commit cue behavior. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponReloadAudio
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio") bool bAuthored = false;
    /** Magazine reload or the opening stage of a per-round reload. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio") TArray<FRetakeWeaponTimedAudioCue> Start;
    /** Repeated per-round insertion, timed from its stage start rather than its ammo grant. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio") TArray<FRetakeWeaponTimedAudioCue> Insert;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio") TArray<FRetakeWeaponTimedAudioCue> End;
    static constexpr int32 MaximumEventsPerPhase = 16;
    TConstArrayView<FRetakeWeaponTimedAudioCue> Events(int32 Stage) const
    { return Stage==0 ? MakeArrayView(Start) : Stage==1 ? MakeArrayView(Insert) : Stage==2 ? MakeArrayView(End) : TConstArrayView<FRetakeWeaponTimedAudioCue>(); }
};

USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponEquipSettings
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Equip", meta=(ClampMin="0")) float AttachSeconds = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Equip", meta=(ClampMin="0")) float DurationSeconds = 0;
};

USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponPutAwaySettings
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions", meta=(ClampMin="0")) float CommitSeconds = .3f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions", meta=(ClampMin="0")) float DurationSeconds = .4f;
};

/** Source-inspired camera presentation, deliberately separate from server ballistic accuracy. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponViewKickSettings
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|ViewKick") ERetakeViewKickMode Mode = ERetakeViewKickMode::None;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|ViewKick") float PitchMinDegrees = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|ViewKick") float PitchMaxDegrees = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|ViewKick") float YawMinDegrees = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|ViewKick") float YawMaxDegrees = 0;
    /** Fraction-per-second recovery coefficient, not an angular velocity. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|ViewKick", meta=(ClampMin="0")) float RecenterSpeed = 0;
    /** Community-engine recovery defaults; retain provisional provenance until retail-tested. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|ViewKick", meta=(ClampMin="0")) float MinDecayDegreesPerSecond = 12;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|ViewKick", meta=(ClampMin="0")) float MaxDecayDegreesPerSecond = 25;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|ViewKick", meta=(ClampMin="0")) float PitchCapDegrees = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|ViewKick", meta=(ClampMin="0")) float YawCapDegrees = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|ViewKick", meta=(ClampMin="0")) float ThresholdDegrees = 0;
    /** Velocity mode only (the fields above are T/V only). Per-shot angular velocity endpoints in the reference weapon
     *  file's sign, pitch up and yaw left positive; either order, the impulse is uniform between them. Each shot assigns
     *  the velocity rather than adding to it. The ADS endpoints apply only at full aim. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|ViewKick") float HipPitchMinDegreesPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|ViewKick") float HipPitchMaxDegreesPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|ViewKick") float HipYawMinDegreesPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|ViewKick") float HipYawMaxDegreesPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|ViewKick") float AdsPitchMinDegreesPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|ViewKick") float AdsPitchMaxDegreesPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|ViewKick") float AdsYawMinDegreesPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|ViewKick") float AdsYawMaxDegreesPerSecond = 0;
    /** Velocity mode only. Acceleration toward rest; the ADS value applies above half aim. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|ViewKick", meta=(ClampMin="0")) float HipCenterDegreesPerSecondSquared = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|ViewKick", meta=(ClampMin="0")) float AdsCenterDegreesPerSecondSquared = 0;
    /** WEAPON-11 BurstKickScale: the per-shot impulse ranges times Scale; caps, thresholds, decay and centring unchanged. */
    FRetakeWeaponViewKickSettings WithImpulseScale(float Scale) const;
};

/** First-person weapon kick (reference CoD4 gun recoil): each shot adds angular velocity to the held weapon, which a
 *  restoring acceleration and viscous/static decay bring back. Presentation only: the camera and shots never follow it. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponGunKickSettings
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Recoil") bool Enabled = false;
    /** Per-shot angular velocity endpoints in the reference weapon file's sign, pitch down and yaw left positive; either
     *  order. Hip endpoints apply at zero aim, ADS endpoints at any aim. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Recoil") float HipPitchMinDegreesPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Recoil") float HipPitchMaxDegreesPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Recoil") float HipYawMinDegreesPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Recoil") float HipYawMaxDegreesPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Recoil") float AdsPitchMinDegreesPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Recoil") float AdsPitchMaxDegreesPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Recoil") float AdsYawMinDegreesPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Recoil") float AdsYawMaxDegreesPerSecond = 0;
    /** Integration settings, blended from hip to ADS by the aim fraction. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Recoil", meta=(ClampMin="0")) float HipAccelerationDegreesPerSecondSquared = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Recoil", meta=(ClampMin="0")) float HipSpeedMaxDegreesPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Recoil", meta=(ClampMin="0")) float HipSpeedDecayPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Recoil", meta=(ClampMin="0")) float HipStaticDecayDegreesPerSecondSquared = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Recoil", meta=(ClampMin="0")) float AdsAccelerationDegreesPerSecondSquared = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Recoil", meta=(ClampMin="0")) float AdsSpeedMaxDegreesPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Recoil", meta=(ClampMin="0")) float AdsSpeedDecayPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Recoil", meta=(ClampMin="0")) float AdsStaticDecayDegreesPerSecondSquared = 0;
    /** Offset caps (reference gunMaxPitch, gunMaxYaw). */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Recoil", meta=(ClampMin="0", ClampMax="90")) float MaxPitchDegrees = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Recoil", meta=(ClampMin="0", ClampMax="90")) float MaxYawDegrees = 0;
    /** WEAPON-11 BurstKickScale: the per-shot velocity endpoints times Scale; integration and caps unchanged. */
    FRetakeWeaponGunKickSettings WithImpulseScale(float Scale) const;
};

/** Optional periodic rocket steering/deceleration followed by a falling phase. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeProjectileDriftSettings
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile|Drift") bool bEnabled=false;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile|Drift", meta=(ClampMin="0", Units="s")) float StartDelaySeconds=0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile|Drift", meta=(ClampMin="0.01", ClampMax="1", Units="s")) float UpdateIntervalSeconds=.05f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile|Drift", meta=(ClampMin="0", ClampMax="90", Units="deg")) float InitialAngleDegrees=0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile|Drift", meta=(ClampMin="0", ClampMax="30", Units="deg")) float AngleGrowthPerUpdate=0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile|Drift", meta=(ClampMin="0.01", ClampMax="1")) float SpeedRetentionPerUpdate=1;
    /** Steering ends after speed falls below this threshold; subsequent updates apply FallingGravity. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile|Drift", meta=(ClampMin="0")) float MinimumSteeringSpeedCmPerSecond=0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile|Drift", meta=(ClampMin="0", ClampMax="20000")) float FallingGravityCmPerSecondSquared=0;
    /** Per-axis speed bound; zero leaves it uncapped. Independent of the ordinary magnitude limit. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile|Drift", meta=(ClampMin="0")) float MaximumAxisSpeedCmPerSecond=0;
    bool Validate(FString& Error) const;
};

/** Converted gameplay settings, not a raw TIKI dictionary. Fuse flags are not durations. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponProjectileSettings
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile", meta=(ClampMin="0")) float DirectDamage = 0;
    /** Maximum-charge launch speed; MinimumSpeed supplies the other endpoint. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile", meta=(ClampMin="0")) float InitialSpeedCmPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile", meta=(ClampMin="0")) float MinimumSpeedCmPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile", meta=(ClampMin="0")) float MaxSpeedCmPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile", meta=(ClampMin="0")) float GravityScale = 1;
    /** Explicit downward acceleration, independent of player/world presets and GravityScale when enabled. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile") bool bOverrideGravity = false;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile", meta=(ClampMin="0", ClampMax="20000", Units="cm/s^2")) float GravityCmPerSecondSquared = 0;
    /** Ignore the launching pawn for the entire flight. Splash/self-damage rules remain independent. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile") bool bIgnoreInstigatorCollision = false;
    /** Flight/launch queries pass through other native projectiles, without changing weapon traces. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile") bool bIgnoreOtherProjectiles = false;
    /** False lets character movement pass through the projectile; projectile hits and weapon traces remain independent. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile") bool bBlockCharacterMovement = true;
    /** Zero preserves indestructible projectiles. Positive health accepts native projectile/blast damage and optional bullets; depletion detonates. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile", meta=(ClampMin="0", ClampMax="1000000")) float DamageableHealth=0;
    /** Requires positive health. Bullets/pellets, including the instigator's, can hit the projectile; melee stays independent. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile") bool bCanBeShot=false;
    /** Zero disables the forward glass probe. Above zero, faster projectiles shatter eligible glass without contact damage. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile", meta=(ClampMin="0", ClampMax="100000")) float GlassBreakMinimumSpeedCmPerSecond=0;
    /** Linear look-ahead for the glass probe. The native probe cadence is bounded independently of frame rate. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile", meta=(ClampMin="0.01", ClampMax="1")) float GlassBreakLookAheadSeconds=0.1f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile|Drift") FRetakeProjectileDriftSettings Drift;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile", meta=(ClampMin="0")) float CollisionRadiusCm = 2;
    /** Use an axis-aligned box instead of the default sphere. Visual rotation never rotates these bounds. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile") bool bAxisAlignedBoxCollision = false;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile", meta=(ClampMin="0", Units="cm")) FVector CollisionBoxHalfExtentCm = FVector::ZeroVector;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile") bool bShouldBounce = false;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile", meta=(ClampMin="0", ClampMax="1")) float Bounciness = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile", meta=(ClampMin="0", ClampMax="1")) float Friction = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile") bool bDetonateOnImpact = false;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile", meta=(ClampMin="0")) float LifetimeSeconds = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile", meta=(ClampMin="0")) float MinLifeSeconds = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile", meta=(ClampMin="0")) float HeldOvercookSeconds = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile", meta=(ClampMin="0")) float MinChargeSeconds = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile", meta=(ClampMin="0")) float MaxChargeSeconds = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Projectile") float LaunchVelocityZCmPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Short Throw", meta=(ClampMin="0")) float ShortThrowInitialSpeedCmPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Short Throw", meta=(ClampMin="0")) float ShortThrowMinimumSpeedCmPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Short Throw", meta=(ClampMin="0")) float ShortThrowMinChargeSeconds = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Short Throw", meta=(ClampMin="0")) float ShortThrowMaxChargeSeconds = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Short Throw", meta=(ClampMin="0")) float ShortThrowMinLifeSeconds = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Short Throw") float ShortThrowLaunchVelocityZCmPerSecond = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Explosion", meta=(ClampMin="0")) float ExplosionDamage = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Explosion", meta=(ClampMin="0")) float ExplosionMinDamage = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Explosion", meta=(ClampMin="0")) float ExplosionInnerRadiusCm = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Explosion", meta=(ClampMin="0")) float ExplosionOuterRadiusCm = 0;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Explosion", meta=(ClampMin="0")) float ExplosionFalloff = 1;
    /** Reference centroid/bounds-adjusted linear falloff; generic inner/minimum/exponent settings apply when false. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Explosion") bool UseClassicExplosionDamage = false;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Explosion", meta=(ClampMin="0", ClampMax="1")) float ExplosionSelfDamageMultiplier = 1;
    /** An impact's directly struck damage target receives direct damage only; surrounding targets still receive splash. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Explosion") bool bExcludeDirectTargetFromExplosion = false;
    /** False excludes projectile bodies from this explosion's cover queries, without excluding them as damage targets. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Explosion") bool bProjectilesBlockExplosion = true;
    bool ValidateCollision(FString& Error) const;
    bool ValidateDrift(FString& Error) const;
};

USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeGrenadeAnimationSet
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Grenades", meta=(AssetBundles="Client")) TSoftObjectPtr<UAnimSequence> PrepareLong;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Grenades", meta=(AssetBundles="Client")) TSoftObjectPtr<UAnimSequence> PrepareShort;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Grenades", meta=(AssetBundles="Client")) TSoftObjectPtr<UAnimSequence> ReleaseLong;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Grenades", meta=(AssetBundles="Client")) TSoftObjectPtr<UAnimSequence> ReleaseShort;
};

/** One paired bash motion. The accepted shot selects the same bounded entry for both views and the recorded hit rig. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeBashAnimationVariant
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Animation", meta=(AssetBundles="Client")) TSoftObjectPtr<UAnimSequence> FirstPersonAnimation;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Animation", meta=(AssetBundles="Client")) TSoftObjectPtr<UAnimSequence> ThirdPersonAnimation;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions", meta=(ClampMin="0.01")) float DurationSeconds=.75f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Actions", meta=(ClampMin="0")) float ImpactDelaySeconds=.2f;
};

/** Optional visual prop for the existing timed bash action. No collision, attack or replicated state of its own. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeBashPropSettings
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Meshes", meta=(AssetBundles="Client")) TSoftObjectPtr<UStaticMesh> Mesh;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Meshes") FName Bone=TEXT("hand_l");
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Meshes") FTransform FirstPersonOffset;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Meshes") FTransform ThirdPersonOffset;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Animation", meta=(ClampMin="0",ClampMax="1")) float VisibleStart=.1f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Animation", meta=(ClampMin="0",ClampMax="1")) float VisibleEnd=.85f;
};

/** A team's mesh variant of one weapon. Gameplay, sockets and animation are shared; only the look changes. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponTeamMeshes
{
    GENERATED_BODY()
    /** Faction index, the same one that selects a team armory preset. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Presentation") int32 Team = INDEX_NONE;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Presentation", meta=(AssetBundles="Client")) TSoftObjectPtr<USkeletalMesh> WorldMesh;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Presentation", meta=(AssetBundles="Client")) TSoftObjectPtr<USkeletalMesh> FirstPersonMesh;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Presentation", meta=(AssetBundles="Client")) TSoftObjectPtr<USkeletalMesh> ProjectileMesh;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Presentation", meta=(AssetBundles="Client")) TSoftObjectPtr<UStaticMesh> ProjectileStaticMesh;
};

USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponPresentationSettings
{
    GENERATED_BODY()
    /** None resolves to the primary-back or secondary-hip socket for the definition's slot. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Presentation") FName HolsterSocket;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Presentation") FTransform HolsterOffset;
    /** Loose reload props (for example a stripper clip) that must not appear on a carried weapon. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Presentation") TArray<FName> HolsterHiddenBones;
    /** Weapon material slots hidden outside authored visibility windows, for example loose reload cartridges.
     * Applies to both weapon views and every LOD; empty preserves the previous all-visible default. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Meshes") TArray<FName> DefaultHiddenMaterialSlots;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Grenades") FRetakeGrenadeAnimationSet FirstPersonGrenade;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Grenades") FRetakeGrenadeAnimationSet ThirdPersonGrenade;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Grenades") FRetakeGrenadeAnimationSet WeaponGrenade;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Presentation", meta=(AssetBundles="Client")) TSoftObjectPtr<URetakeWeaponAnimationProfile> CharacterProfile;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Presentation", meta=(AssetBundles="Client")) TSoftObjectPtr<USkeletalMesh> WorldMesh;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Presentation", meta=(AssetBundles="Client")) TSoftObjectPtr<USkeletalMesh> FirstPersonMesh;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Presentation", meta=(AssetBundles="Client")) TSoftObjectPtr<USkeletalMesh> ProjectileMesh;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Presentation", meta=(AssetBundles="Client")) TSoftObjectPtr<UStaticMesh> ProjectileStaticMesh;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Presentation") FName GripSocket = TEXT("GripPoint");
    /** Attachment on the third-person body. None means GripSocket. First person can follow the animated ik_hand_gun bone while third-person clips that do not animate it keep the hand socket. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Presentation") FName ThirdPersonGripSocket;
    /** Parts fitted on both view meshes (handguard, sights, magazine, optics). Loadout-selectable attachments will build on this list. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Attachments") TArray<FRetakeWeaponAttachment> Attachments;
    /** WorldMesh (and every TeamMeshes.WorldMesh) already contains the Attachments' geometry: a merged third-person mesh such as
     *  SKM_<Weapon>_TP (Docs/Content/Weapons/ThirdPersonMeshes.md). Third-person views then fit no part components. The parts
     *  still define sockets, bounds and aim axes, so the world mesh keeps every part socket. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Attachments") bool bWorldMeshIncludesAttachments = false;
    /** The parts to fit as components on one view mesh: none on the world mesh when it already contains them. */
    const TArray<FRetakeWeaponAttachment>& GetFittedAttachments(bool bFirstPerson) const
    {
        static const TArray<FRetakeWeaponAttachment> None;
        return !bFirstPerson && bWorldMeshIncludesAttachments ? None : Attachments;
    }
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Presentation") FName MuzzleSocket = TEXT("Muzzle");
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Clearance") FRetakeWeaponClearanceProfile Clearance;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Presentation") FTransform WorldOffset;
    /** Third person, while an action clip's RetakeWeaponOnGunBone curve is up: the weapon relative to the animated ik_hand_gun
     *  bone, so the rifle follows whichever hand holds it (Mohaa moves it to the left hand for reloads and rechambers). */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Presentation") FTransform ThirdPersonGunBoneOffset;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Presentation") FTransform FirstPersonOffset;
    /** Whole arms-and-weapon hip-fire framing relative to the shared rig. Translation uses camera axes (X forward, Y right, Z up); rotation adjusts the rig orientation about its origin. Identity preserves existing framing. Authored ADS is a separate absolute target. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Presentation") FTransform FirstPersonRigOffset = FTransform::Identity;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation|FirstPerson") FRetakeFirstPersonMotionSettings FirstPersonMotion;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation|FirstPerson") FTransform FirstPersonSprintRigOffset=FTransform::Identity;
    /** Rig offset blended in with an authored first-person run loop (profile RunAnim, View.AuthoredRunLoops), camera axes like the sprint offset.
     *  A loop authored for another viewmodel can hold the weapon far forward; this pulls the whole rig back so the support hand can reach it. Fades with aim and sprint. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation|FirstPerson") FTransform FirstPersonRunRigOffset=FTransform::Identity;
    /** Shared by gameplay presentation and the first-person outfit preview; never moves the camera or Mesh3P. */
    FTransform ResolveFirstPersonRigTransform(const FTransform& SharedRig) const;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation", meta=(AssetBundles="Client,Server")) TSoftObjectPtr<UAnimSequence> IdleAnimation;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation", meta=(AssetBundles="Client")) TSoftObjectPtr<UAnimSequence> FireAnimation;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation", meta=(AssetBundles="Client")) TSoftObjectPtr<UAnimSequence> ReloadAnimation;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation", meta=(AssetBundles="Client")) TSoftObjectPtr<UAnimSequence> ReloadInsertAnimation;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation", meta=(AssetBundles="Client")) TSoftObjectPtr<UAnimSequence> ReloadEndAnimation;
    /** Weapon-mesh reload for the first-person weapon only, timed to the first-person arms' reload when that differs from
     *  third person (the pistols' first-person set, Docs/Content/Weapons/PistolFirstPersonSet.md). Empty plays
     *  ReloadAnimation in both views. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation", meta=(AssetBundles="Client")) TSoftObjectPtr<UAnimSequence> FirstPersonReloadAnimation;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation", meta=(AssetBundles="Client")) TSoftObjectPtr<UAnimSequence> FirstPersonBashAnimation;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation", meta=(AssetBundles="Client")) TSoftObjectPtr<UAnimSequence> ThirdPersonBashAnimation;
    /** Optional paired alternatives, including index zero. Empty retains the legacy single clip and action timings. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Animation") TArray<FRetakeBashAnimationVariant> BashVariants;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Animation") FRetakeBashPropSettings BashProp;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation", meta=(AssetBundles="Client")) TSoftObjectPtr<UAnimSequence> FirstPersonReloadStart;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation", meta=(AssetBundles="Client")) TSoftObjectPtr<UAnimSequence> FirstPersonReloadInsert;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation", meta=(AssetBundles="Client")) TSoftObjectPtr<UAnimSequence> FirstPersonReloadEnd;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation", meta=(AssetBundles="Client")) TSoftObjectPtr<UAnimSequence> ThirdPersonReloadStart;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation", meta=(AssetBundles="Client")) TSoftObjectPtr<UAnimSequence> ThirdPersonReloadInsert;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Animation", meta=(AssetBundles="Client")) TSoftObjectPtr<UAnimSequence> ThirdPersonReloadEnd;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio", meta=(AssetBundles="Client")) TSoftObjectPtr<URetakeAudioCueDefinition> PrimaryFireCue;
    /** Optional mechanical cycle sound. Triggered only by Retake Weapon Rechamber in FireAnimation. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio", meta=(AssetBundles="Client")) TSoftObjectPtr<USoundBase> RechamberSound;
    /** Short-range spatial policy for remote bolt/foley audio; independent of the gunshot's range. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio", meta=(AssetBundles="Client")) TSoftObjectPtr<USoundAttenuation> RechamberAttenuation;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio", meta=(AssetBundles="Client")) TSoftObjectPtr<URetakeAudioCueDefinition> SecondaryFireCue;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio", meta=(AssetBundles="Client")) TSoftObjectPtr<URetakeAudioCueDefinition> DryFireCue;
    /**
     * Fire-selector switch, heard by the switching player only (WEAPON-11). CoD4 has no selector; its rifle alternate-mode switch
     * aliases (weap_m4m16_switch_grenade/_back) play misc/pu_weapon01.wav on the viewmodel. Optional: a weapon without it switches silently.
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio", meta=(AssetBundles="Client")) TSoftObjectPtr<URetakeAudioCueDefinition> FireSelectorCue;
    /** WEAPON-11: an optional lever part turned to the selected fire mode (FRetakeFireSelectorPresentation). Empty: no moving lever. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Fire Modes") FRetakeFireSelectorPresentation FireSelector;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio", meta=(AssetBundles="Client")) TSoftObjectPtr<URetakeAudioCueDefinition> ReloadCue;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio", meta=(AssetBundles="Client")) TSoftObjectPtr<URetakeAudioCueDefinition> ReloadInsertCue;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio", meta=(AssetBundles="Client")) TSoftObjectPtr<URetakeAudioCueDefinition> ReloadEndCue;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio", meta=(AssetBundles="Client")) TSoftObjectPtr<URetakeAudioCueDefinition> EquipCue;
    /** Played when the weapon is put away: on a switch at its start (the draw sound then follows the attach), and on a holster. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio", meta=(AssetBundles="Client")) TSoftObjectPtr<URetakeAudioCueDefinition> HolsterCue;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio", meta=(AssetBundles="Client")) TSoftObjectPtr<URetakeAudioCueDefinition> PinCue;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio", meta=(AssetBundles="Client")) TSoftObjectPtr<URetakeAudioCueDefinition> ThrowCue;
    /** First-person cook warning while a throw that cooks is held (Spearhead's grenade timer). Its one recording ends at the
     *  in-hand overcook: playback starts HeldOvercookSeconds minus its length after arming and stops on release. Null plays none. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio", meta=(AssetBundles="Client")) TSoftObjectPtr<URetakeAudioCueDefinition> CookTimerCue;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio", meta=(AssetBundles="Client")) TSoftObjectPtr<URetakeAudioCueDefinition> ExplosionCue;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio", meta=(AssetBundles="Client")) TSoftObjectPtr<USoundAttenuation> Attenuation;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Audio", meta=(AssetBundles="Client")) TSoftObjectPtr<USoundConcurrency> Concurrency;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Effects", meta=(AssetBundles="Client")) TSoftObjectPtr<UNiagaraSystem> MuzzleEffect;
    /** Which shared surface response this weapon's hits and bounces use (DA_RetakeSurfaceEffects). Bullet also leaves marks. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Effects") ERetakeSurfaceImpactFamily SurfaceImpactFamily = ERetakeSurfaceImpactFamily::None;
    /** Cosmetic muzzle-to-impact streak for hitscan/pellet shots; null draws none. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Effects", meta=(AssetBundles="Client")) TSoftObjectPtr<URetakeTracerProfile> Tracer;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Effects", meta=(AssetBundles="Client")) TSoftObjectPtr<UNiagaraSystem> ShellEjectEffect;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Effects", meta=(AssetBundles="Client")) TSoftObjectPtr<UStaticMesh> ShellEjectMesh;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Effects") FName EjectSocket = TEXT("tag_eject");
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Effects", meta=(AssetBundles="Client")) TSoftObjectPtr<UNiagaraSystem> ExplosionEffect;
    /** Used instead of ExplosionEffect when the grenade detonates without a floor beneath it; falls back to ExplosionEffect when unset. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Effects", meta=(AssetBundles="Client")) TSoftObjectPtr<UNiagaraSystem> ExplosionAirEffect;
    /** Uniform spawn scale applied only to ExplosionAirEffect; the source system simulates in local space, so this shrinks or grows it cleanly. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Effects", meta=(ClampMin="0.1", ClampMax="2")) float ExplosionAirEffectScale = 1.f;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke", meta=(AssetBundles="Client")) TSoftObjectPtr<UNiagaraSystem> SmokeEffect;
    /** Uniform scale applied to the smoke cloud component; purely cosmetic, independent of Grenade.Smoke's gameplay radius/growth. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke", meta=(ClampMin="0.1", ClampMax="3")) float SmokeEffectScale = 1.25f;
    /** Looped while the cloud is alive (emission + lingering puffs); stops the moment it ends. Null disables it. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke", meta=(AssetBundles="Client")) TSoftObjectPtr<USoundBase> SmokeLoopSound;
    /** Short-range falloff for SmokeLoopSound (not the long-range combat Attenuation). Assign an owned resource in a mod.
     *  The private game adapter supplies its existing builtin default separately. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Smoke", meta=(AssetBundles="Client")) TSoftObjectPtr<USoundAttenuation> SmokeLoopAttenuation;
    /** Incendiary floor fire; driven by DesiredAge with User.RadiusCm and User.DurationSeconds. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Fire", meta=(AssetBundles="Client")) TSoftObjectPtr<UNiagaraSystem> FireEffect;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Fire", meta=(AssetBundles="Client")) TSoftObjectPtr<USoundBase> FireLoopSound;
    /** Flash exposure post-process: FlashIntensity, AfterImageIntensity, DarkFlash scalars and an AfterImage texture. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Flash", meta=(AssetBundles="Client")) TSoftObjectPtr<UMaterialInterface> FlashScreenMaterial;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Weapons|Flash", meta=(AssetBundles="Client")) TSoftObjectPtr<USoundBase> FlashRingSound;
    /** Optional per-team look. The first entry whose Team matches the thrower or holder wins; empty fields keep the defaults above. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|Presentation") TArray<FRetakeWeaponTeamMeshes> TeamMeshes;
    /** Team-resolved meshes. INDEX_NONE or an unlisted team returns the defaults. */
    const FRetakeWeaponTeamMeshes* FindTeamMeshes(int32 Team) const
    { return Team==INDEX_NONE ? nullptr : TeamMeshes.FindByPredicate([Team](const FRetakeWeaponTeamMeshes& M){ return M.Team==Team; }); }
    USkeletalMesh* GetWorldMesh(int32 Team) const;
    USkeletalMesh* GetFirstPersonMesh(int32 Team) const;
    USkeletalMesh* GetProjectileMesh(int32 Team) const;
    UStaticMesh* GetProjectileStaticMesh(int32 Team) const;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|UI", meta=(AssetBundles="UI")) TSoftObjectPtr<UTexture2D> Icon;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|UI", meta=(AssetBundles="UI")) TSoftObjectPtr<UTexture2D> LoadoutHero;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|UI", meta=(AssetBundles="UI")) TSoftObjectPtr<UTexture2D> LoadoutThumbnail;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|UI") FText LoadoutCategory;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|UI", meta=(MultiLine="true")) FText LoadoutDescription;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|UI", meta=(AssetBundles="UI")) TSoftObjectPtr<UMaterialInterface> WeaponIconMaterial;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|UI", meta=(AssetBundles="UI")) TSoftObjectPtr<UMaterialInterface> ReticleMaterial;
    /** UI-domain full-screen scope. Required for Scope aim mode; no procedural fallback. */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|UI", meta=(AssetBundles="UI,Client")) TSoftObjectPtr<UMaterialInterface> ScopeMaterial;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Retake|UI") FReticleParams ReticleParams;
};

UENUM(BlueprintType)
enum class ERetakeWeaponGripStyle : uint8 { OneHand, TwoHands };

/** Authoring presets copy only grip-related presentation. Weapon identity and balance never change. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponGripPreset
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, Category="Retake|Weapons|Grips") TSoftObjectPtr<URetakeWeaponAnimationProfile> CharacterProfile;
    UPROPERTY(EditDefaultsOnly, Category="Retake|Weapons|Grips") FName GripSocket;
    UPROPERTY(EditDefaultsOnly, Category="Retake|Weapons|Grips") FName ThirdPersonGripSocket;
    UPROPERTY(EditDefaultsOnly, Category="Retake|Weapons|Grips") FTransform FirstPersonOffset;
    UPROPERTY(EditDefaultsOnly, Category="Retake|Weapons|Grips") FTransform WorldOffset;
    UPROPERTY(EditDefaultsOnly, Category="Retake|Weapons|Grips") FTransform FirstPersonRigOffset;
    UPROPERTY(EditDefaultsOnly, Category="Retake|Weapons|Grips") FTransform FirstPersonSprintRigOffset;
    UPROPERTY(EditDefaultsOnly, Category="Retake|Weapons|Grips") FTransform ThirdPersonGunBoneOffset;
    UPROPERTY(EditDefaultsOnly, Category="Retake|Weapons|Grips") FTransform AimRigTransform;
    UPROPERTY(EditDefaultsOnly, Category="Retake|Weapons|Grips") bool bAimAlignmentAuthored=false;
    UPROPERTY(EditDefaultsOnly, Category="Retake|Weapons|Grips") TSoftObjectPtr<UAnimSequence> FirstPersonBashAnimation;
    UPROPERTY(EditDefaultsOnly, Category="Retake|Weapons|Grips") TSoftObjectPtr<UAnimSequence> ThirdPersonBashAnimation;
    UPROPERTY(EditDefaultsOnly, Category="Retake|Weapons|Grips") TArray<FRetakeBashAnimationVariant> BashVariants;
    /** Matching weapon-part motion for this grip's hand reload. Empty keeps the definition's existing clip. */
    UPROPERTY(EditDefaultsOnly, Category="Retake|Weapons|Grips") TSoftObjectPtr<UAnimSequence> ReloadAnimation;
    /** First-person-only weapon reload for this grip's first-person reload. Always applied: empty plays ReloadAnimation. */
    UPROPERTY(EditDefaultsOnly, Category="Retake|Weapons|Grips") TSoftObjectPtr<UAnimSequence> FirstPersonReloadAnimation;
    UPROPERTY(EditDefaultsOnly, Category="Retake|Weapons|Grips") FRetakeBashPropSettings BashProp;
};
