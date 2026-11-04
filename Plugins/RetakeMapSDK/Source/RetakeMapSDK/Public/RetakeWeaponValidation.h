// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once

#include "RetakeWeaponData.h"
#include "RetakeWeaponResources.h"
#include "Misc/DataValidation.h"

namespace RetakeWeaponValidation
{
    inline bool SelectAimPointProvider(const FRetakeWeaponAimPoint& WeaponAimPoint,const TArray<FRetakeWeaponAttachment>& Attachments,FRetakeWeaponAimPoint& OutPoint,FName& OutSlot)
    {
        for (int32 I=Attachments.Num()-1;I>=0;--I)
            if (Attachments[I].AimPoint.IsSet()) { OutPoint=Attachments[I].AimPoint; OutSlot=Attachments[I].Slot; return true; }
        if (!WeaponAimPoint.IsSet()) return false;
        OutPoint=WeaponAimPoint; OutSlot=NAME_None; return true;
    }

    struct FWeaponValidation
    {
        TArray<FText>& Errors;
        const int32 InitialCount;
        explicit FWeaponValidation(TArray<FText>& InErrors) : Errors(InErrors), InitialCount(InErrors.Num()) {}
        bool IsValid() const { return Errors.Num() == InitialCount; }
        void Require(bool bCondition, const FString& Message)
        {
            if (!bCondition) Errors.Add(FText::FromString(Message));
        }
        void Nonnegative(double Value, const FString& Field)
        {
            Require(FMath::IsFinite(Value) && Value >= 0, Field + TEXT(" must be finite and nonnegative."));
        }
        void Positive(double Value, const FString& Field)
        {
            Require(FMath::IsFinite(Value) && Value > 0, Field + TEXT(" must be finite and positive."));
        }
        void Range(double Value, double Minimum, double Maximum, const FString& Field)
        {
            Require(FMath::IsFinite(Value) && Value >= Minimum && Value <= Maximum, Field + TEXT(" is outside its allowed finite range."));
        }
        void Identity(FName Value, const FString& Field)
        {
            const FString Name = Value.ToString();
            Require(!Value.IsNone() && Name == Name.TrimStartAndEnd() && !Name.Contains(TEXT(":"))
                && !Name.Contains(TEXT("/")) && !Name.Contains(TEXT("\\")), Field + TEXT(" requires a non-empty stable ID without path/asset-type separators."));
        }
        void Reference(const FSoftObjectPath& Path, const FString& Field, bool bRequired = false)
        {
            if (Path.IsNull())
            {
                Require(!bRequired, Field + TEXT(" needs an asset reference."));
                return;
            }
            Require(Path.IsValid(), Field + TEXT(" has an invalid asset path."));
            const FString Package = Path.GetLongPackageName();
            Require(!Package.StartsWith(TEXT("/MOHPC/")) && !Package.StartsWith(TEXT("/GameImporter/"))
                && !Package.StartsWith(TEXT("/Script/mohue")), Field + TEXT(" still depends on an importer/UE4 runtime mount."));
        }
    };

    inline bool IsSlotValid(ERetakeWeaponSlot Slot)
    {
        return Slot == ERetakeWeaponSlot::Primary || Slot == ERetakeWeaponSlot::Secondary || Slot == ERetakeWeaponSlot::Grenade || Slot == ERetakeWeaponSlot::Equipment;
    }

