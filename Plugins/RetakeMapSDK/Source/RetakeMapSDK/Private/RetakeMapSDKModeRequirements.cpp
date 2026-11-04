// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeMapSDKModeRequirements.h"

const RetakeMapSDK::FModeRequirement* RetakeMapSDK::FindModeRequirement(FStringView ModeId)
{
    for (const FModeRequirement& Requirement : ModeRequirements)
        if (ModeId.Equals(Requirement.ModeId, ESearchCase::IgnoreCase)) return &Requirement;
    return nullptr;
}
