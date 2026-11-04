// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeWeaponData.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "NiagaraSystem.h"
#include "NiagaraDataInterfaceArrayFloat.h"

bool FRetakeSmokeRules::ValidateEmission(FString& Error) const
{
    Error.Reset();
    if (!FMath::IsFinite(EmitterGravityCmPerSecondSquared) || EmitterGravityCmPerSecondSquared<0 || EmitterGravityCmPerSecondSquared>20000
        || !FMath::IsFinite(EmitterMaximumSpeedCmPerSecond) || EmitterMaximumSpeedCmPerSecond<0 || EmitterMaximumSpeedCmPerSecond>50000
        || !FMath::IsFinite(EmitterCollisionRadiusCm) || EmitterCollisionRadiusCm<0.1f || EmitterCollisionRadiusCm>100
        || InitialPuffCount<0 || InitialPuffCount>MaximumTrackedPuffs)
    { Error=TEXT("Smoke emitter gravity, radius or initial puff count is out of range.");return false; }
    if (!bTrackEmissionOrigins && (EmitterGravityCmPerSecondSquared!=0 || EmitterMaximumSpeedCmPerSecond!=0 || InitialPuffCount!=6))
    { Error=TEXT("Smoke emitter motion or a custom initial puff count requires tracked emission origins.");return false; }
    if (bTrackEmissionOrigins && (!FMath::IsFinite(EmissionSeconds) || EmissionSeconds<=0 || EmissionSeconds>120
        || !FMath::IsFinite(PuffsPerSecond) || PuffsPerSecond<=0 || PuffsPerSecond>60
        || FMath::CeilToDouble(double(EmissionSeconds)*PuffsPerSecond)+InitialPuffCount>MaximumTrackedPuffs))
    { Error=TEXT("Tracked smoke emission must fit its bounded 512-puff history.");return false; }
    return true;
}

bool FRetakeSmokeRules::ValidateEmissionPresentation(const UNiagaraSystem* System,FString& Error) const
{
    Error.Reset();
    if (!bTrackEmissionOrigins) return true;
    if (!System) {Error=TEXT("Tracked smoke needs its loaded Niagara emission-history interface.");return false;}
    const auto& Parameters=System->GetExposedParameters();
    const FNiagaraVariable Origins(FNiagaraTypeDefinition(UNiagaraDataInterfaceArrayFloat3::StaticClass()),TEXT("User.EmissionOriginOffsets"));
    const FNiagaraVariable Version(FNiagaraTypeDefinition::GetIntDef(),TEXT("User.EmissionHistoryVersion"));
    const FNiagaraVariable Count(FNiagaraTypeDefinition::GetIntDef(),TEXT("User.InitialPuffCount"));
    const int32 OriginsIndex=Parameters.IndexOf(Origins);
    if (OriginsIndex==INDEX_NONE || !Cast<UNiagaraDataInterfaceArrayFloat3>(Parameters.GetDataInterface(OriginsIndex)) || Parameters.IndexOf(Version)==INDEX_NONE
        || Parameters.GetParameterValue<int32>(Version)!=1 || Parameters.IndexOf(Count)==INDEX_NONE)
    {Error=TEXT("Tracked smoke requires Niagara emission-history version1, vector-array origins and InitialPuffCount.");return false;}
    return true;
}

USkeletalMesh* FRetakeWeaponPresentationSettings::GetWorldMesh(int32 Team) const
{
    const auto* T=FindTeamMeshes(Team);
    return T && !T->WorldMesh.IsNull() ? T->WorldMesh.Get() : WorldMesh.Get();
}

USkeletalMesh* FRetakeWeaponPresentationSettings::GetFirstPersonMesh(int32 Team) const
{
    const auto* T=FindTeamMeshes(Team);
    return T && !T->FirstPersonMesh.IsNull() ? T->FirstPersonMesh.Get() : FirstPersonMesh.Get();
}

USkeletalMesh* FRetakeWeaponPresentationSettings::GetProjectileMesh(int32 Team) const
{
    // A team variant is one visual: a static team projectile replaces the default skeletal one too.
    const auto* T=FindTeamMeshes(Team);
    if (T && (!T->ProjectileMesh.IsNull() || !T->ProjectileStaticMesh.IsNull())) return T->ProjectileMesh.Get();
    return ProjectileMesh.Get();
}

UStaticMesh* FRetakeWeaponPresentationSettings::GetProjectileStaticMesh(int32 Team) const
{
    const auto* T=FindTeamMeshes(Team);
    if (T && (!T->ProjectileMesh.IsNull() || !T->ProjectileStaticMesh.IsNull())) return T->ProjectileStaticMesh.Get();
    return ProjectileStaticMesh.Get();
}

FTransform FRetakeWeaponPresentationSettings::ResolveFirstPersonRigTransform(const FTransform& SharedRig) const
{
    if (FirstPersonRigOffset.ContainsNaN() || !FirstPersonRigOffset.GetRotation().IsNormalized()
        || FirstPersonRigOffset.GetScale3D().GetMin() <= 0.0 || FirstPersonRigOffset.Equals(FTransform::Identity)) return SharedRig;
    // Compose orientation in camera space without rotating the shared rig's large root-position offset.
    // Ordinary transform multiplication would swing that position around the camera or rotate translation into mesh axes.
    return FTransform((FirstPersonRigOffset.GetRotation() * SharedRig.GetRotation()).GetNormalized(),
        SharedRig.GetTranslation() + FirstPersonRigOffset.GetTranslation(),
        SharedRig.GetScale3D() * FirstPersonRigOffset.GetScale3D());
}

