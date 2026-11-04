// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeWeaponSpecification.h"
#include "NiagaraSystem.h"
#include "RetakeWeaponValidation.h"
#include "RetakeLoadoutPolicy.h"
#include "RetakeAudioCueDefinition.h"
#include "UObject/UnrealType.h"
#include "UObject/SoftObjectPath.h"
#include "Engine/SkeletalMesh.h"

using namespace RetakeWeaponValidation;

namespace
{
    void CollectAuthoredReferences(const FProperty* Property,const void* Value,TArray<FSoftObjectPath>& Paths)
    {
        if (Property->HasAnyPropertyFlags(CPF_Transient)) return;
        if (const auto* Soft=CastField<FSoftObjectProperty>(Property))
        {
            const FSoftObjectPath Path=Soft->GetPropertyValue(Value).ToSoftObjectPath();
            if (Path.IsValid()) Paths.AddUnique(Path);
        }
        else if (const auto* Struct=CastField<FStructProperty>(Property))
        {
            if (Struct->Struct==TBaseStructure<FSoftObjectPath>::Get())
            {
                const auto& Path=*static_cast<const FSoftObjectPath*>(Value);
                if (Path.IsValid()) Paths.AddUnique(Path);
            }
            else for (TFieldIterator<FProperty> It(Struct->Struct);It;++It)
                for (int32 Index=0;Index<It->ArrayDim;++Index)
                    CollectAuthoredReferences(*It,It->ContainerPtrToValuePtr<void>(Value,Index),Paths);
        }
        else if (const auto* Array=CastField<FArrayProperty>(Property))
        {
            FScriptArrayHelper Items(Array,Value);
            for (int32 Index=0;Index<Items.Num();++Index) CollectAuthoredReferences(Array->Inner,Items.GetRawPtr(Index),Paths);
        }
        else if (const auto* Map=CastField<FMapProperty>(Property))
        {
            FScriptMapHelper Items(Map,Value);
            for (int32 Index=0;Index<Items.GetMaxIndex();++Index) if (Items.IsValidIndex(Index))
            {
                CollectAuthoredReferences(Map->KeyProp,Items.GetKeyPtr(Index),Paths);
                CollectAuthoredReferences(Map->ValueProp,Items.GetValuePtr(Index),Paths);
            }
        }
        else if (const auto* Set=CastField<FSetProperty>(Property))
        {
            FScriptSetHelper Items(Set,Value);
            for (int32 Index=0;Index<Items.GetMaxIndex();++Index) if (Items.IsValidIndex(Index))
                CollectAuthoredReferences(Set->ElementProp,Items.GetElementPtr(Index),Paths);
        }
    }
}

void URetakeWeaponSpecification::GetAllReferencedSoftPaths(TArray<FSoftObjectPath>& OutPaths) const
{
    for (TFieldIterator<FProperty> It(StaticClass(),EFieldIterationFlags::None);It;++It)
        for (int32 Index=0;Index<It->ArrayDim;++Index)
            CollectAuthoredReferences(*It,It->ContainerPtrToValuePtr<void>(this,Index),OutPaths);
}

FPrimaryAssetId URetakeWeaponSpecification::GetPrimaryAssetId() const
{
    return FPrimaryAssetId(TEXT("RetakeWeaponSpecification"), FName(*GetPathName()));
}

void URetakeWeaponSpecification::CopySpecificationTo(URetakeWeaponSpecification& Destination) const
{
    // The fixed native contract is the whitelist. Inherited/derived fields and runtime state are never copied.
    for (TFieldIterator<FProperty> It(StaticClass(),EFieldIterationFlags::None); It; ++It)
        It->CopyCompleteValue_InContainer(&Destination,this);
}

