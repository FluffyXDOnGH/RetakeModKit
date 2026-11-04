// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeWeaponTracerParameters.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#if WITH_EDITOR
bool RetakeTracerParameters::ValidateSystem(const UNiagaraSystem& System,FDataValidationContext& Context)
{
    bool Valid=true;
    auto Require=[&](bool Condition,const FString& Message) { if (!Condition) { Valid=false; Context.AddError(FText::FromString(Message)); } };
    const FString Name=System.GetName();
    // The subsystem writes these through the user redirection store, which accepts the bare names.
    const FNiagaraUserRedirectionParameterStore& Exposed=System.GetExposedParameters();
    const auto Has=[&](const FNiagaraTypeDefinition& Type,FName Parameter)
    {
        return Exposed.IndexOf(FNiagaraVariable(Type,Parameter))!=INDEX_NONE
            || Exposed.IndexOf(FNiagaraVariable(Type,*FString::Printf(TEXT("User.%s"),*Parameter.ToString())))!=INDEX_NONE;
    };
    Require(Has(FNiagaraTypeDefinition::GetPositionDef(),Start) && Has(FNiagaraTypeDefinition::GetPositionDef(),End),
        Name+TEXT(" must expose the position user parameters Start and End."));
    Require(Has(FNiagaraTypeDefinition::GetFloatDef(),TravelSeconds) && Has(FNiagaraTypeDefinition::GetFloatDef(),SpawnRate),
        Name+TEXT(" must expose the float user parameters TravelSeconds and SpawnRate."));
    Require(!System.IsLooping(),Name+TEXT(" must run once per activation, not loop."));
    return Valid;
}

EDataValidationResult URetakeTracerProfile::IsDataValid(FDataValidationContext& Context) const
{
    bool Valid=true;
    auto Require=[&](bool Condition,const TCHAR* Message) { if (!Condition) { Valid=false; Context.AddError(FText::FromString(Message)); } };
    Require(System!=nullptr,TEXT("Tracer profile requires a Niagara system."));
    Require(FMath::IsFinite(SpeedCmPerSecond) && SpeedCmPerSecond>=5000.f,TEXT("Tracer speed must be at least 5000 cm/s."));
    Require(FMath::IsFinite(LingerSeconds) && LingerSeconds>0.f,TEXT("Tracer linger must be positive."));
    Require(MinimumPathCm<MaximumPathCm,TEXT("Tracer minimum path must be shorter than its maximum path."));
    if (System && !RetakeTracerParameters::ValidateSystem(*System,Context)) Valid=false;
    const auto Parent=Super::IsDataValid(Context);
    return Valid && Parent!=EDataValidationResult::Invalid ? EDataValidationResult::Valid : EDataValidationResult::Invalid;
}
#endif