    inline void ValidateAction(const FRetakeWeaponActionSettings& Action, const FString& Name, FWeaponValidation& V, bool bSecondary)
    {
        V.Require(StaticEnum<ERetakeWeaponAction>()->IsValidEnumValue(static_cast<int64>(Action.Type)), Name + TEXT(" has an invalid action type."));
        V.Require(FMath::IsFinite(Action.PenetrationPower) && Action.PenetrationPower >= 0 && Action.PenetrationPower <= 10, Name + TEXT(".PenetrationPower must be in [0,10]."));
        V.Nonnegative(Action.IntervalSeconds, Name + TEXT(".IntervalSeconds"));
        V.Nonnegative(Action.AmmoCost, Name + TEXT(".AmmoCost"));
        V.Nonnegative(Action.ImpactDelaySeconds, Name + TEXT(".ImpactDelaySeconds"));
        V.Nonnegative(Action.Damage.BaseDamage, Name + TEXT(".Damage.BaseDamage"));
        V.Nonnegative(Action.Damage.HeadMultiplier, Name + TEXT(".Damage.HeadMultiplier"));
        V.Nonnegative(Action.Damage.BodyMultiplier, Name + TEXT(".Damage.BodyMultiplier"));
        V.Nonnegative(Action.Damage.LimbMultiplier, Name + TEXT(".Damage.LimbMultiplier"));
        V.Nonnegative(Action.Damage.MinimumDamage, Name + TEXT(".Damage.MinimumDamage"));
        V.Nonnegative(Action.Damage.FullDamageRangeCm, Name + TEXT(".Damage.FullDamageRangeCm"));
        V.Nonnegative(Action.Damage.MinimumDamageRangeCm, Name + TEXT(".Damage.MinimumDamageRangeCm"));
        V.Nonnegative(Action.RangeCm, Name + TEXT(".RangeCm"));
        V.Range(Action.PelletCount, 1, 64, Name + TEXT(".PelletCount"));
        const auto& Spread = Action.Spread;
        V.Range(Spread.MinDegrees, 0, 89, Name + TEXT(".Spread.MinDegrees"));
        V.Range(Spread.MaxDegrees, Spread.MinDegrees, 89, Name + TEXT(".Spread.MaxDegrees"));
        V.Nonnegative(Spread.PerShotDegrees, Name + TEXT(".Spread.PerShotDegrees"));
        V.Nonnegative(Spread.RecoveryDegreesPerSecond, Name + TEXT(".Spread.RecoveryDegreesPerSecond"));
        V.Nonnegative(Spread.MovingMultiplier, Name + TEXT(".Spread.MovingMultiplier"));
        V.Nonnegative(Spread.CrouchedMultiplier, Name + TEXT(".Spread.CrouchedMultiplier"));
        V.Nonnegative(Spread.ProneMultiplier, Name + TEXT(".Spread.ProneMultiplier"));
        if (Action.Type == ERetakeWeaponAction::None || Action.Type == ERetakeWeaponAction::Melee)
            V.Require(Action.AmmoCost == 0, Name + TEXT(" must not consume ammunition for none/melee."));
        else
            V.Positive(Action.AmmoCost, Name + TEXT(".AmmoCost"));
        if (Action.Type != ERetakeWeaponAction::None)
            V.Positive(Action.IntervalSeconds, Name + TEXT(".IntervalSeconds"));
        if (Action.Type == ERetakeWeaponAction::Hitscan || Action.Type == ERetakeWeaponAction::Pellets || Action.Type == ERetakeWeaponAction::Melee)
            V.Positive(Action.RangeCm, Name + TEXT(".RangeCm"));
        if (Action.Type == ERetakeWeaponAction::Melee)
            V.Require(Action.ImpactDelaySeconds <= Action.IntervalSeconds, Name + TEXT(" melee impact must occur before the action completes."));
        if (Action.Type != ERetakeWeaponAction::Pellets)
            V.Require(Action.PelletCount == 1, Name + TEXT(" only pellet delivery may declare multiple pellets."));
        FString FireModeProblem;
        const bool bFirearm = Action.Type == ERetakeWeaponAction::Hitscan || Action.Type == ERetakeWeaponAction::Pellets
            || Action.Type == ERetakeWeaponAction::Rocket;
        V.Require(Action.FireModes.Validate(bFirearm, bSecondary, FireModeProblem), Name + TEXT(".FireModes: ") + FireModeProblem);
    }

    inline void ValidateAimPoint(const FRetakeWeaponAimPoint& Point, const FString& Owner, FWeaponValidation& V)
    {
        if (!Point.IsSet()) return;
        V.Range(Point.EyeReliefCm, 3, 60, Owner + TEXT(" aim point EyeReliefCm"));
        V.Require(!Point.Offset.ContainsNaN() && Point.Offset.GetRotation().IsNormalized(), Owner + TEXT(" aim point Offset must be finite and normalized."));
    }

