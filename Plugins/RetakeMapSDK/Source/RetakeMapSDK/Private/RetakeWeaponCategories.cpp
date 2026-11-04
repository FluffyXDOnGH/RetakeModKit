// Copyright 2026 Codnyx. All Rights Reserved.
#include "NativeGameplayTags.h"

// Shared authoring vocabulary. Category tags never identify a reserve pool; URetakeAmmoDefinition::AmmoId does.
namespace
{
    UE_DEFINE_GAMEPLAY_TAG_STATIC(WeaponNone,"Weapon.Type.None");
    UE_DEFINE_GAMEPLAY_TAG_STATIC(WeaponPistol,"Weapon.Type.Pistol");
    UE_DEFINE_GAMEPLAY_TAG_STATIC(WeaponRifle,"Weapon.Type.Rifle");
    UE_DEFINE_GAMEPLAY_TAG_STATIC(AmmoSMG,"Ammo.Type.smg");
    UE_DEFINE_GAMEPLAY_TAG_STATIC(AmmoMG,"Ammo.Type.mg");
    UE_DEFINE_GAMEPLAY_TAG_STATIC(AmmoRifle,"Ammo.Type.rifle");
    UE_DEFINE_GAMEPLAY_TAG_STATIC(AmmoPistol,"Ammo.Type.pistol");
    UE_DEFINE_GAMEPLAY_TAG_STATIC(AmmoShotgun,"Ammo.Type.shotgun");
    UE_DEFINE_GAMEPLAY_TAG_STATIC(AmmoHeavy,"Ammo.Type.heavy");
    UE_DEFINE_GAMEPLAY_TAG_STATIC(AmmoGrenade,"Ammo.Type.grenade");
    UE_DEFINE_GAMEPLAY_TAG_STATIC(AmmoRifle545,"Ammo.Type.Rifle545");
    UE_DEFINE_GAMEPLAY_TAG_STATIC(AmmoRifle556,"Ammo.Type.Rifle556");
    UE_DEFINE_GAMEPLAY_TAG_STATIC(AmmoRifle762,"Ammo.Type.Rifle762");
}
