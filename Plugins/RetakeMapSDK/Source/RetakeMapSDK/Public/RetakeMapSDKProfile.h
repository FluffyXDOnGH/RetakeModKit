// Copyright 2026 Codnyx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FConfigCacheIni;

/**
 * Identity of the project settings that change cooked map content: collision channels and profiles, physical
 * surfaces, renderer settings and target shader formats (Docs/Proposals/MapModding.md section 4).
 * Full hashes remain authoring diagnostics. Runtime checks structural collision/surface definitions; other cook settings
 * belong to the reviewed engine cook record rather than an all-or-nothing profile equality gate.
 */
struct RETAKEMAPSDK_API FRetakeMapSDKProfile
{
    /** Sorted "Section|Key=Value" lines for one ini platform ("Windows", "Linux"). */
    static FString BuildCanonicalText(FConfigCacheIni& Config, const FString& IniPlatformName);
    /** Lower-case BLAKE3 hex of BuildCanonicalText. */
    static FString ComputeHash(FConfigCacheIni& Config, const FString& IniPlatformName);
    /** This process's ini platform name (FPlatformProperties::IniPlatformName). */
    static FString GetRuntimeIniPlatformName();
    /** Hash of GConfig for this process's own platform. */
    static FString ComputeRuntimeHash();
    /** Collision/surface contract: adding entries is allowed, changing an existing definition is not. */
    static TArray<FString> StructuralEntries(FConfigCacheIni& Config);
};