    inline void ValidatePresentation(const FRetakeWeaponPresentationSettings& P, FWeaponValidation& V, bool bRequiresMuzzle)
    {
        V.Reference(P.WorldMesh.ToSoftObjectPath(), TEXT("Presentation.WorldMesh"), true);
        V.Reference(P.FirstPersonMesh.ToSoftObjectPath(), TEXT("Presentation.FirstPersonMesh"));
        V.Require(P.DefaultHiddenMaterialSlots.Num()<=64,TEXT("Presentation.DefaultHiddenMaterialSlots supports at most 64 slots."));
        {
            TSet<FName> Hidden;
            for (FName Slot:P.DefaultHiddenMaterialSlots)
            {
                V.Require(!Slot.IsNone() && !Hidden.Contains(Slot),TEXT("Default-hidden material slots must be nonempty and unique."));
                Hidden.Add(Slot);
            }
        }
        {
            TSet<FName> Slots;
            for (const FRetakeWeaponAttachment& A : P.Attachments)
            {
                V.Require(!A.Slot.IsNone() && !A.Socket.IsNone() && !Slots.Contains(A.Slot), TEXT("Every attachment needs a unique slot and a socket."));
                V.Require(A.ParentSlot.IsNone() || Slots.Contains(A.ParentSlot), FString::Printf(TEXT("Attachment %s must be listed after its parent slot."), *A.Slot.ToString()));
                V.Reference(A.Mesh.ToSoftObjectPath(), TEXT("Presentation.Attachments.Mesh"), true);
                ValidateAimPoint(A.AimPoint, TEXT("Attachment ") + A.Slot.ToString(), V);
                Slots.Add(A.Slot);
            }
        }
        // A merged world mesh is third-person only: first person must keep its own mesh and fit the parts itself.
        V.Require(!P.bWorldMeshIncludesAttachments || P.Attachments.IsEmpty()
            || (!P.FirstPersonMesh.IsNull() && P.FirstPersonMesh.ToSoftObjectPath()!=P.WorldMesh.ToSoftObjectPath()),
            TEXT("bWorldMeshIncludesAttachments needs a FirstPersonMesh other than the WorldMesh: the world mesh already contains the parts."));
        V.Reference(P.ProjectileMesh.ToSoftObjectPath(), TEXT("Presentation.ProjectileMesh"));
        V.Reference(P.ProjectileStaticMesh.ToSoftObjectPath(), TEXT("Presentation.ProjectileStaticMesh"));
        V.Require(P.ProjectileMesh.IsNull() || P.ProjectileStaticMesh.IsNull(), TEXT("Choose one projectile visual: skeletal or static mesh, not both."));
        {
            TSet<int32> Teams;
            for (const FRetakeWeaponTeamMeshes& T : P.TeamMeshes)
            {
                V.Require(T.Team>=0 && !Teams.Contains(T.Team), TEXT("Presentation.TeamMeshes needs one entry per team, each with a team index of 0 or more."));
                Teams.Add(T.Team);
                V.Reference(T.WorldMesh.ToSoftObjectPath(), TEXT("Presentation.TeamMeshes.WorldMesh"));
                V.Reference(T.FirstPersonMesh.ToSoftObjectPath(), TEXT("Presentation.TeamMeshes.FirstPersonMesh"));
                V.Reference(T.ProjectileMesh.ToSoftObjectPath(), TEXT("Presentation.TeamMeshes.ProjectileMesh"));
                V.Reference(T.ProjectileStaticMesh.ToSoftObjectPath(), TEXT("Presentation.TeamMeshes.ProjectileStaticMesh"));
                V.Require(T.ProjectileMesh.IsNull() || T.ProjectileStaticMesh.IsNull(), TEXT("Choose one team projectile visual: skeletal or static mesh, not both."));
            }
        }
        V.Reference(P.CharacterProfile.ToSoftObjectPath(), TEXT("Presentation.CharacterProfile"));
        V.Require(!P.GripSocket.IsNone(), TEXT("Presentation.GripSocket needs an explicit attachment name."));
        if (bRequiresMuzzle) V.Require(!P.MuzzleSocket.IsNone(), TEXT("Presentation.MuzzleSocket needs an explicit attachment name."));
        V.Require(P.WorldOffset.IsValid() && P.FirstPersonOffset.IsValid(), TEXT("Presentation offsets must contain finite values and normalized rotations."));
        const FVector WorldScale = P.WorldOffset.GetScale3D();
        const FVector FirstScale = P.FirstPersonOffset.GetScale3D();
        V.Require(WorldScale.GetMin() > 0 && FirstScale.GetMin() > 0, TEXT("Presentation offsets need positive nonzero scales."));
#define VALIDATE_PRESENTATION_REF(Field) V.Reference(P.Field.ToSoftObjectPath(), TEXT("Presentation." #Field))
        VALIDATE_PRESENTATION_REF(IdleAnimation); VALIDATE_PRESENTATION_REF(FireAnimation); VALIDATE_PRESENTATION_REF(ReloadAnimation);
        VALIDATE_PRESENTATION_REF(ReloadInsertAnimation); VALIDATE_PRESENTATION_REF(ReloadEndAnimation);
        VALIDATE_PRESENTATION_REF(FirstPersonReloadAnimation);
        VALIDATE_PRESENTATION_REF(FirstPersonBashAnimation); VALIDATE_PRESENTATION_REF(ThirdPersonBashAnimation);
        V.Require(P.BashVariants.Num()<=8,TEXT("BashVariants exceeds the recorded three-bit variant index."));
        for (const FRetakeBashAnimationVariant& Variant:P.BashVariants)
        {
            V.Require(!Variant.FirstPersonAnimation.IsNull() && !Variant.ThirdPersonAnimation.IsNull(),TEXT("BashVariants needs paired first/third-person clips."));
            V.Reference(Variant.FirstPersonAnimation.ToSoftObjectPath(),TEXT("Presentation.BashVariants.FirstPersonAnimation"));
            V.Reference(Variant.ThirdPersonAnimation.ToSoftObjectPath(),TEXT("Presentation.BashVariants.ThirdPersonAnimation"));
            V.Require(FMath::IsFinite(Variant.DurationSeconds) && Variant.DurationSeconds>=.01f && Variant.DurationSeconds<=65.f
                && FMath::IsFinite(Variant.ImpactDelaySeconds) && Variant.ImpactDelaySeconds>=0.f
                && Variant.ImpactDelaySeconds<Variant.DurationSeconds,TEXT("BashVariants needs a positive duration and an impact inside its action window."));
        }
        V.Reference(P.BashProp.Mesh.ToSoftObjectPath(),TEXT("Presentation.BashProp.Mesh"));
        V.Require(P.BashProp.Mesh.IsNull() || (!P.BashProp.Bone.IsNone() && FMath::IsFinite(P.BashProp.VisibleStart)
            && FMath::IsFinite(P.BashProp.VisibleEnd) && P.BashProp.VisibleStart>=0.f && P.BashProp.VisibleEnd<=1.f
            && P.BashProp.VisibleEnd>P.BashProp.VisibleStart && P.BashProp.FirstPersonOffset.IsValid()
            && P.BashProp.ThirdPersonOffset.IsValid()),TEXT("Bash prop requires a bone, valid offsets and an ordered visibility window in [0,1]."));
        VALIDATE_PRESENTATION_REF(FirstPersonReloadStart); VALIDATE_PRESENTATION_REF(FirstPersonReloadInsert); VALIDATE_PRESENTATION_REF(FirstPersonReloadEnd);
        VALIDATE_PRESENTATION_REF(ThirdPersonReloadStart); VALIDATE_PRESENTATION_REF(ThirdPersonReloadInsert); VALIDATE_PRESENTATION_REF(ThirdPersonReloadEnd);
        VALIDATE_PRESENTATION_REF(ReloadInsertCue); VALIDATE_PRESENTATION_REF(ReloadEndCue); VALIDATE_PRESENTATION_REF(ShellEjectEffect); VALIDATE_PRESENTATION_REF(ShellEjectMesh);
        VALIDATE_PRESENTATION_REF(PrimaryFireCue); VALIDATE_PRESENTATION_REF(SecondaryFireCue); VALIDATE_PRESENTATION_REF(DryFireCue);
        VALIDATE_PRESENTATION_REF(FireSelectorCue);
        VALIDATE_PRESENTATION_REF(RechamberSound); VALIDATE_PRESENTATION_REF(RechamberAttenuation);
        if (!P.RechamberSound.IsNull())
        {
            V.Reference(P.FireAnimation.ToSoftObjectPath(), TEXT("Presentation.FireAnimation for rechamber sound"), true);
            V.Reference(P.RechamberAttenuation.ToSoftObjectPath(), TEXT("Presentation.RechamberAttenuation"), true);
        }
        VALIDATE_PRESENTATION_REF(ReloadCue); VALIDATE_PRESENTATION_REF(EquipCue); VALIDATE_PRESENTATION_REF(HolsterCue); VALIDATE_PRESENTATION_REF(PinCue);
        VALIDATE_PRESENTATION_REF(ThrowCue); VALIDATE_PRESENTATION_REF(CookTimerCue); VALIDATE_PRESENTATION_REF(ExplosionCue);
        VALIDATE_PRESENTATION_REF(Attenuation); VALIDATE_PRESENTATION_REF(Concurrency);
        VALIDATE_PRESENTATION_REF(MuzzleEffect); VALIDATE_PRESENTATION_REF(ExplosionEffect); VALIDATE_PRESENTATION_REF(SmokeEffect); VALIDATE_PRESENTATION_REF(SmokeLoopSound);
        VALIDATE_PRESENTATION_REF(ExplosionAirEffect);
        V.Range(P.ExplosionAirEffectScale,0.1,2,TEXT("Presentation.ExplosionAirEffectScale"));
        if (!P.SmokeLoopSound.IsNull()) VALIDATE_PRESENTATION_REF(SmokeLoopAttenuation);
        VALIDATE_PRESENTATION_REF(FireEffect); VALIDATE_PRESENTATION_REF(FireLoopSound); VALIDATE_PRESENTATION_REF(FlashScreenMaterial); VALIDATE_PRESENTATION_REF(FlashRingSound);
        VALIDATE_PRESENTATION_REF(Tracer);
        VALIDATE_PRESENTATION_REF(Icon); VALIDATE_PRESENTATION_REF(ScopeMaterial);
        VALIDATE_PRESENTATION_REF(WeaponIconMaterial); VALIDATE_PRESENTATION_REF(ReticleMaterial);
#undef VALIDATE_PRESENTATION_REF
        const FReticleParams& Reticle = P.ReticleParams;
#define VALIDATE_RETICLE(Field) V.Require(FMath::IsFinite(Reticle.Field), TEXT("Presentation.ReticleParams." #Field " must be finite."))
        VALIDATE_RETICLE(ShapeCutFactor_RoundFired); VALIDATE_RETICLE(ShapeCutFactor_Aiming); VALIDATE_RETICLE(ShapeCutFactor_NotAiming);
        VALIDATE_RETICLE(ScaleFactor_RoundFired); VALIDATE_RETICLE(ScaleFactor_Aiming); VALIDATE_RETICLE(ScaleFactor_NotAiming);
#undef VALIDATE_RETICLE
        V.Nonnegative(Reticle.RoundFiredInterpSpeed, TEXT("Presentation.ReticleParams.RoundFiredInterpSpeed"));
        V.Nonnegative(Reticle.AimingInterpSpeed, TEXT("Presentation.ReticleParams.AimingInterpSpeed"));
    }

#if WITH_EDITOR
    template<typename T> T* LoadForValidation(const TSoftObjectPtr<T>& Ref, const TCHAR* Field, FWeaponValidation& V)
    {
        if (Ref.IsNull()) return nullptr;
        T* Asset = Ref.LoadSynchronous();
        V.Require(IsValid(Asset), FString(Field) + TEXT(" could not be loaded as its declared native asset type: ") + Ref.ToString());
        return Asset;
    }

    inline EDataValidationResult FinishValidation(EDataValidationResult SuperResult, const TArray<FText>& Errors, FDataValidationContext& Context)
    {
        for (const FText& Error : Errors) Context.AddError(Error);
        return SuperResult == EDataValidationResult::Invalid || !Errors.IsEmpty() ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
    }
#endif
}
