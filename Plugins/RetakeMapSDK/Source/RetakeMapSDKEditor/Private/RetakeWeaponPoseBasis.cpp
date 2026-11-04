// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeWeaponPoseBasis.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Engine/SkeletalMesh.h"

RetakeWeaponPoseBaking::FInputs URetakeWeaponPoseBasis::GetInputs() const
{
    RetakeWeaponPoseBaking::FInputs Out;Out.Body=Body;Out.ReferenceMesh=ReferenceMesh;
    Out.StandLower=StandLower;Out.CrouchLower=CrouchLower;Out.StandLean=StandLean;Out.CrouchLean=CrouchLean;
    Out.StandMovingLean=StandMovingLean;Out.CrouchMovingLean=CrouchMovingLean;
    Out.ClearanceStand=ClearanceStand;Out.ClearanceCrouch=ClearanceCrouch;Out.LeanStrength=LeanStrength;Out.SupportSocket=SupportSocket;
    return Out;
}
