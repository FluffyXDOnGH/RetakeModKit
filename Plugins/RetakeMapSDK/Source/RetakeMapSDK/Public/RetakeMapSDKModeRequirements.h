// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * What a map must contain to offer each built-in mode (SDK contract, MKD-036). The Map Kit refuses to export a map for a mode
 * whose minimums it misses; the advised counts only warn. The game's own map validation stays authoritative at runtime and may
 * be more permissive within a contract, never stricter: tightening a minimum is a ContractVersion bump. Minimums follow the
 * shipped profiles, checked by Retake.MapSDK.ModeRequirementParity. The game spawns on free starts first and reuses a taken one
 * only when none is free, so any team size runs; OBJ and FT still require five a side so a 5v5 round deploys onto free starts.
 */
namespace RetakeMapSDK
{
    /** Which player starts serve a mode, mirroring the game's start rules. */
    enum class EModeStartRule : uint8
    {
        AnyRole,           // FFA: every start
        SharedOrTeam,      // TDM, CTF, DOM: per team 0 and 1, Shared starts or Team starts with that TeamId
        TeamOnly,          // FT: per spawn end 0 and 1, Team starts with that TeamId
        AttackerDefender,  // OBJ: Attacker starts and Defender starts
    };

    struct FModeRequirement
    {
        const TCHAR* ModeId;
        EModeStartRule Starts;
        /** Per side; for AnyRole, in total. */
        int32 MinStarts;
        int32 AdvisedStarts;
        int32 MinBombSites;
        int32 MinControlPoints;
        bool bCaptureTheFlag;
        int32 MinRelays;
        /** FT presets that thaw players at shared spawns need at least one Shared start. */
        bool bRequireSharedStart;
    };

    inline constexpr FModeRequirement ModeRequirements[] =
    {
        { TEXT("FFA"), EModeStartRule::AnyRole,          2, 10, 0, 0, false, 0, false },
        { TEXT("TDM"), EModeStartRule::SharedOrTeam,     1,  5, 0, 0, false, 0, false },
        { TEXT("OBJ"), EModeStartRule::AttackerDefender, 5,  5, 1, 0, false, 0, false },
        { TEXT("CTF"), EModeStartRule::SharedOrTeam,     1,  5, 0, 0, true,  0, false },
        { TEXT("DOM"), EModeStartRule::SharedOrTeam,     1,  5, 0, 1, false, 0, false },
        { TEXT("FT"),  EModeStartRule::TeamOnly,         5,  5, 0, 0, false, 1, true  },
    };

    /** The requirement for a manifest mode token (case-insensitive), or null for a mode this contract does not know. */
    RETAKEMAPSDK_API const FModeRequirement* FindModeRequirement(FStringView ModeId);
}
