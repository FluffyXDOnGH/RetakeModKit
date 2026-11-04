// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeWeaponResources.h"
#include "RetakeWeaponValidation.h"
#include "RetakeWeaponPack.h"
#include "RetakeMapSDKManifest.h"
#include "UObject/Package.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimMontage.h"

using namespace RetakeWeaponValidation;

FPrimaryAssetId URetakeWeaponAnimationProfile::GetPrimaryAssetId() const
{
    return ProfileId.IsNone() ? FPrimaryAssetId() : FPrimaryAssetId(TEXT("RetakeWeaponAnimation"), ProfileId);
}

FPrimaryAssetId URetakeAmmoDefinition::GetPrimaryAssetId() const
{
    return AmmoId.IsNone() ? FPrimaryAssetId() : FPrimaryAssetId(TEXT("RetakeAmmo"), AmmoId);
}

bool URetakeAmmoDefinition::ValidateDefinition(TArray<FText>& OutErrors) const
{
    FWeaponValidation V(OutErrors);
    V.Identity(AmmoId, TEXT("AmmoId"));
    V.Require(!DisplayName.IsEmpty(), TEXT("Ammo DisplayName is required."));
    V.Require(AmmoType.IsValid(), TEXT("AmmoType requires a registered descriptive ammunition category."));
    V.Nonnegative(ReserveCapacity, TEXT("ReserveCapacity"));
    return V.IsValid();
}

bool URetakeAmmoDefinition::ValidateForPack(const FString& PackId,TArray<FText>& OutErrors) const
{
    FWeaponValidation V(OutErrors);
    ValidateDefinition(OutErrors);
    FString Owner,Local;
    const bool Qualified=AmmoId.ToString().Split(TEXT("."),&Owner,&Local)
        && Owner.Equals(PackId,ESearchCase::IgnoreCase) && URetakeWeaponPack::IsValidLocalId(Local.ToLower());
    V.Require(FRetakeModManifest::IsValidModId(PackId) && Qualified,
        TEXT("Mod ammunition requires a pack-qualified AmmoId (PackId.local_id); categories do not share ammunition."));
    V.Require(GetClass()==StaticClass(),TEXT("Mod ammunition must use the supported native definition class."));
    V.Require(GetOutermost()->GetName().StartsWith(TEXT("/")+PackId+TEXT("/"),ESearchCase::IgnoreCase),
        TEXT("Mod ammunition definitions must be owned by their weapon pack."));
    return V.IsValid();
}

#if WITH_EDITOR
EDataValidationResult URetakeWeaponAnimationProfile::IsDataValid(FDataValidationContext& Context) const
{
    TArray<FText> Errors;
    FWeaponValidation V(Errors);
    const auto ValidateSteps=[&V](const UAnimSequence* Sequence,const TCHAR* Name)
    {
        if (!Sequence) return; // Authored locomotion loops are optional.
        TArray<float> Times;
        for (const auto& Marker:Sequence->AuthoredSyncMarkers)
            if (Marker.MarkerName==TEXT("RetakeStep")) Times.Add(Marker.Time);
        if (Times.IsEmpty())
            for (const auto& Marker:Sequence->AuthoredSyncMarkers)
                if (Marker.MarkerName==TEXT("L") || Marker.MarkerName==TEXT("R")) Times.Add(Marker.Time);
        Times.Sort();
        V.Require(Times.Num()>=2,FString(Name)+TEXT(" needs authored step contacts (RetakeStep or L/R markers)."));
        for (int32 I=0;I<Times.Num();++I)
            V.Require(FMath::IsFinite(Times[I]) && Times[I]>=0 && Times[I]<Sequence->GetPlayLength()
                && (I==0 || Times[I]-Times[I-1]>1.e-4f),FString(Name)+TEXT(" has invalid or duplicate step markers."));
    };
    ValidateSteps(FirstPersonAnims.RunAnim,TEXT("FirstPersonAnims.RunAnim"));
    ValidateSteps(FirstPersonAnims.SprintAnim,TEXT("FirstPersonAnims.SprintAnim"));
    const auto ValidatePutAway=[&V](const UAnimMontage* Montage,const TCHAR* Name)
    {
        V.Require(IsValid(Montage),FString(Name)+TEXT(" must be assigned."));
        if (!IsValid(Montage)) return;
        V.Require(Montage->GetSkeleton()!=nullptr,FString(Name)+TEXT(" must have a skeleton."));
        V.Positive(Montage->GetPlayLength(),FString(Name)+TEXT(" length"));
        V.Positive(Montage->RateScale,FString(Name)+TEXT(" rate scale"));
        V.Require(!Montage->SlotAnimTracks.IsEmpty(),FString(Name)+TEXT(" must have an animation slot."));
    };
    if (bNoHandlingAnimations)
    {
        for (const auto* Montages:{&FirstPersonMontages,&ThirdPersonMontages})
            V.Require(!Montages->EquipMontage && !Montages->HolsterMontage && !Montages->DropMontage,
                TEXT("No-handling-animation profiles cannot also declare draw/holster/drop montages."));
    }
    else
    {
        ValidatePutAway(FirstPersonMontages.HolsterMontage,TEXT("FirstPersonMontages.HolsterMontage"));
        ValidatePutAway(FirstPersonMontages.DropMontage,TEXT("FirstPersonMontages.DropMontage"));
        ValidatePutAway(ThirdPersonMontages.HolsterMontage,TEXT("ThirdPersonMontages.HolsterMontage"));
        ValidatePutAway(ThirdPersonMontages.DropMontage,TEXT("ThirdPersonMontages.DropMontage"));
    }
    if (ThirdPersonBoltCycle)
    {
        ValidatePutAway(ThirdPersonBoltCycle,TEXT("ThirdPersonBoltCycle"));
        const auto* Cycle=ThirdPersonBoltCycle.Get();
        V.Require(Cycle->GetPlayLength()/FMath::Max(.01f,Cycle->RateScale)<=10.f,TEXT("ThirdPersonBoltCycle must finish within ten seconds."));
        V.Require(Cycle->SlotAnimTracks.Num()==1,TEXT("ThirdPersonBoltCycle requires one upper-body slot (no independent aiming layer)."));
        for (const auto& Slot:Cycle->SlotAnimTracks)
            V.Require(Slot.SlotName==TEXT("DefaultSlot") || Slot.SlotName==TEXT("UpperBody"),TEXT("ThirdPersonBoltCycle must use DefaultSlot or UpperBody."));
    }
    if (FirstPersonMontages.InspectMontage || ThirdPersonMontages.InspectMontage)
    {
        ValidatePutAway(FirstPersonMontages.InspectMontage,TEXT("FirstPersonMontages.InspectMontage"));
        ValidatePutAway(ThirdPersonMontages.InspectMontage,TEXT("ThirdPersonMontages.InspectMontage"));
    }
    return FinishValidation(Super::IsDataValid(Context),Errors,Context);
}

EDataValidationResult URetakeAmmoDefinition::IsDataValid(FDataValidationContext& Context) const
{
    TArray<FText> Errors;
    FString Owner,Relative;
    const FString Package=GetOutermost()->GetName();
    if (Package.Mid(1).Split(TEXT("/"),&Owner,&Relative) && Owner.StartsWith(TEXT("RetakeMod_"),ESearchCase::IgnoreCase))
        ValidateForPack(Owner,Errors);
    else ValidateDefinition(Errors);
    return FinishValidation(Super::IsDataValid(Context), Errors, Context);
}
#endif
