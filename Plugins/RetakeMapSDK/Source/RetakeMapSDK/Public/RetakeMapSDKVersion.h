// Copyright 2026 Codnyx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Compatibility identity shared by the Retake game and the Retake Map Kit (Docs/Proposals/MapModding.md section 3).
 * Fresh mod packages use tagged serialization. SDK readability is a range; engine-cooked data has a separate reviewed
 * platform compatibility record. Game version and editor BuildId are not map-compatibility identities.
 */
namespace RetakeMapSDK
{
    /** Element, link and manifest contract of the SDK classes that maps contain. */
    inline constexpr int32 ContractVersion = 39; // Explicit map-owned minefield presentation; no implicit weapon resource lookup.
    /** Raise only when a reviewed breaking change makes older tagged maps unreadable or changes their meaning. */
    inline constexpr int32 OldestReadableContract = 39; // Unpublished baseline: old minefields borrowed weapon resources that no longer exist.
    /** Weapon exports use the same fresh development baseline; prior experimental revisions require re-export. */
    inline constexpr int32 MinimumWeaponContract = 39;
    /** Encoding of manifest.json. */
    inline constexpr int32 ManifestSchemaVersion = 3; // Fresh format; optional metadata additions do not bump this encoding.
    /**
     * Base-game package roots a mod may reference besides its own. Each referenced package, like each /Script module, must also be in
     * the shipped lists of the release the kit was staged from (SDK.lock.json "shipped", written by packaging; MKI-030).
     */
    inline constexpr const TCHAR* AllowedForeignRoots[] = { TEXT("/Engine/"), TEXT("/Niagara/"), TEXT("/ACLPlugin/"), TEXT("/RetakeMapContent/") };
    /** Native packages whose actor classes are map elements: the Map Kit validator runs their own checks on every placed instance. */
    inline constexpr const TCHAR* ElementScriptPackages[] = { TEXT("/Script/RetakeMapSDK"), TEXT("/Script/RetakeDestruction") };
}