FRetakeWeaponViewKickSettings FRetakeWeaponViewKickSettings::WithImpulseScale(float Scale) const
{
    FRetakeWeaponViewKickSettings Scaled = *this;
    for (float* Value : {&Scaled.PitchMinDegrees, &Scaled.PitchMaxDegrees, &Scaled.YawMinDegrees, &Scaled.YawMaxDegrees,
        &Scaled.HipPitchMinDegreesPerSecond, &Scaled.HipPitchMaxDegreesPerSecond, &Scaled.HipYawMinDegreesPerSecond, &Scaled.HipYawMaxDegreesPerSecond,
        &Scaled.AdsPitchMinDegreesPerSecond, &Scaled.AdsPitchMaxDegreesPerSecond, &Scaled.AdsYawMinDegreesPerSecond, &Scaled.AdsYawMaxDegreesPerSecond})
        *Value *= Scale;
    return Scaled;
}

FRetakeWeaponGunKickSettings FRetakeWeaponGunKickSettings::WithImpulseScale(float Scale) const
{
    FRetakeWeaponGunKickSettings Scaled = *this;
    for (float* Value : {&Scaled.HipPitchMinDegreesPerSecond, &Scaled.HipPitchMaxDegreesPerSecond, &Scaled.HipYawMinDegreesPerSecond,
        &Scaled.HipYawMaxDegreesPerSecond, &Scaled.AdsPitchMinDegreesPerSecond, &Scaled.AdsPitchMaxDegreesPerSecond,
        &Scaled.AdsYawMinDegreesPerSecond, &Scaled.AdsYawMaxDegreesPerSecond})
        *Value *= Scale;
    return Scaled;
}



float FRetakeWeaponDamageSettings::MaximumMultiplier() const
{
    const float Basic=FMath::Max3(HeadMultiplier,BodyMultiplier,LimbMultiplier);
    return UseDetailedHitLocations ? FMath::Max3(Basic,FMath::Max3(NeckMultiplier,MidTorsoMultiplier,LowerTorsoMultiplier),
        FMath::Max3(PelvisMultiplier,UpperLimbMultiplier,FMath::Max(LowerLimbMultiplier,ExtremityMultiplier))) : Basic;
}

bool FRetakeWeaponProjectileSettings::ValidateCollision(FString& Error) const
{
    Error.Reset();
    if (!FMath::IsFinite(CollisionRadiusCm) || CollisionRadiusCm<0
        || CollisionBoxHalfExtentCm.ContainsNaN() || CollisionBoxHalfExtentCm.GetMin()<0
        || (bAxisAlignedBoxCollision && CollisionBoxHalfExtentCm.GetMin()<=0))
    { Error=TEXT("Projectile collision needs finite nonnegative dimensions and positive box half extents when enabled.");return false; }
    return true;
}

bool FRetakeProjectileDriftSettings::Validate(FString& Error) const
{
    Error.Reset();
    const auto Range=[](float V,float Low,float High){return FMath::IsFinite(V) && V>=Low && V<=High;};
    if (!Range(StartDelaySeconds,0,119) || !Range(UpdateIntervalSeconds,.01f,1)
        || !Range(InitialAngleDegrees,0,90) || !Range(AngleGrowthPerUpdate,0,30)
        || !Range(SpeedRetentionPerUpdate,.01f,1) || !Range(MinimumSteeringSpeedCmPerSecond,0,1000000)
        || !Range(FallingGravityCmPerSecondSquared,0,20000) || !Range(MaximumAxisSpeedCmPerSecond,0,1000000))
    {Error=TEXT("Projectile drift values must be finite and within their documented bounds.");return false;}
    if (bEnabled && (InitialAngleDegrees<=0 || SpeedRetentionPerUpdate>=1 || MinimumSteeringSpeedCmPerSecond<=0))
    {Error=TEXT("Enabled projectile drift requires positive initial angle/minimum speed and speed retention below one.");return false;}
    return true;
}

bool FRetakeWeaponProjectileSettings::ValidateDrift(FString& Error) const
{
    if (!Drift.Validate(Error)) return false;
    if (!Drift.bEnabled) return true;
    if (!bOverrideGravity || GravityCmPerSecondSquared!=0 || MaxSpeedCmPerSecond!=0 || bShouldBounce || !bDetonateOnImpact
        || Drift.StartDelaySeconds>=LifetimeSeconds || Drift.MinimumSteeringSpeedCmPerSecond>InitialSpeedCmPerSecond
        || (Drift.MaximumAxisSpeedCmPerSecond>0 && Drift.MaximumAxisSpeedCmPerSecond<InitialSpeedCmPerSecond))
    {Error=TEXT("Drift needs impact detonation without bounce, explicit zero baseline gravity, no magnitude speed cap, a delay before expiry and compatible speed bounds.");return false;}
    return true;
}