bool URetakeWeaponSpecification::ValidateSpecification(TArray<FText>& OutErrors) const
{
    FWeaponValidation V(OutErrors);
    V.Identity(WeaponId, TEXT("WeaponId"));
    V.Require(!DisplayName.IsEmpty(), TEXT("Weapon DisplayName is required."));
    V.Require(IsSlotValid(Slot), TEXT("Weapon Slot is invalid."));
    V.Require(Selection.Role <= ERetakeWeaponSelectionRole::Equipment, TEXT("Selection.Role is invalid."));
    V.Nonnegative(Selection.CycleOrder, TEXT("Selection.CycleOrder"));
    V.Require(PrimaryAction.Type != ERetakeWeaponAction::Grenade || Selection.Role == ERetakeWeaponSelectionRole::Equipment,
        TEXT("Grenades require the Equipment selection role."));
    V.Positive(MovementSpeedMultiplier, TEXT("MovementSpeedMultiplier"));
    V.Require(!Presentation.FirstPersonRigOffset.ContainsNaN() && Presentation.FirstPersonRigOffset.GetRotation().IsNormalized()
        && Presentation.FirstPersonRigOffset.GetScale3D().GetMin() > 0.0,
        TEXT("Presentation.FirstPersonRigOffset must be finite, have a normalized rotation and positive scale."));
    const auto& Clearance=Presentation.Clearance;
    V.Range(Clearance.ThirdPersonAimConvergenceDegrees,0,45,TEXT("Clearance.ThirdPersonAimConvergenceDegrees"));
    V.Range(Clearance.ThirdPersonLeanElbowDownWeight,0,1,TEXT("Clearance.ThirdPersonLeanElbowDownWeight"));
    V.Range(Clearance.FirstPersonAimConvergenceDegrees,0,45,TEXT("Clearance.FirstPersonAimConvergenceDegrees"));
    V.Require(PrimaryAction.Type != ERetakeWeaponAction::None || Equipment.IsEnabled(), TEXT("A weapon requires a primary attack or a supported equipment use capability."));
    V.Require(uint8(Equipment.Use)<=uint8(ERetakeEquipmentUse::ActivateTarget),TEXT("Equipment use capability is invalid."));
    V.Range(Equipment.UseIntervalSeconds,.01f,60.f,TEXT("Equipment.UseIntervalSeconds"));
    V.Range(Equipment.UseRangeCm,0.f,100000.f,TEXT("Equipment.UseRangeCm"));
    V.Range(Equipment.SecondaryReturnDelaySeconds,0.f,2.f,TEXT("Equipment.SecondaryReturnDelaySeconds"));
    if (Equipment.IsEnabled())
    {
        V.Require(Slot==ERetakeWeaponSlot::Equipment && Selection.Role==ERetakeWeaponSelectionRole::Equipment,
            TEXT("Equipment use requires the Equipment slot and selection role."));
        V.Require(PrimaryAction.Type==ERetakeWeaponAction::None && SecondaryAction.Type==ERetakeWeaponAction::None,
            TEXT("Equipment use is separate from damaging primary/secondary attacks."));
        V.Require(PrimaryAction.AmmoCost==0 && SecondaryAction.AmmoCost==0 && Ammo.IsNull() && SecondaryAmmo.IsNull()
            && StartingAmmo==0 && SecondaryStartingAmmo==0 && MagazineCapacity==0 && Reload.Policy==ERetakeReloadPolicy::None,
            TEXT("Non-damaging equipment cannot consume, grant or reload ammunition."));
        V.Positive(Equipment.UseRangeCm,TEXT("Equipment.UseRangeCm"));
        V.Require(!Equipment.bSilentUse || Equipment.UseCue.IsNull(),TEXT("Silent equipment use must not also declare a use cue."));
        V.Reference(Equipment.UseCue.ToSoftObjectPath(),TEXT("Equipment.UseCue"),!Equipment.bSilentUse);
        if (const auto* Cue=Equipment.UseCue.Get())
        {
            FString Error;const bool Valid=Cue->Validate(Error);V.Require(Valid,TEXT("Equipment audio: ")+Error);
            V.Require(Cue->bSpatial && !Cue->bLooping && Cue->Category==ERetakeAudioCategory::Gameplay,
                TEXT("Equipment use audio requires a positional Gameplay one-shot cue."));
        }
        V.Require(Aim.Mode!=ERetakeAimMode::IronSights,TEXT("Equipment optics use Scope, not firearm iron-sight alignment."));
    }
    V.Require(!Equipment.bSecondaryReturnsToPrevious || Equipment.IsEnabled(),TEXT("Secondary return requires an enabled equipment capability."));
    V.Require(!Equipment.bAutomaticScope || (Equipment.IsEnabled() && Aim.Mode==ERetakeAimMode::Scope),
        TEXT("Automatic equipment optics require an enabled equipment capability and Scope aiming."));
    V.Require(static_cast<uint8>(Aim.Mode) <= static_cast<uint8>(ERetakeAimMode::Scope), TEXT("Aim.Mode is invalid."));
    V.Range(Aim.FieldOfView, 1, 170, TEXT("Aim.FieldOfView"));
    V.Range(Aim.InSeconds, 0.01f, 5.f, TEXT("Aim.InSeconds"));
    V.Range(Aim.OutSeconds, 0.01f, 5.f, TEXT("Aim.OutSeconds"));
    V.Range(Aim.MovementMultiplier, 0.01f, 1.f, TEXT("Aim.MovementMultiplier"));
    V.Range(Aim.SpreadMultiplier, 0, 1, TEXT("Aim.SpreadMultiplier"));
    V.Require(!Aim.FirstPersonRigTransform.ContainsNaN() && Aim.FirstPersonRigTransform.GetRotation().IsNormalized(), TEXT("Aim rig transform must be finite and normalized."));
    ValidateAimPoint(Aim.AimPoint, TEXT("Aim"), V);
    {
        FRetakeWeaponAimPoint Provider; FName ProviderSlot;
        if (RetakeWeaponValidation::SelectAimPointProvider(Aim.AimPoint, Presentation.Attachments, Provider, ProviderSlot))
            V.Require(Aim.bAlignmentAuthored, TEXT("An aim point requires Aim.bAlignmentAuthored; without it aiming is denied and the sight is never used."));
    }
    V.Require(Aim.Mode == ERetakeAimMode::None || PrimaryAction.Type != ERetakeWeaponAction::Grenade, TEXT("Grenades cannot enter ADS/scope."));
    ValidateAction(PrimaryAction, TEXT("PrimaryAction"), V, false);
    if (!Presentation.RechamberSound.IsNull())
    {
        V.Require(PrimaryAction.Type==ERetakeWeaponAction::Hitscan || PrimaryAction.Type==ERetakeWeaponAction::Pellets
            || PrimaryAction.Type==ERetakeWeaponAction::Rocket, TEXT("Rechamber presentation requires a firearm primary."));
    }
    ValidateAction(SecondaryAction, TEXT("SecondaryAction"), V, true);
    const bool bConsumesAmmo = PrimaryAction.AmmoCost > 0 || (SecondaryAmmo.IsNull() && SecondaryAction.AmmoCost > 0);
    V.Reference(Ammo.ToSoftObjectPath(), TEXT("Ammo"), bConsumesAmmo || StartingAmmo > 0);
    V.Nonnegative(StartingAmmo,TEXT("StartingAmmo"));
    V.Require(StartingItems.Num()<=16 && StartingAmmoGrants.Num()<=32,TEXT("Starting grants exceed the per-definition item/ammo limits."));
    TSet<FName> ItemIds,AmmoIds;
    for (const auto& Item:StartingItems)
    {
        V.Require(FRetakeLoadoutPolicyDefinition::IsValidId(Item.WeaponId) && Item.WeaponId!=WeaponId && !ItemIds.Contains(Item.WeaponId),
            TEXT("Starting items need unique valid identities and cannot reference their own weapon."));
        ItemIds.Add(Item.WeaponId);
    }
    for (const auto& Grant:StartingAmmoGrants)
    {
        V.Require(FRetakeLoadoutPolicyDefinition::IsValidId(Grant.AmmoId) && !AmmoIds.Contains(Grant.AmmoId)
            && Grant.Amount>0 && Grant.Amount<=1000000,TEXT("Starting ammunition grants need unique pools and bounded positive amounts."));
        V.Require(Grant.FallbackWeaponId.IsNone() || FRetakeLoadoutPolicyDefinition::IsValidId(Grant.FallbackWeaponId),
            TEXT("Starting ammunition fallback must be empty or a valid weapon identity."));
        AmmoIds.Add(Grant.AmmoId);
    }
    if (const URetakeAmmoDefinition* LoadedAmmo=Ammo.Get())
    {
        const int64 Capacity=int64(LoadedAmmo->ReserveCapacity)
            + (PrimaryAction.Type==ERetakeWeaponAction::Grenade ? 0 : int64(MagazineCapacity));
        V.Require(int64(StartingAmmo)<=Capacity,TEXT("Starting ammunition exceeds the total loaded/reserve capacity (or grenade stack capacity)."));
    }
    if (const URetakeAmmoDefinition* LoadedAmmo = Ammo.Get()) LoadedAmmo->ValidateDefinition(OutErrors);
    V.Nonnegative(SecondaryStartingAmmo,TEXT("SecondaryStartingAmmo"));
    if (!SecondaryAmmo.IsNull())
    {
        V.Reference(SecondaryAmmo.ToSoftObjectPath(),TEXT("SecondaryAmmo"),true);
        V.Require(SecondaryAction.AmmoCost>0 && (SecondaryAction.Type==ERetakeWeaponAction::Hitscan
            || SecondaryAction.Type==ERetakeWeaponAction::Pellets || SecondaryAction.Type==ERetakeWeaponAction::Rocket),
            TEXT("Independent secondary ammunition requires a consuming firearm secondary action."));
        V.Require(PrimaryAction.Type!=ERetakeWeaponAction::Grenade,TEXT("Consumable grenades cannot also own a secondary reserve."));
        V.Require(SecondaryAmmo.ToSoftObjectPath()!=Ammo.ToSoftObjectPath(),TEXT("Secondary ammunition must be distinct from the primary pool."));
        if (const URetakeAmmoDefinition* Secondary=SecondaryAmmo.Get())
        {
            Secondary->ValidateDefinition(OutErrors);
            V.Require(SecondaryStartingAmmo<=Secondary->ReserveCapacity,TEXT("Secondary starting ammunition exceeds its capacity."));
            V.Require(SecondaryAction.AmmoCost<=Secondary->ReserveCapacity,TEXT("Secondary attack costs more than its reserve capacity."));
            if (const auto* Primary=Ammo.Get()) V.Require(Primary->AmmoId!=Secondary->AmmoId,TEXT("Primary and secondary reserves need distinct ammunition IDs."));
        }
    }
    else V.Require(SecondaryStartingAmmo==0,TEXT("Secondary starting ammunition needs a secondary ammunition definition."));
    V.Nonnegative(MagazineCapacity, TEXT("MagazineCapacity"));
    const bool bGrenade = PrimaryAction.Type == ERetakeWeaponAction::Grenade;
    V.Require((Slot == ERetakeWeaponSlot::Grenade) == bGrenade, TEXT("Grenade primaries must use the Grenade slot; other weapons must not."));
    V.Require(SecondaryAction.Type != ERetakeWeaponAction::Grenade || bGrenade, TEXT("A short throw requires a grenade primary."));
    if (bGrenade)
    {
        V.Range(Grenade.GroundDragRatePerSecond,0,100,TEXT("Grenade.GroundDragRatePerSecond"));
        V.Range(Grenade.ContactHeadDamage,0,1000000,TEXT("Grenade.ContactHeadDamage"));
        V.Range(Grenade.ContactBodyDamage,0,1000000,TEXT("Grenade.ContactBodyDamage"));
        V.Range(Grenade.ContactDestructibleDamage,0,1000000,TEXT("Grenade.ContactDestructibleDamage"));
        V.Require(MagazineCapacity == 0 && Reload.Policy == ERetakeReloadPolicy::None, TEXT("Grenades use a consumable stack, not magazines/reloads."));
        V.Require(PrimaryAction.AmmoCost == 1 && (SecondaryAction.Type != ERetakeWeaponAction::Grenade || SecondaryAction.AmmoCost == 1), TEXT("Each grenade arm consumes exactly one grenade."));
    }
    else if (bConsumesAmmo)
    {
        V.Positive(MagazineCapacity, TEXT("MagazineCapacity"));
        V.Require(PrimaryAction.AmmoCost <= MagazineCapacity && (!SecondaryAmmo.IsNull() || SecondaryAction.AmmoCost <= MagazineCapacity), TEXT("A magazine action cannot cost more than a full magazine."));
    }
    V.Require(StaticEnum<ERetakeReloadPolicy>()->IsValidEnumValue(static_cast<int64>(Reload.Policy)), TEXT("Reload.Policy is invalid."));
    V.Nonnegative(Reload.CommitSeconds, TEXT("Reload.CommitSeconds"));
    V.Nonnegative(Reload.DurationSeconds, TEXT("Reload.DurationSeconds"));
    V.Nonnegative(Reload.PerRoundStartSeconds, TEXT("Reload.PerRoundStartSeconds"));
    V.Nonnegative(Reload.PerRoundInsertSeconds, TEXT("Reload.PerRoundInsertSeconds"));
    V.Nonnegative(Reload.PerRoundEndSeconds, TEXT("Reload.PerRoundEndSeconds"));
    V.Nonnegative(Reload.PerRoundCommitLeadSeconds,TEXT("Reload.PerRoundCommitLeadSeconds"));
    V.Require(Reload.PerRoundCommitLeadSeconds<=Reload.PerRoundInsertSeconds,
        TEXT("Reload.PerRoundCommitLeadSeconds must not exceed the insertion duration."));
    if (Reload.Policy != ERetakeReloadPolicy::None)
        V.Require(bConsumesAmmo && MagazineCapacity > 0, TEXT("Reload requires a magazine and an explicit ammo definition."));
    if (Reload.Policy == ERetakeReloadPolicy::Magazine)
    {
        V.Positive(Reload.DurationSeconds, TEXT("Reload.DurationSeconds"));
        V.Require(Reload.CommitSeconds <= Reload.DurationSeconds, TEXT("Magazine ammo commit must not follow reload completion."));
    }
    if (Reload.Policy == ERetakeReloadPolicy::PerRound) V.Positive(Reload.PerRoundInsertSeconds, TEXT("Reload.PerRoundInsertSeconds"));
    if (ReloadAudio.bAuthored)
    {
        V.Require(Reload.Policy!=ERetakeReloadPolicy::None,TEXT("Authored reload audio needs a reload policy."));
        V.Require(!ReloadAudio.Start.IsEmpty() || !ReloadAudio.Insert.IsEmpty() || !ReloadAudio.End.IsEmpty(),
            TEXT("Authored reload audio requires at least one event."));
        V.Require(Reload.Policy==ERetakeReloadPolicy::PerRound || (ReloadAudio.Insert.IsEmpty() && ReloadAudio.End.IsEmpty()),
            TEXT("Magazine reload audio uses the Start phase only."));
    }
    for (int32 Stage=0;Stage<3;++Stage)
    {
        const auto Events=ReloadAudio.Events(Stage);
        V.Require(Events.Num()<=FRetakeWeaponReloadAudio::MaximumEventsPerPhase,TEXT("Reload audio exceeds the per-phase event limit."));
        const float Duration=Reload.Policy==ERetakeReloadPolicy::Magazine ? Reload.DurationSeconds
            : Stage==0 ? Reload.PerRoundStartSeconds : Stage==1 ? Reload.PerRoundInsertSeconds:Reload.PerRoundEndSeconds;
        float Previous=-1.f;
        TArray<TPair<float,FSoftObjectPath>> Seen;
        for (const auto& Event:Events)
        {
            V.Range(Event.Seconds,0.f,65.f,TEXT("Reload audio event time"));
            V.Require(Event.Seconds>=Previous,TEXT("Reload audio events must be ordered by time."));Previous=Event.Seconds;
            if (ReloadAudio.bAuthored) V.Require(Event.Seconds<=Duration,TEXT("Reload audio event follows its authored phase end."));
            V.Reference(Event.Cue.ToSoftObjectPath(),TEXT("Reload audio cue"),true);
            const auto Key=TPair<float,FSoftObjectPath>(Event.Seconds,Event.Cue.ToSoftObjectPath());
            V.Require(!Seen.Contains(Key),TEXT("Reload audio repeats the same cue at the same phase time."));Seen.Add(Key);
            if (const auto* Cue=Event.Cue.Get())
            {
                FString Error;V.Require(Cue->Validate(Error),TEXT("Reload audio: ")+Error);
                V.Require(Cue->bSpatial && !Cue->bLooping && Cue->Category==ERetakeAudioCategory::Gameplay && Cue->Generator.IsNull(),
                    TEXT("Timed reload audio requires a positional Gameplay recording cue without a generator."));
            }
        }
    }
    V.Nonnegative(Equip.AttachSeconds, TEXT("Equip.AttachSeconds"));
    V.Nonnegative(Equip.DurationSeconds, TEXT("Equip.DurationSeconds"));
    V.Require(Equip.AttachSeconds <= Equip.DurationSeconds, TEXT("Equipment attachment must not follow equip completion."));
    V.Nonnegative(Holster.DurationSeconds,TEXT("Holster.DurationSeconds"));
    V.Nonnegative(Holster.CommitSeconds,TEXT("Holster.CommitSeconds"));
    V.Require(Holster.CommitSeconds<=Holster.DurationSeconds,TEXT("Holster commit must not follow completion."));
    V.Nonnegative(Drop.DurationSeconds,TEXT("Drop.DurationSeconds"));
    V.Nonnegative(Drop.CommitSeconds,TEXT("Drop.CommitSeconds"));
    V.Require(Drop.CommitSeconds<=Drop.DurationSeconds,TEXT("Drop commit must not follow completion."));
    V.Require(StaticEnum<ERetakeViewKickMode>()->IsValidEnumValue(static_cast<int64>(ViewKick.Mode)), TEXT("ViewKick.Mode is invalid."));
    V.Range(ViewKick.PitchMinDegrees, -180, 180, TEXT("ViewKick.PitchMinDegrees"));
    V.Range(ViewKick.PitchMaxDegrees, ViewKick.PitchMinDegrees, 180, TEXT("ViewKick.PitchMaxDegrees"));
    V.Range(ViewKick.YawMinDegrees, -180, 180, TEXT("ViewKick.YawMinDegrees"));
    V.Range(ViewKick.YawMaxDegrees, ViewKick.YawMinDegrees, 180, TEXT("ViewKick.YawMaxDegrees"));
    V.Nonnegative(ViewKick.RecenterSpeed, TEXT("ViewKick.RecenterSpeed"));
    V.Nonnegative(ViewKick.MinDecayDegreesPerSecond, TEXT("ViewKick.MinDecayDegreesPerSecond"));
    V.Nonnegative(ViewKick.MaxDecayDegreesPerSecond, TEXT("ViewKick.MaxDecayDegreesPerSecond"));
    V.Require(ViewKick.MinDecayDegreesPerSecond <= ViewKick.MaxDecayDegreesPerSecond, TEXT("ViewKick minimum decay must not exceed maximum decay."));
    if (ViewKick.Mode == ERetakeViewKickMode::T || ViewKick.Mode == ERetakeViewKickMode::V) V.Positive(ViewKick.MaxDecayDegreesPerSecond, TEXT("ViewKick.MaxDecayDegreesPerSecond"));
    if (ViewKick.Mode == ERetakeViewKickMode::Velocity)
    {
        // Without centring the camera would stay kicked at its cap.
        V.Positive(ViewKick.HipCenterDegreesPerSecondSquared, TEXT("ViewKick.HipCenterDegreesPerSecondSquared"));
        V.Positive(ViewKick.AdsCenterDegreesPerSecondSquared, TEXT("ViewKick.AdsCenterDegreesPerSecondSquared"));
    }
    V.Range(ViewKick.PitchCapDegrees, 0, 180, TEXT("ViewKick.PitchCapDegrees"));
    V.Range(ViewKick.YawCapDegrees, 0, 180, TEXT("ViewKick.YawCapDegrees"));
    V.Range(ViewKick.ThresholdDegrees, 0, 180, TEXT("ViewKick.ThresholdDegrees"));
    const auto& P = Projectile;
#define NONNEGATIVE_PROJECTILE(Field) V.Nonnegative(P.Field, TEXT("Projectile." #Field))
    NONNEGATIVE_PROJECTILE(DirectDamage); NONNEGATIVE_PROJECTILE(InitialSpeedCmPerSecond); NONNEGATIVE_PROJECTILE(MaxSpeedCmPerSecond);
    NONNEGATIVE_PROJECTILE(MinimumSpeedCmPerSecond); NONNEGATIVE_PROJECTILE(MinLifeSeconds);
    NONNEGATIVE_PROJECTILE(GravityScale); NONNEGATIVE_PROJECTILE(CollisionRadiusCm); NONNEGATIVE_PROJECTILE(LifetimeSeconds);
    NONNEGATIVE_PROJECTILE(HeldOvercookSeconds); NONNEGATIVE_PROJECTILE(MinChargeSeconds); NONNEGATIVE_PROJECTILE(MaxChargeSeconds);
    NONNEGATIVE_PROJECTILE(ExplosionDamage); NONNEGATIVE_PROJECTILE(ExplosionMinDamage); NONNEGATIVE_PROJECTILE(ExplosionInnerRadiusCm);
    NONNEGATIVE_PROJECTILE(ExplosionOuterRadiusCm); NONNEGATIVE_PROJECTILE(ExplosionFalloff);
    NONNEGATIVE_PROJECTILE(ShortThrowInitialSpeedCmPerSecond); NONNEGATIVE_PROJECTILE(ShortThrowMinimumSpeedCmPerSecond);
    NONNEGATIVE_PROJECTILE(ShortThrowMinChargeSeconds); NONNEGATIVE_PROJECTILE(ShortThrowMaxChargeSeconds); NONNEGATIVE_PROJECTILE(ShortThrowMinLifeSeconds);
#undef NONNEGATIVE_PROJECTILE
    V.Range(P.Bounciness, 0, 1, TEXT("Projectile.Bounciness"));
    V.Range(P.Friction, 0, 1, TEXT("Projectile.Friction"));
    V.Range(P.GravityCmPerSecondSquared,0,20000,TEXT("Projectile.GravityCmPerSecondSquared"));
    V.Range(P.DamageableHealth,0,1000000,TEXT("Projectile.DamageableHealth"));
    V.Require(!P.bCanBeShot || P.DamageableHealth>0,TEXT("Shootable projectiles require positive DamageableHealth."));
    V.Range(P.GlassBreakMinimumSpeedCmPerSecond,0,100000,TEXT("Projectile.GlassBreakMinimumSpeedCmPerSecond"));
    V.Range(P.GlassBreakLookAheadSeconds,.01f,1,TEXT("Projectile.GlassBreakLookAheadSeconds"));
    V.Require(FMath::IsFinite(P.LaunchVelocityZCmPerSecond) && FMath::IsFinite(P.ShortThrowLaunchVelocityZCmPerSecond), TEXT("Projectile launch velocity additions must be finite."));
    V.Require(P.MinChargeSeconds <= P.MaxChargeSeconds, TEXT("Projectile minimum charge must not exceed maximum charge."));
    V.Require(P.ShortThrowMinChargeSeconds <= P.ShortThrowMaxChargeSeconds, TEXT("Short throw minimum charge must not exceed maximum charge."));
    V.Require(P.MinLifeSeconds <= P.LifetimeSeconds && P.ShortThrowMinLifeSeconds <= P.LifetimeSeconds, TEXT("Minimum projectile lives must not exceed their lifetime."));
    V.Require(P.MinimumSpeedCmPerSecond <= P.InitialSpeedCmPerSecond && P.ShortThrowMinimumSpeedCmPerSecond <= P.ShortThrowInitialSpeedCmPerSecond, TEXT("Minimum launch speeds must not exceed their maximum-charge speeds."));
    V.Require(P.ExplosionMinDamage <= P.ExplosionDamage && P.ExplosionInnerRadiusCm <= P.ExplosionOuterRadiusCm, TEXT("Explosion minimum damage/radius must not exceed their maxima."));
    V.Range(P.ExplosionSelfDamageMultiplier,0,1,TEXT("Projectile.ExplosionSelfDamageMultiplier"));
    FString CollisionError;
    if (!P.ValidateCollision(CollisionError)) V.Require(false,*CollisionError);
    if (!P.ValidateDrift(CollisionError)) V.Require(false,*CollisionError);
    if (P.Drift.bEnabled)
        V.Require(!bGrenade && SecondaryAction.Type!=ERetakeWeaponAction::Grenade && (PrimaryAction.Type==ERetakeWeaponAction::Rocket || SecondaryAction.Type==ERetakeWeaponAction::Rocket),TEXT("Projectile drift requires a rocket action, not a thrown grenade."));
    const bool bProjectile = bGrenade || PrimaryAction.Type == ERetakeWeaponAction::Rocket || SecondaryAction.Type == ERetakeWeaponAction::Rocket;
    V.Require(P.DamageableHealth==0 || bProjectile,TEXT("Projectile health requires a physical projectile action."));
    V.Require(P.GlassBreakMinimumSpeedCmPerSecond==0 || bProjectile,TEXT("Projectile glass breaking requires a physical projectile action."));
    if (bProjectile)
    {
        V.Positive(P.InitialSpeedCmPerSecond, TEXT("Projectile.InitialSpeedCmPerSecond"));
        if (!P.bAxisAlignedBoxCollision) V.Positive(P.CollisionRadiusCm, TEXT("Projectile.CollisionRadiusCm"));
        V.Positive(P.LifetimeSeconds, TEXT("Projectile.LifetimeSeconds"));
        V.Require(P.MaxSpeedCmPerSecond == 0 || P.MaxSpeedCmPerSecond >= P.InitialSpeedCmPerSecond, TEXT("Projectile maximum speed must be zero (uncapped) or at least its launch speed."));
        if (P.ExplosionDamage > 0) V.Positive(P.ExplosionOuterRadiusCm, TEXT("Projectile.ExplosionOuterRadiusCm"));
        if (bGrenade && (Grenade.CookWhileHeld || Grenade.ShortThrowCookWhileHeld)) V.Positive(P.HeldOvercookSeconds, TEXT("Projectile.HeldOvercookSeconds"));
        if (SecondaryAction.Type == ERetakeWeaponAction::Grenade)
            V.Positive(P.ShortThrowInitialSpeedCmPerSecond, TEXT("Projectile.ShortThrowInitialSpeedCmPerSecond"));
    }
    if (bGrenade)
    {
        const auto CheckSet=[&V](const FRetakeGrenadeAnimationSet& Set,const FString& Prefix)
        {
            V.Reference(Set.PrepareLong.ToSoftObjectPath(),Prefix+TEXT(".PrepareLong"),true);
            V.Reference(Set.PrepareShort.ToSoftObjectPath(),Prefix+TEXT(".PrepareShort"),true);
            V.Reference(Set.ReleaseLong.ToSoftObjectPath(),Prefix+TEXT(".ReleaseLong"),true);
            V.Reference(Set.ReleaseShort.ToSoftObjectPath(),Prefix+TEXT(".ReleaseShort"),true);
        };
        CheckSet(Presentation.FirstPersonGrenade,TEXT("Presentation.FirstPersonGrenade"));
        CheckSet(Presentation.ThirdPersonGrenade,TEXT("Presentation.ThirdPersonGrenade"));
        CheckSet(Presentation.WeaponGrenade,TEXT("Presentation.WeaponGrenade"));
        if (Grenade.Effect==ERetakeGrenadeEffect::Smoke)
        {
            V.Reference(Presentation.SmokeEffect.ToSoftObjectPath(),TEXT("Presentation.SmokeEffect"),true);
            FString EmissionError;
            if (!Grenade.Smoke.ValidateEmission(EmissionError)) V.Require(false,*EmissionError);
            if (const auto* System=Presentation.SmokeEffect.Get())
                if (!Grenade.Smoke.ValidateEmissionPresentation(System,EmissionError)) V.Require(false,*EmissionError);
            V.Range(Grenade.Smoke.BulletHoleRadiusCm,.1f,200.f,TEXT("Grenade.Smoke.BulletHoleRadiusCm"));
            V.Range(Grenade.Smoke.BulletHoleHoldSeconds,0.f,10.f,TEXT("Grenade.Smoke.BulletHoleHoldSeconds"));
            V.Range(Grenade.Smoke.BulletHoleCloseSeconds,.01f,30.f,TEXT("Grenade.Smoke.BulletHoleCloseSeconds"));
            V.Range(Grenade.Smoke.BlastHoleHoldSeconds,0.f,10.f,TEXT("Grenade.Smoke.BlastHoleHoldSeconds"));
            V.Range(Grenade.Smoke.BlastHoleCloseSeconds,.01f,30.f,TEXT("Grenade.Smoke.BlastHoleCloseSeconds"));
        }
        if (Grenade.Effect==ERetakeGrenadeEffect::Incendiary) V.Reference(Presentation.FireEffect.ToSoftObjectPath(),TEXT("Presentation.FireEffect"),true);
        if (Grenade.Effect==ERetakeGrenadeEffect::Flash) V.Reference(Presentation.FlashScreenMaterial.ToSoftObjectPath(),TEXT("Presentation.FlashScreenMaterial"),true);
        V.Require(!Projectile.bDetonateOnImpact || Grenade.Effect==ERetakeGrenadeEffect::Incendiary, TEXT("Only incendiary grenades may break on impact."));
        V.Require(Grenade.Effect==ERetakeGrenadeEffect::Explosion || (Projectile.ExplosionDamage==0 && Projectile.ExplosionMinDamage==0),
            TEXT("Only explosive grenades deal blast damage."));
    }
    if (const auto* Timer=Presentation.CookTimerCue.Get())
        V.Require(Timer->Recordings.Num()==1 && Timer->PitchVariationSemitones==0.f
            && Timer->Recordings[0].PitchMinimum==1.f && Timer->Recordings[0].PitchMaximum==1.f,
            TEXT("Presentation.CookTimerCue needs exactly one recording at fixed normal pitch: its end marks the overcook."));
    ValidatePresentation(Presentation, V, !Equipment.IsEnabled() && !bGrenade);
    if (Aim.Mode==ERetakeAimMode::Scope)
        V.Reference(Presentation.ScopeMaterial.ToSoftObjectPath(),TEXT("Presentation.ScopeMaterial"),true);

    return V.IsValid();
}

