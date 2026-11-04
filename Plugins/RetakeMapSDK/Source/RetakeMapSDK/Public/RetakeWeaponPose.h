// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "RetakeWeaponData.h"
#include "RetakeWeaponPose.generated.h"

class UAnimSequence;
class UAnimSequenceBase;
class USkeleton;
class URetakeWeaponSpecification;

/** Explicit double fields keep baked catalogue values exact in the saved rig asset. */
USTRUCT()
struct RETAKEMAPSDK_API FRetakePoseVector
{
    GENERATED_BODY()
    UPROPERTY() double X=0.;
    UPROPERTY() double Y=0.;
    UPROPERTY() double Z=0.;
    FRetakePoseVector()=default;
    explicit FRetakePoseVector(const FVector& V):X(V.X),Y(V.Y),Z(V.Z) {}
    FVector Get() const { return FVector(X,Y,Z); }
};
USTRUCT()
struct RETAKEMAPSDK_API FRetakePoseQuaternion
{
    GENERATED_BODY()
    UPROPERTY() double X=0.;
    UPROPERTY() double Y=0.;
    UPROPERTY() double Z=0.;
    UPROPERTY() double W=1.;
    FRetakePoseQuaternion()=default;
    explicit FRetakePoseQuaternion(const FQuat& Q):X(Q.X),Y(Q.Y),Z(Q.Z),W(Q.W) {}
    FQuat Get() const { return FQuat(X,Y,Z,W); }
};
USTRUCT()
struct RETAKEMAPSDK_API FRetakePoseBone
{
    GENERATED_BODY()
    UPROPERTY() FName Name;
    /** Catalogue index, not a mesh index. Parents always precede children. */
    UPROPERTY() int16 Parent=INDEX_NONE;
    UPROPERTY() FRetakePoseVector ReferencePosition;
    UPROPERTY() FRetakePoseQuaternion ReferenceRotation;
    UPROPERTY() FRetakePoseVector ReferenceScale=FRetakePoseVector(FVector::OneVector);
    FTransform Reference() const { return FTransform(ReferenceRotation.Get(),ReferencePosition.Get(),ReferenceScale.Get()); }
};
UENUM()
enum class ERetakeHitRigPose : uint8
{
    Stand, Crouch, StandAim, CrouchAim, StandLeanLeft, StandLeanRight, CrouchLeanLeft, CrouchLeanRight,
    /** The profile's third-person sprint pose (the graph's sprint weapon base pose), over the standing legs. */
    Sprint,
    StandMovingLeanLeft, StandMovingLeanRight, CrouchMovingLeanLeft, CrouchMovingLeanRight,
    Count UMETA(Hidden)
};

/** Action clips baked per hold (HB-04.3), in the order URetakeHitRigDefinition bakes them. */
UENUM()
enum class ERetakeHitRigTrack : uint8
{
    Reload, Equip, Holster, Drop, Bash,
    /** HB-04.3b grenade throw: the prepare and release poses of the hold's grenade weapon (FRetakeGrenadeAnimationSet). */
    GrenadePrepareLong, GrenadePrepareShort, GrenadeReleaseLong, GrenadeReleaseShort,
    /** HB-04.3b per-round reload stages (weapon definition ThirdPersonReloadStart/Insert/End). */
    ReloadStart, ReloadInsert, ReloadEnd,
    Inspect,
    BoltCycle,
    Count UMETA(Hidden)
};

/** How a baked layer combines with the posture beneath it, mirroring the graph's slot evaluation. */
UENUM()
enum class ERetakeHitRigLayer : uint8
{
    None,
    /** Absolute clip: replaces the upper-body mesh-space rotations. */
    Absolute,
    /** Local-space additive: delta * local rotation. */
    LocalAdditive,
    /** Mesh-space rotation offset: delta * mesh-space rotation. */
    MeshAdditive
};

/**
 * One upper-body action clip sampled at a fixed rate for the rig's upper-body bones (spine_01 and its descendants, the
 * graph's upper-body layer), sample-major. The main layer comes from the montage's DefaultSlot/UpperBody track and the
 * optional additive layer from UpperBodyAdditive/UpperBodyDynAdditive, each stored in its own space (see ERetakeHitRigLayer).
 */
