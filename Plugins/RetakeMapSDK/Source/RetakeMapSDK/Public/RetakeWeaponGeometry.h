// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "RetakeWeaponData.h"

class USkeletalMesh;

namespace RetakeWeaponGeometry
{
    /** Reference-pose socket or named bone in weapon space. Does not instantiate components or read a world. */
    RETAKEMAPSDK_API bool SocketTransform(const USkeletalMesh& Mesh,FName Name,FTransform& Out);
    /** Resolves the selected sight through its ordered attachment chain. Loading is explicitly limited to authoring callers. */
    RETAKEMAPSDK_API bool ResolveAimAxis(const USkeletalMesh* WeaponMesh,const FRetakeWeaponAimPoint& WeaponAimPoint,
        const TArray<FRetakeWeaponAttachment>& Attachments,FTransform& OutAimInWeapon,bool bLoadMeshes=false);
    /** First-person preview/runtime solve: places the sight on the camera axis at the authored eye relief. */
    RETAKEMAPSDK_API bool SolveAimRig(const FTransform& SightInArms,float EyeReliefCm,const FVector& ArmsScale,FTransform& OutRig);
}