void URetakeWeaponSpecification::GetReferencedSoftPaths(TArray<FSoftObjectPath>& OutPaths, bool bServer) const
{
    auto Add = [&OutPaths](const FSoftObjectPath& Path) { if (Path.IsValid()) OutPaths.AddUnique(Path); };
    Add(Ammo.ToSoftObjectPath());
    Add(SecondaryAmmo.ToSoftObjectPath());
    const auto& P = Presentation;
    Add(P.CharacterProfile.ToSoftObjectPath());
    Add(P.WorldMesh.ToSoftObjectPath()); // Server geometry/muzzle reference is required until independently replaced.
    for (const FRetakeWeaponAttachment& A : P.Attachments) Add(A.Mesh.ToSoftObjectPath()); // Clearance/visibility metadata also belongs on authority.
    Add(P.IdleAnimation.ToSoftObjectPath()); // The gameplay muzzle uses the authored idle bone pose on the server too.
    const auto AddGrenade=[&Add](const FRetakeGrenadeAnimationSet& G){Add(G.PrepareLong.ToSoftObjectPath());Add(G.PrepareShort.ToSoftObjectPath());Add(G.ReleaseLong.ToSoftObjectPath());Add(G.ReleaseShort.ToSoftObjectPath());};
    AddGrenade(P.ThirdPersonGrenade);
    // Cue definitions are metadata only; their recording soft references do not load on the server.
    Add(P.PrimaryFireCue.ToSoftObjectPath()); Add(P.SecondaryFireCue.ToSoftObjectPath());
    Add(P.ReloadCue.ToSoftObjectPath()); Add(P.ReloadInsertCue.ToSoftObjectPath()); Add(P.ReloadEndCue.ToSoftObjectPath());
    Add(P.EquipCue.ToSoftObjectPath()); Add(P.HolsterCue.ToSoftObjectPath()); Add(P.ThrowCue.ToSoftObjectPath()); Add(P.PinCue.ToSoftObjectPath());
    Add(P.CookTimerCue.ToSoftObjectPath());
    Add(P.ExplosionCue.ToSoftObjectPath()); Add(P.Attenuation.ToSoftObjectPath()); Add(P.DryFireCue.ToSoftObjectPath());
    Add(P.FireSelectorCue.ToSoftObjectPath());
    if (ReloadAudio.bAuthored)
        for (int32 Stage=0;Stage<3;++Stage) for (const auto& Event:ReloadAudio.Events(Stage)) Add(Event.Cue.ToSoftObjectPath());
    if (Equipment.IsEnabled()) Add(Equipment.UseCue.ToSoftObjectPath());
    if (bServer) return;
    Add(P.BashProp.Mesh.ToSoftObjectPath());
    AddGrenade(P.FirstPersonGrenade); AddGrenade(P.WeaponGrenade);
#define ADD_PRESENTATION_REF(Field) Add(P.Field.ToSoftObjectPath())
    ADD_PRESENTATION_REF(FirstPersonMesh);
    for (const FRetakeWeaponAttachment& A : P.Attachments) Add(A.Mesh.ToSoftObjectPath());
    ADD_PRESENTATION_REF(ReloadInsertAnimation); ADD_PRESENTATION_REF(ReloadEndAnimation);
    ADD_PRESENTATION_REF(FirstPersonBashAnimation); ADD_PRESENTATION_REF(ThirdPersonBashAnimation);
    for (const auto& Variant:P.BashVariants)
    { Add(Variant.FirstPersonAnimation.ToSoftObjectPath()); Add(Variant.ThirdPersonAnimation.ToSoftObjectPath()); }
    ADD_PRESENTATION_REF(FirstPersonReloadStart); ADD_PRESENTATION_REF(FirstPersonReloadInsert); ADD_PRESENTATION_REF(FirstPersonReloadEnd);
    ADD_PRESENTATION_REF(ThirdPersonReloadStart); ADD_PRESENTATION_REF(ThirdPersonReloadInsert); ADD_PRESENTATION_REF(ThirdPersonReloadEnd);
    ADD_PRESENTATION_REF(ReloadInsertCue); ADD_PRESENTATION_REF(ReloadEndCue); ADD_PRESENTATION_REF(ShellEjectEffect); ADD_PRESENTATION_REF(ShellEjectMesh);
    ADD_PRESENTATION_REF(ProjectileMesh);
    ADD_PRESENTATION_REF(ProjectileStaticMesh);
    ADD_PRESENTATION_REF(FireAnimation); ADD_PRESENTATION_REF(ReloadAnimation); ADD_PRESENTATION_REF(FirstPersonReloadAnimation);
    ADD_PRESENTATION_REF(PrimaryFireCue); ADD_PRESENTATION_REF(SecondaryFireCue); ADD_PRESENTATION_REF(DryFireCue); ADD_PRESENTATION_REF(FireSelectorCue);
    ADD_PRESENTATION_REF(RechamberSound); ADD_PRESENTATION_REF(RechamberAttenuation);
    ADD_PRESENTATION_REF(ReloadCue); ADD_PRESENTATION_REF(EquipCue); ADD_PRESENTATION_REF(HolsterCue); ADD_PRESENTATION_REF(PinCue);
    ADD_PRESENTATION_REF(ThrowCue); ADD_PRESENTATION_REF(CookTimerCue); ADD_PRESENTATION_REF(ExplosionCue);
    ADD_PRESENTATION_REF(Attenuation); ADD_PRESENTATION_REF(Concurrency);
    ADD_PRESENTATION_REF(MuzzleEffect); ADD_PRESENTATION_REF(ExplosionEffect); ADD_PRESENTATION_REF(SmokeEffect); ADD_PRESENTATION_REF(SmokeLoopSound);
    ADD_PRESENTATION_REF(ExplosionAirEffect);
    if (!P.SmokeLoopSound.IsNull()) ADD_PRESENTATION_REF(SmokeLoopAttenuation); // Preloaded so a smoke start never loads it.
    ADD_PRESENTATION_REF(FireEffect); ADD_PRESENTATION_REF(FireLoopSound); ADD_PRESENTATION_REF(FlashScreenMaterial); ADD_PRESENTATION_REF(FlashRingSound);
    for (const FRetakeWeaponTeamMeshes& T : P.TeamMeshes)
    {
        Add(T.WorldMesh.ToSoftObjectPath()); Add(T.FirstPersonMesh.ToSoftObjectPath());
        Add(T.ProjectileMesh.ToSoftObjectPath()); Add(T.ProjectileStaticMesh.ToSoftObjectPath());
    }
    ADD_PRESENTATION_REF(Tracer);
    ADD_PRESENTATION_REF(Icon); ADD_PRESENTATION_REF(ScopeMaterial);
    ADD_PRESENTATION_REF(WeaponIconMaterial); ADD_PRESENTATION_REF(ReticleMaterial);
    // The HUD loadout shows the thumbnail, or the hero art when there is none, and loads it synchronously on every gun
    // change; resident here, that load finds it in memory (A20-20). Only the image actually shown is kept.
    if (!P.LoadoutThumbnail.IsNull()) ADD_PRESENTATION_REF(LoadoutThumbnail); else ADD_PRESENTATION_REF(LoadoutHero);
#undef ADD_PRESENTATION_REF
}

