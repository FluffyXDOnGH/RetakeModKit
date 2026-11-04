// Copyright 2026 Codnyx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "RetakeMapSDKCommandlets.generated.h"

/**
 * -run=RetakeMapSDKProfile [-Out=<dir>]
 * Logs the SDK contract, content generation and authoring-profile hash per ini platform. Run it in the Retake
 * project and in the Map Kit: equal hashes prove the kit's generated configuration matches the game (MK-042).
 * -Out also writes the canonical text per platform for diffing.
 */
UCLASS()
class URetakeMapSDKProfileCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    URetakeMapSDKProfileCommandlet();
    virtual int32 Main(const FString& Params) override;
};

/**
 * -run=RetakeModManifest -Mod=<RetakeMod_X> -Revision=<rev> -Dir=<export dir> [-Modes=<spec>] [-DisplayName=..] [-Author=..] [-SourceAvailable]
 * Validates every map (RetakeMapValidator) and records the modes and presentation (map settings, minimap, ambience) each
 * offers: -Modes=auto (default) lists every mode a map
 * passes, "FFA,TDM" requires those for every map, "Map=FFA,TDM;Other=auto" sets them per map. Checks that every package of
 * the mod plugin references only its own root, /Script and RetakeMapSDK::AllowedForeignRoots, and that every cooked reference
 * outside the mod is shipped by the base game release in SDK.lock.json (MKI-030), then writes <Dir>/manifest.json with the
 * per-platform profile hashes and the BLAKE3 of every container under <Dir>/<CookPlatform>/. Scripts/ModKit/modkit.py cooks.
 */
UCLASS()
class URetakeModManifestCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    URetakeModManifestCommandlet();
    virtual int32 Main(const FString& Params) override;
};

/**
 * -run=RetakeMapValidate -Mod=<RetakeMod_X> [-Modes=<spec>]
 * Validates the mod's maps for the requested modes (same spec as RetakeModManifest) and logs every issue as
 * event=MapValidation; exits non-zero when a map fails. Nothing is cooked or written.
 */
UCLASS()
class URetakeMapValidateCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    URetakeMapValidateCommandlet();
    virtual int32 Main(const FString& Params) override;
};

/**
 * -run=RetakeMinimapBake -Mod=<RetakeMod_X> [-Map=<Name>]
 * Bakes the minimap of every mod map with a Retake Minimap Capture (or only -Map, which then must have one) into
 * /<Mod>/Minimap/T_Minimap_<Map> and DA_Minimap_<Map> (RetakeMinimapBaker), captioned with the map settings' Display Name.
 * Export never bakes; validation reports a stale bake. Scripts/ModKit/modkit.py minimap runs it.
 */
UCLASS()
class URetakeMinimapBakeCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    URetakeMinimapBakeCommandlet();
    virtual int32 Main(const FString& Params) override;
};

/** Explicit acoustic preparation, independent of visibility: -run=RetakeAcousticsBake -Mod=RetakeMod_X [-Map=Name]. */
UCLASS()
class URetakeAcousticsBakeCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    URetakeAcousticsBakeCommandlet();
    virtual int32 Main(const FString& Params) override;
};

/** Creator visibility preparation: -Mod=RetakeMod_X -Map=Name -Phase=Prepare|Export|Import.
 * Prepare retains CPU geometry on eligible mod-owned meshes; Export/Import also require -File=<scene or bake JSON>. */
UCLASS()
class URetakeVisibilityBakeCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    URetakeVisibilityBakeCommandlet();
    virtual int32 Main(const FString& Params) override;
};

/** Same creator service as Retake > Create New Mod: -run=RetakeModCreate -Name=MyMap [-Basic]. */
UCLASS()
class URetakeModCreateCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    URetakeModCreateCommandlet();
    virtual int32 Main(const FString& Params) override;
};
