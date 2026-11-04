// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeMinimapDefinition.h"
#include "Engine/Texture2D.h"
#include "Misc/DataValidation.h"

bool URetakeMinimapDefinition::ValidateDefinition(FString& Error) const
{
    int32 Width=Floorplan ? Floorplan->GetSizeX():0;
    int32 Height=Floorplan ? Floorplan->GetSizeY():0;
#if WITH_EDITORONLY_DATA
    // NullRHI authoring has no texture platform data, even after texture compilation is drained.
    // Validate its saved source image; cooked/runtime assets still require their real platform dimensions.
    if (Floorplan && Width==0 && Height==0 && Floorplan->Source.IsValid())
    {
        Width=Floorplan->Source.GetSizeX();
        Height=Floorplan->Source.GetSizeY();
    }
#endif
    if (World.IsNull()) Error=TEXT("No world is assigned.");
    else if (!Floorplan) Error=TEXT("No floorplan texture is loaded.");
    else if (Width!=1024 || Height!=1024)
        Error=FString::Printf(TEXT("Floorplan %s is %d x %d; expected 1024 x 1024."),
            *Floorplan->GetPathName(),Width,Height);
    else if (!FMath::IsFinite(WorldSpan) || WorldSpan<=0 || WorldCenter.ContainsNaN()
        || !FMath::IsFinite(NorthYaw) || !FMath::IsFinite(ElevationDatum)) Error=TEXT("Invalid world-to-map calibration.");
    else if (DisplayName.IsEmpty()) Error=TEXT("No display name is assigned.");
    else { Error.Reset(); return true; }
    return false;
}

FVector2D URetakeMinimapDefinition::WorldToUV(const FVector& Location) const
{
    const double Radians=FMath::DegreesToRadians(NorthYaw);
    const FVector2D North(FMath::Cos(Radians),FMath::Sin(Radians)), East(-North.Y,North.X);
    const FVector2D Delta(Location.X-WorldCenter.X,Location.Y-WorldCenter.Y);
    return FVector2D(.5+FVector2D::DotProduct(Delta,East)/WorldSpan,.5-FVector2D::DotProduct(Delta,North)/WorldSpan);
}

#if WITH_EDITOR
EDataValidationResult URetakeMinimapDefinition::IsDataValid(FDataValidationContext& Context) const
{
    const EDataValidationResult Parent=Super::IsDataValid(Context);
    FString Error;
    if (!ValidateDefinition(Error)) { Context.AddError(FText::FromString(Error)); return EDataValidationResult::Invalid; }
    return Parent==EDataValidationResult::Invalid ? Parent : EDataValidationResult::Valid;
}
#endif