USTRUCT()
struct RETAKEMAPSDK_API FRetakeHitRigTrack
{
    GENERATED_BODY()
    /** Authored source evaluated by the TP montage graph at the same recorded phase as this baked track. */
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") TObjectPtr<UAnimSequenceBase> SourceAnimation;
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") float Length = 0.f;
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") float SampleRate = 30.f;
    /** Simple montage layers retain the compressed source's key grid. */
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") bool bSourceKeyAligned = false;
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") float RateScale = 1.f;
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") float BlendIn = .25f;
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") float BlendOut = .25f;
    /** Full-body slot: the clip also owns the pelvis and legs (not evaluated in v1). */
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") bool bFullBody = false;
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") ERetakeHitRigLayer MainLayer = ERetakeHitRigLayer::None;
    /** Sample-major local transforms for every catalogue bone, before the graph's upper-body mask. */
    UPROPERTY() TArray<FTransform3f> MainPoses;
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") ERetakeHitRigLayer AdditiveLayer = ERetakeHitRigLayer::None;
    UPROPERTY() TArray<FTransform3f> AdditivePoses;
    /** HB-04.3b aimed variant: the montage's Aiming slot, blended over the main layer by the recorded ADS alpha (graph: RetakeAimingTrackAlpha). */
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") ERetakeHitRigLayer AimingLayer = ERetakeHitRigLayer::None;
    UPROPERTY() TArray<FTransform3f> AimingPoses;
    /**
     * HB-12 held-weapon placement: where the clip carries the weapon on ik_hand_gun (curve RetakeWeaponOnGunBone, the maximum over
     * every slot as the graph samples it), that bone's component-space transform relative to hand_r, and the curve weight, per sample.
     * Empty when the clip never moves the weapon off its hand socket.
     */
    UPROPERTY() TArray<FTransform3f> GunInHand;
    UPROPERTY() TArray<float> GunWeight;
    /** Gun-relative transforms/curves retain their dense sampling independently of pose keys. */
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") float GunSampleRate = 30.f;
    bool IsSet() const { return Length > 0.f && (!MainPoses.IsEmpty() || !AdditivePoses.IsEmpty()); }
};

/** Clearance rotations relative to each arm's parent. No torso, translation or scale tracks are retained. */
USTRUCT()
struct RETAKEMAPSDK_API FRetakeHitRigArmClearance
{
    GENERATED_BODY()
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") TArray<float> Keys;
    /** Key-major local rotations in RetakeWeaponArmIK::BoneNames order. */
    UPROPERTY() TArray<FQuat4f> Rotations;
    bool IsValid() const;
};

/** Key poses for one hold (a weapon animation profile, or unarmed): bone-local transforms in catalogue order, pose-major. */
USTRUCT()
struct RETAKEMAPSDK_API FRetakeHitRigHold
{
    GENERATED_BODY()
    /** Weapon animation profile id; None is the unarmed hold. */
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") FName ProfileId;
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") FRetakeThirdPersonAimPose Aim;
    UPROPERTY() TArray<FTransform> Poses;
    /** Non-notifying carry loops: standing, crouched, aimed, sprinting. Sampled on the shared presentation clock. */
    UPROPERTY() TArray<TObjectPtr<UAnimSequence>> CarrySequences;
    /** ERetakeHitRigTrack order. */
    UPROPERTY() TArray<FRetakeHitRigTrack> Tracks;
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") bool bContinuousReloadStages=false;
    /** Bash variants 1..7; variant zero remains Tracks[Bash] for the single-clip fallback. */
    UPROPERTY() TArray<FRetakeHitRigTrack> AdditionalBashTracks;
    /** The weapon's wall clearance is enabled (FRetakeWeaponClearanceProfile::bEnabled of the first weapon using the profile). */
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") bool bClearance = false;
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") FRetakeHitRigArmClearance ClearanceStand;
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") FRetakeHitRigArmClearance ClearanceCrouch;
    /** Support hand (HB-04A): the weapon's FABRIK socket in hand_r space; the left arm reaches it while no action plays. */
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") bool bSupportHand = false;
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") FTransform SupportInHand;
    /** Barrel convergence (HB-04A): grip and barrel axis in hand_r space, and the largest turn onto the aim. */
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") bool bConvergence = false;
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") FTransform GripInHand;
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") FVector BarrelInHand = FVector::ZeroVector;
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") float ConvergenceDegrees = 0.f;
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") float LeanElbowDownWeight = 0.f;
    /** Reference weapon geometry for the shared arms-only ready/ADS height solve. */
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") bool bWeaponHeight = false;
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") FVector MuzzleInHand = FVector::ZeroVector;
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") bool bSight = false;
    UPROPERTY(VisibleAnywhere, Category="Retake|Animation") FTransform SightInHand = FTransform::Identity;
};

