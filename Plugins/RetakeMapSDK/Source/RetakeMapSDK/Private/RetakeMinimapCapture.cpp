// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeMinimapCapture.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

ARetakeMinimapCapture::ARetakeMinimapCapture()
{
    PrimaryActorTick.bCanEverTick = false;
    bIsEditorOnlyActor = true;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Datum"));
    Bounds = CreateDefaultSubobject<UBoxComponent>(TEXT("Bounds"));
    Bounds->SetupAttachment(RootComponent);
    Bounds->SetCollisionProfileName(TEXT("NoCollision"));
    Bounds->SetGenerateOverlapEvents(false);
    Bounds->SetHiddenInGame(true);
    Bounds->ShapeColor = FColor(246, 193, 88);
    Bounds->SetLineThickness(8.f);
}

bool ARetakeMinimapCapture::ValidateCapture(FString& Error) const
{
    const FRotator Rotation = GetActorRotation();
    if (!FMath::IsNearlyZero(Rotation.Pitch, 0.01) || !FMath::IsNearlyZero(Rotation.Roll, 0.01))
        Error = TEXT("Keep the minimap capture level: only its yaw is used.");
    else if (!GetActorScale3D().Equals(FVector::OneVector, 1e-4))
        Error = TEXT("Keep the minimap capture at scale 1; set Capture Size and Capture Height instead.");
    else if (!FMath::IsFinite(CaptureSize) || CaptureSize < 1000 || !FMath::IsFinite(CaptureHeight) || CaptureHeight < 100)
        Error = TEXT("Capture Size must be at least 1000 cm and Capture Height at least 100 cm.");
    else if (FloorBandHeights.Num() > 3) Error = TEXT("Use at most three floor band heights (four shades).");
    else
    {
        double Previous = 0;
        for (const double Height : FloorBandHeights)
        {
            if (!FMath::IsFinite(Height) || Height <= Previous || Height >= CaptureHeight)
            { Error = TEXT("Floor band heights must ascend, be above the datum and stay below Capture Height."); return false; }
            Previous = Height;
        }
        Error.Reset();
        return true;
    }
    return false;
}

#if WITH_EDITOR
void ARetakeMinimapCapture::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    // The box shows what the bake reads: the square above the datum, up to the capture ceiling.
    const double Half = FMath::Max(CaptureSize, 1.0) * .5, HalfHeight = FMath::Max(CaptureHeight, 1.0) * .5;
    Bounds->SetBoxExtent(FVector(Half, Half, HalfHeight), false);
    Bounds->SetRelativeLocation(FVector(0, 0, HalfHeight));
}

EDataValidationResult ARetakeMinimapCapture::IsDataValid(FDataValidationContext& Context) const
{
    const EDataValidationResult Parent = Super::IsDataValid(Context);
    FString Error;
    if (!ValidateCapture(Error)) { Context.AddError(FText::FromString(Error)); return EDataValidationResult::Invalid; }
    return Parent == EDataValidationResult::Invalid ? Parent : EDataValidationResult::Valid;
}
#endif