#if WITH_EDITOR
EDataValidationResult URetakeWeaponSpecification::IsDataValid(FDataValidationContext& Context) const
{
    TArray<FText> Errors;
    TArray<FSoftObjectPath> Paths;
    GetAllReferencedSoftPaths(Paths);
    for (const auto& Path:Paths)
        if (!Path.TryLoad()) Errors.Add(FText::FromString(TEXT("Weapon resource could not load: ")+Path.ToString()));
    ValidateSpecification(Errors);
    if (Presentation.LoadoutThumbnail.IsNull() || Presentation.WeaponIconMaterial.IsNull())
        Context.AddWarning(FText::FromString(TEXT("Weapon presentation is missing a loadout thumbnail or HUD icon material.")));
    if (!Presentation.DefaultHiddenMaterialSlots.IsEmpty())
    {
        TArray<USkeletalMesh*> Meshes;
        Meshes.AddUnique(Presentation.WorldMesh.Get());
        Meshes.AddUnique(Presentation.FirstPersonMesh.Get());
        for (const auto& Team:Presentation.TeamMeshes)
        { Meshes.AddUnique(Team.WorldMesh.Get()); Meshes.AddUnique(Team.FirstPersonMesh.Get()); }
        for (const auto* Mesh:Meshes) if (Mesh) for (FName MaterialSlot:Presentation.DefaultHiddenMaterialSlots)
            if (!Mesh->GetMaterials().ContainsByPredicate([MaterialSlot](const FSkeletalMaterial& Material){return Material.MaterialSlotName==MaterialSlot;}))
                Errors.Add(FText::FromString(TEXT("Default-hidden material slot ")+MaterialSlot.ToString()+TEXT(" is absent from ")+Mesh->GetPathName()));
    }
    return FinishValidation(Super::IsDataValid(Context),Errors,Context);
}
#endif

