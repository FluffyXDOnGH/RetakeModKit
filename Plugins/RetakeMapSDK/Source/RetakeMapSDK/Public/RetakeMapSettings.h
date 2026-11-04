// Copyright 2026 Codnyx. All Rights Reserved.

#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "RetakeMapSettings.generated.h"

class UTexture2D;
class URetakeAmbienceProfile;
class URetakeVisibilityData;
class URetakeLoadoutPolicy;

/** Stable faction IDs from the Retake outfit catalogue, independent of match-team score IDs. */
UENUM(BlueprintType)
enum class ERetakeMapDefendingFaction : uint8
{
    Security = 0,
    Contractors = 1
};

/** Map-wide rules and, in mods, presentation. Place one in the persistent level; absent settings default to Security defending.
 *  Mod maps must place one with a Display Name. */
UCLASS(NotBlueprintable, meta=(DisplayName="Retake Map Settings"))
class RETAKEMAPSDK_API ARetakeMapSettings : public AInfo
{
    GENERATED_BODY()
public:
    ARetakeMapSettings();

    /** OBJ defenders for the entire match. Players choose their starting faction and switch factions at halftime.
     *  A MeleeDecider chooses each player group's starting faction, never changes this map role. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Objectives", meta=(DisplayName="Defending Faction"))
    ERetakeMapDefendingFaction DefendingFaction = ERetakeMapDefendingFaction::Security;

    /** Mod-owned visibility proof. Cooked with the map; export refuses stale or foreign preparation. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Network")
    TObjectPtr<URetakeVisibilityData> VisibilityBake;

    /** Recommended equipment policy; an operator's map/mode assignment takes precedence. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts")
    TObjectPtr<URetakeLoadoutPolicy> DefaultLoadoutPolicy;

    /** Stable mode catalogue IDs, not private GameMode classes. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Loadouts")
    TMap<FName, TObjectPtr<URetakeLoadoutPolicy>> ModeLoadoutPolicies;

#if WITH_EDITORONLY_DATA
    // How a mod map presents itself. Export copies this block into the manifest (schema 2), so the catalogue, vote and loading
    // screens can show it before the map loads. Stripped at cook. Required in mod maps (validator); built-in maps leave it empty.
    /** Map name in the catalogue, vote screens and the minimap caption. At most 64 characters, without quotes, backslashes, '?' or '='. */
    UPROPERTY(EditAnywhere, Category="Retake|Presentation")
    FString DisplayName;
    /** Optional, one line of at most 280 characters, without control characters, quotes or backslashes. */
    UPROPERTY(EditAnywhere, Category="Retake|Presentation")
    FString Description;
    /** Full-screen poster while the map loads: a texture in this mod, at most 2048 px, 16:9, UI texture settings. */
    UPROPERTY(EditAnywhere, Category="Retake|Presentation")
    TSoftObjectPtr<UTexture2D> LoadingArt;
    /** Thumbnail for map lists and votes: a texture in this mod, at most 1024 px, 16:9, UI texture settings. */
    UPROPERTY(EditAnywhere, Category="Retake|Presentation")
    TSoftObjectPtr<UTexture2D> Preview;
    /** Default (exterior) ambience; ambience zones override it while the listener is inside. */
    UPROPERTY(EditAnywhere, Category="Retake|Presentation")
    TSoftObjectPtr<URetakeAmbienceProfile> Ambience;
#endif

    /** Resolves the fixed attacking faction; no actor defaults to Security. Rejects ambiguous/invalid settings. */
    static bool ResolveAttackingFaction(UWorld* World, int32& OutAttackingFaction, FString& Error);

#if WITH_EDITOR
    virtual bool CanChangeIsSpatiallyLoadedFlag() const override { return false; }
    virtual bool ActorTypeSupportsDataLayer() const override { return false; }
    virtual bool ActorTypeSupportsExternalDataLayer() const override { return false; }
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};
