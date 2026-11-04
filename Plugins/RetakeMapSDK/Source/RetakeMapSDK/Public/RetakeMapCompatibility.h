// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "RetakeMapCompatibility.generated.h"

class FJsonObject;
struct FRetakeModManifest;

/** Cooked dependencies for one platform. Engine compatibility is independent of the SDK readable range. */
USTRUCT()
struct RETAKEMAPSDK_API FRetakeModPlatformCompatibility
{
    GENERATED_BODY()
    UPROPERTY() FString Platform;
    /** BLAKE3 of the immutable engine cook record, not an editor BuildId or a game release number. */
    UPROPERTY() FString EngineRecordId;
    /** SdkMaterials or CustomShaders; determined from authored dependencies AND cooked shader chunks. */
    UPROPERTY() FString MaterialPath;
    UPROPERTY() TArray<FString> ForeignPackages;
    UPROPERTY() TArray<FString> NativeClasses;
    /** Canonical collision/surface definitions. Existing definitions must remain byte-for-byte equivalent. */
    UPROPERTY() TArray<FString> StructuralProfile;
    /** Only versions actually present in this platform's cooked package headers. */
    UPROPERTY() TMap<FString, int32> CustomVersions;
};

struct RETAKEMAPSDK_API FRetakeModPackageVersions
{
    int32 UE4 = 0, UE5 = 0, Licensee = 0;
    TMap<FString, int32> CustomVersions;
};

/** Trusted studio qualification, loaded before opening any mod shader library or deserializing an object. */
struct RETAKEMAPSDK_API FRetakeMapCompatibility
{
    /** Validate a reviewed producer/consumer pair and the actual runtime structural profile. */
    static bool Check(const FRetakeModManifest& Manifest, const FString& Platform, FString& Error);
    /** Inspect an individual Zen package header without loading its objects. */
    static bool CheckPackage(TConstArrayView<uint8> Header, const FRetakeModPlatformCompatibility& Compatibility, FString& Error);
    /** Bounded, object-free inspection shared by export and runtime; unversioned packages are always refused. */
    static bool InspectPackage(TConstArrayView<uint8> Header, FRetakeModPackageVersions& Out, FString& Error);
    /** The qualified consumer record and executable binding, staged by game packaging. */
    static bool LoadPolicy(TSharedPtr<FJsonObject>& Out, FString& Error);
    /** Canonical BLAKE3 file hash, also used by creator recovery/install tooling. */
    static bool IsHash(const FString& Value);
};
