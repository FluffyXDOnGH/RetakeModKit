// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "RetakeWeaponResources.h"

class UNiagaraSystem;
class FDataValidationContext;

/** User parameter names the tracer system must expose (the user redirection store accepts them without the User. prefix). */
namespace RetakeTracerParameters
{
    inline const FName Start(TEXT("Start"));
    inline const FName End(TEXT("End"));
    inline const FName TravelSeconds(TEXT("TravelSeconds"));
    inline const FName SpawnRate(TEXT("SpawnRate"));
#if WITH_EDITOR
    /** Adds an error to Context for each missing part of the path contract (the four user parameters, no looping); true when System satisfies it. */
    RETAKEMAPSDK_API bool ValidateSystem(const UNiagaraSystem& System, FDataValidationContext& Context);
#endif
}