bool URetakeWeaponSpecification::ValidateStartingReferences(TConstArrayView<const URetakeWeaponSpecification*> Inputs,FString& Error)
{
    const auto MatchesPool=[](const URetakeWeaponSpecification& Definition,FName Id)
    {
        const auto* Primary=Definition.Ammo.Get();const auto* Secondary=Definition.SecondaryAmmo.Get();
        return (Primary && Primary->AmmoId==Id) || (Secondary && Secondary->AmmoId==Id);
    };

    Error.Reset();TMap<FName,const URetakeWeaponSpecification*> Definitions;TSet<FName> Pools;
    for (const auto* Definition:Inputs)
    {
        if (!Definition || Definitions.Contains(Definition->WeaponId)) {Error=TEXT("Starting grants require unique loaded definitions.");return false;}
        Definitions.Add(Definition->WeaponId,Definition);
        for (const auto* Ammo:{Definition->Ammo.Get(),Definition->SecondaryAmmo.Get()}) if (Ammo) Pools.Add(Ammo->AmmoId);
    }
    TSet<FName> Active,Done;
    TFunction<bool(FName,int32)> Visit=[&](FName Id,int32 Depth)
    {
        if (Done.Contains(Id)) return true;
        if (Depth>=64 || Active.Contains(Id)) {Error=TEXT("Starting-item graph is cyclic or too deep: ")+Id.ToString();return false;}
        const auto* const* Found=Definitions.Find(Id);
        if (!Found) {Error=TEXT("Missing starting-item definition: ")+Id.ToString();return false;}
        Active.Add(Id);
        for (const auto& Item:(*Found)->StartingItems) if (!Visit(Item.WeaponId,Depth+1)) return false;
        for (const auto& Grant:(*Found)->StartingAmmoGrants)
        {
            if (!Pools.Contains(Grant.AmmoId)) {Error=TEXT("Missing starting ammunition pool: ")+Grant.AmmoId.ToString();return false;}
            if (!Grant.FallbackWeaponId.IsNone())
            {
                const auto* const* Target=Definitions.Find(Grant.FallbackWeaponId);
                if (!Target || !MatchesPool(**Target,Grant.AmmoId))
                {Error=TEXT("Starting ammunition fallback is missing or uses another pool: ")+Grant.FallbackWeaponId.ToString();return false;}
            }
        }
        Active.Remove(Id);Done.Add(Id);return true;
    };
    for (const auto& Pair:Definitions) if (!Visit(Pair.Key,0)) return false;
    return true;
}
