// Copyright 2026 Codnyx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RetakeMapCompatibility.h"
#include "RetakeWeaponPack.h"
#include "RetakeMapSDKManifest.generated.h"

/** One map a mod adds (Docs/Proposals/MapModding.md section 8). */
USTRUCT()
struct RETAKEMAPSDK_API FRetakeModMap
{
    GENERATED_BODY()
    /** Catalogue identity, "<ModId>.<MapName>". */
    UPROPERTY() FString MapId;
    /** Long package name inside the mod root, e.g. /RetakeMod_Probe/Maps/Probe. */
    UPROPERTY() FString PackageName;
    /** From the map's Retake Map Settings (schema 2). */
    UPROPERTY() FString DisplayName;
    /** Requested game-mode IDs; the game keeps only modes it implements. */
    UPROPERTY() TArray<FString> SupportedModes;
    /** "Baked" or "Unbaked" (MKD-024). */
    UPROPERTY() FString Visibility;
    /** Required for Baked maps; an exact object path in this mod, never a base-game lookup. */
    UPROPERTY() FString VisibilityBake;
    // Presentation (schema 2): what the game shows before and while the map loads. Paths are object paths inside the mod root;
    // empty means none, never a built-in fallback. Every named package must be in the mod's own container.
    /** Optional; replicates in the match catalogue. */
    UPROPERTY() FString Description;
    UPROPERTY() FString LoadingArt;
    UPROPERTY() FString Preview;
    /** The mod's URetakeMinimapDefinition for this map, written by -run=RetakeMinimapBake. */
    UPROPERTY() FString Minimap;
    /** Default (exterior) URetakeAmbienceProfile. */
    UPROPERTY() FString Ambience;
    /** Required current acoustic bake; identical to the placed ARetakeAcousticMap's hard Data reference. */
    UPROPERTY() FString Acoustics;
    /** Optional author recommendations. Omitted fields preserve maps exported before loadout policies existed. */
    UPROPERTY() FString DefaultLoadoutPolicy;
    UPROPERTY() TMap<FString, FString> ModeLoadoutPolicies;

    /** Long package names of the presentation paths that are set. */
    TArray<FString> GetPresentationPackages() const;
};

/** One cooked container file for one cook platform ("Windows", "LinuxServer"). */
USTRUCT()
struct RETAKEMAPSDK_API FRetakeModArtifact
{
    GENERATED_BODY()
    UPROPERTY() FString Platform;
    /** Relative to the mod folder; forward slashes, never absolute or "..". */
    UPROPERTY() FString RelativePath;
    UPROPERTY() int64 Size = 0;
    /** Lower-case BLAKE3 hex. */
    UPROPERTY() FString Hash;
};

/** manifest.json of an exported mod revision. */
USTRUCT()
struct RETAKEMAPSDK_API FRetakeModManifest
{
    GENERATED_BODY()
    UPROPERTY() int32 SchemaVersion = 0;
    /** "RetakeMod_<Name>"; also the plugin name and its /<ModId>/ package root. */
    UPROPERTY() FString ModId;
    /** Immutable export revision, [A-Za-z0-9]{1,32}. */
    UPROPERTY() FString Revision;
    UPROPERTY() FString DisplayName;
    UPROPERTY() FString Author;
    UPROPERTY() int32 SdkContractVersion = -1;
    /** RetakeMap or RetakeWeapons. Old experimental exports have no reader or migration path. */
    UPROPERTY() FString Format = TEXT("RetakeMap");
    /** RetakeWeapons only: the owned URetakeWeaponPack catalogue. No map is required or permitted in that format. */
    UPROPERTY() FString WeaponPack;
    /** Exact weapon-pack requirements of a map plugin. Empty preserves existing independent map exports. */
    UPROPERTY() TArray<FRetakeWeaponPackRequirement> RequiredWeaponPacks;
    UPROPERTY() TArray<FRetakeModPlatformCompatibility> Compatibility;
    UPROPERTY() TArray<FRetakeModMap> Maps;
    UPROPERTY() TArray<FRetakeModArtifact> Artifacts;

    bool SaveToFile(const FString& Path, FString& OutError) const;
    static bool LoadFromFile(const FString& Path, FRetakeModManifest& Out, FString& OutError);
    /** Structural checks only; compatibility and integrity are checked by the reader. */
    bool Validate(FString& OutError) const;
    /** First 12 hex of BLAKE3 over the sorted artifact list: two exports that reuse a revision name still differ. */
    FString GetContentDigest() const;
    /** Login token entry "<ModId>.<Revision>.<ContentDigest>". */
    FString GetRevisionToken() const { return ModId + TEXT(".") + Revision + TEXT(".") + GetContentDigest(); }
    bool IsWeaponPack() const { return Format == TEXT("RetakeWeapons"); }

    static bool IsValidModId(const FString& Value);
    static bool IsValidRevision(const FString& Value);
    /** Display text reaches logs, refusal messages and the replicated catalogue: 1-64 printable characters without quotes, backslashes, '?' or '='. */
    static bool IsValidDisplayName(const FString& Value);
    /** Map description: at most 280 characters, no control characters, quotes or backslashes (it replicates as FText). */
    static bool IsValidDescription(const FString& Value);
    /** BLAKE3 of a file streamed from disk; empty on read failure. */
    static FString HashFile(const FString& Path, int64& OutSize);
};
