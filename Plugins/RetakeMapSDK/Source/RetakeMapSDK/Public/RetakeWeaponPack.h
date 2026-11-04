// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "RetakeWeaponPack.generated.h"

/** One stable weapon identity. Profiles select its definition; presentation names never identify a weapon. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponPackEntry
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Weapons|Identity") FString LocalId;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Weapons|Identity") FString DisplayName;
};

/** Complete definition references, not property patches. Several profiles can select the same definition asset. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponPackProfile
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Weapons|Profiles") FString ProfileId;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Weapons|Profiles") FString DisplayName;
    /** Owned native pose contribution prepared for this profile's specifications. May be shared by compatible profiles. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Weapons|Preparation", meta=(AllowedClasses="/Script/RetakeMapSDK.RetakeWeaponPoseContribution")) FSoftObjectPath PoseContribution;
    /** Local weapon ID -> owned definition object path. Omitted weapons are unavailable in this profile. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Weapons|Profiles") TMap<FString, FSoftObjectPath> Weapons;
};

/** Content-only authoring catalogue. Validation is structural; the exporter must also validate resource types and closure. */
UCLASS(BlueprintType, NotBlueprintable)
class RETAKEMAPSDK_API URetakeWeaponPack : public UDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Weapons|Identity") FString PackId;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Weapons|Identity") FString DisplayName;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Weapons|Definitions") TArray<FRetakeWeaponPackEntry> Weapons;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Weapons|Profiles") TArray<FRetakeWeaponPackProfile> Profiles;

    bool Validate(FString& Error) const;
    const FRetakeWeaponPackProfile* FindProfile(const FString& Id) const;
    /** Full BLAKE3 over all catalogue metadata and profile bindings, independent of container iteration order. */
    FString GetCatalogueIdentity() const;
    static bool IsValidLocalId(const FString& Id);
    static bool IsOwnedObjectPath(const FString& Owner, const FSoftObjectPath& Path);
#if WITH_EDITOR
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};

/** An exact dependency, resolved before weapon definitions or loadout policies may be activated. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeWeaponPackRequirement
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Weapons|Dependencies") FString PackId;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Weapons|Dependencies") FString Revision;
    /** Existing mod revision content digest (12 lower-case hex); container files still require their full verified hashes. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Weapons|Dependencies") FString ContentDigest;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Weapons|Dependencies") FString ProfileId;
    bool Validate(FString& Error) const;
};

/** A caller-owned, already integrity-checked installed pack. This helper does not mount or trust files. */
struct RETAKEMAPSDK_API FRetakeSelectedWeaponPack
{
    const URetakeWeaponPack* Catalogue = nullptr;
    FRetakeWeaponPackRequirement Selection;
};

struct RETAKEMAPSDK_API FRetakeResolvedPackWeapon
{
    FString WeaponId;
    FString DisplayName;
    FSoftObjectPath Definition;
};

/** Immutable plan for a session. It is not a registered weapon catalogue until native resource/preparation checks pass. */
struct RETAKEMAPSDK_API FRetakeWeaponPackSession
{
    TArray<FRetakeResolvedPackWeapon> Weapons;
    FString Identity;
};

namespace RetakeWeaponPacks
{
    /** Atomic, order-independent resolution. Errors clear Out; no partial registrations or load-order overrides. */
    RETAKEMAPSDK_API bool Resolve(TConstArrayView<FRetakeSelectedWeaponPack> Selected,
        TConstArrayView<FRetakeWeaponPackRequirement> Required, const TSet<FName>& BuiltinWeaponIds,
        FRetakeWeaponPackSession& Out, FString& Error);
}
