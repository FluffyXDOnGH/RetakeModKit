// Copyright 2026 Codnyx. All Rights Reserved.

#include "RetakeMapSettings.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

ARetakeMapSettings::ARetakeMapSettings()
{
    // AInfo is non-spatial and does not tick. Only the server reads map rules;
    // clients receive the resolved attack/defend roles through the match state.
    bNetLoadOnClient = false;
}

bool ARetakeMapSettings::ResolveAttackingFaction(UWorld* World, int32& OutAttackingFaction, FString& Error)
{
    // Security defends unless the map's settings say otherwise, so Contractors (faction 1) attack.
    OutAttackingFaction = 1;
    Error.Reset();
    if (!World) { Error = TEXT("The world is unavailable for map faction roles."); return false; }
    const ARetakeMapSettings* MapSettings = nullptr;
    for (TActorIterator<ARetakeMapSettings> It(World); It; ++It)
    {
        if (MapSettings)
        { Error = TEXT("Place only one Retake Map Settings actor in the map."); return false; }
        if (It->GetLevel() != World->PersistentLevel)
        { Error = TEXT("Retake Map Settings must be placed in the persistent level, outside streamed sublevels and level instances."); return false; }
        MapSettings = *It;
    }
    if (!MapSettings) return true;
    switch (MapSettings->DefendingFaction)
    {
    case ERetakeMapDefendingFaction::Security: OutAttackingFaction = 1; return true;
    case ERetakeMapDefendingFaction::Contractors: OutAttackingFaction = 0; return true;
    default: Error = TEXT("Retake Map Settings has an invalid Defending Faction."); return false;
    }
}

#if WITH_EDITOR
EDataValidationResult ARetakeMapSettings::IsDataValid(FDataValidationContext& Context) const
{
    const EDataValidationResult SuperResult = Super::IsDataValid(Context);
    int32 AttackingFaction = 0;
    FString Error;
    if (!ResolveAttackingFaction(GetWorld(), AttackingFaction, Error))
    {
        Context.AddError(FText::FromString(Error));
        return EDataValidationResult::Invalid;
    }
    return SuperResult == EDataValidationResult::Invalid ? SuperResult : EDataValidationResult::Valid;
}
#endif