/** Explicit scalar storage keeps the shared lower-body basis exact across save/cook/reload. */
USTRUCT()
struct RETAKEMAPSDK_API FRetakeWeaponPoseTransform
{
    GENERATED_BODY()
    UPROPERTY() FRetakePoseVector Position;
    UPROPERTY() FRetakePoseQuaternion Rotation;
    UPROPERTY() FRetakePoseVector Scale=FRetakePoseVector(FVector::OneVector);
    FRetakeWeaponPoseTransform()=default;
    explicit FRetakeWeaponPoseTransform(const FTransform& Value)
        : Position(Value.GetTranslation()),Rotation(Value.GetRotation()),Scale(Value.GetScale3D()) {}
    FTransform Get() const { return FTransform(Rotation.Get(),Position.Get(),Scale.Get()); }
};

/** Mesh and animation-skeleton reference spaces plus the shared lower-body basis. No private hit shapes or locomotion sources. */
USTRUCT()
struct RETAKEMAPSDK_API FRetakeWeaponPoseContract
{
    GENERATED_BODY()
    UPROPERTY() int32 FormatVersion=1;
    UPROPERTY() uint32 RecipeVersion=1;
    UPROPERTY() TArray<FRetakePoseBone> Bones;
    /** Same closed names/parents as Bones, with the actual animation Skeleton's distinct reference transforms. */
    UPROPERTY() TArray<FRetakePoseBone> AnimationBones;
    UPROPERTY() FName UpperBodyRoot=TEXT("spine_01");
    /** Pose-major, then non-upper-body bone order (including root). Weapons cannot replace these transforms. */
    UPROPERTY() TArray<FRetakeWeaponPoseTransform> LowerBodyPoses;
    bool Validate(FString& Error) const;
    /** Capture only after the mesh catalogue is assigned. Failure preserves the previous animation reference. */
    bool SetAnimationSkeleton(const USkeleton& Skeleton,FString& Error);
    bool MatchesAnimationSkeleton(const USkeleton& Skeleton,FString& Error) const;
    void GetLowerBodyBones(TArray<int32>& Out) const;
    FString ComputeIdentity() const;
};

/** Prepared weapon-owned poses; exact body contract and source digest are checked before session assembly. */
UCLASS(NotBlueprintable)
class RETAKEMAPSDK_API URetakeWeaponPoseContribution : public UDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(VisibleAnywhere, Category="Retake|Weapons|Preparation") FString Owner;
    UPROPERTY(VisibleAnywhere, Category="Retake|Weapons|Preparation") FRetakeWeaponPoseContract BodyContract;
    UPROPERTY(VisibleAnywhere, Category="Retake|Weapons|Preparation") FString SourceSignature;
    UPROPERTY(VisibleAnywhere, Category="Retake|Weapons|Preparation") TArray<FRetakeHitRigHold> Holds;
    UPROPERTY(VisibleAnywhere, Category="Retake|Weapons|Preparation") FString ContentHash;
#if WITH_EDITORONLY_DATA
    /** Owned editor-only recipe subobject. It is saved with the editable contribution and stripped from cooked output. */
    UPROPERTY(VisibleAnywhere, Instanced, Category="Retake|Weapons|Preparation") TObjectPtr<UObject> AuthoringRecipe;
#endif
    /** Structural/content/ownership checks. Does not claim source freshness or behavioral agreement. */
    bool Validate(const FString& ExpectedOwner,const FRetakeWeaponPoseContract& ExpectedBody,FString& Error) const;
    /** Requires the selected specification/profile to be loaded. Checks declared carry/action bindings, not source freshness. */
    bool ValidateSpecificationBindings(const URetakeWeaponSpecification& Specification,FString& Error) const;
    const FRetakeHitRigHold* FindHold(FName ProfileId) const;
    /** Bounded sample accounting used before retaining multiple providers in one session. */
    int64 PreparedTransformCount() const;
    /** Canonical BLAKE3 over the prepared payload and source digest; assets are identified by cooked-stable paths. */
    FString ComputeContentHash() const;
#if WITH_EDITOR
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
private:
    friend struct FRetakeWeaponPoseBuildAccess;
    bool ValidatePayload(const FString& ExpectedOwner,const FRetakeWeaponPoseContract& ExpectedBody,FString& Error) const;
};

namespace RetakeWeaponPoses
{
    inline constexpr int32 FormatVersion=1;
    inline constexpr uint32 RecipeVersion=1;
    /** Native sampling policy, separate from serialized layout. Both game and creator freshness include it. */
    inline constexpr uint32 PreparationVersion=3; // Prepared holds include muzzle/sight geometry for arms-only height placement.
    inline constexpr int32 MaximumBones=128;
    inline constexpr int32 MaximumHolds=255;
    inline constexpr int64 MaximumTransforms=8*1024*1024;
    inline constexpr float MaximumTrackSeconds=120.f;
    inline constexpr float MaximumSampleRate=600.f;
    RETAKEMAPSDK_API FString HoldIdentity(const FRetakeHitRigHold& Hold);
}

