// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RetakeMinimapCapture.generated.h"

class UBoxComponent;

/**
 * The square a mod map's minimap is baked from (-run=RetakeMinimapBake, Scripts/ModKit/modkit.py minimap). Place one per map.
 * Location XY is the image centre and Z the elevation datum; yaw is the direction shown at the top of the image. The bake draws
 * the upward faces of static meshes (LOD0) inside the square and below CaptureHeight, shaded by floor band; tag an actor or a
 * component ExcludeTag to leave it out. Editor only: never cooked, so its layout is not part of the SDK contract.
 */
UCLASS(NotBlueprintable, HideCategories=(Rendering, Replication, Collision, HLOD, Input, Actor, LOD, Cooking, Physics, Networking),
    meta=(DisplayName="Retake Minimap Capture"))
class RETAKEMAPSDK_API ARetakeMinimapCapture : public AActor
{
    GENERATED_BODY()
public:
    ARetakeMinimapCapture();

    /** Side of the captured square: the minimap's world span. */
    UPROPERTY(EditAnywhere, Category="Retake|UI|Minimap", meta=(ClampMin="1000", Units="cm"))
    double CaptureSize = 10000;
    /** Geometry higher than this above the datum (roofs, canopies, sky meshes) is left out. */
    UPROPERTY(EditAnywhere, Category="Retake|UI|Minimap", meta=(ClampMin="100", Units="cm"))
    double CaptureHeight = 2000;
    /** Floor band boundaries above the datum in cm, ascending: up to three, so up to four shades. Geometry below the first is the ground floor. */
    UPROPERTY(EditAnywhere, Category="Retake|UI|Minimap")
    TArray<double> FloorBandHeights = { 300, 600, 900 };
    /** Actors or components with this tag are left out of the bake. */
    UPROPERTY(EditAnywhere, Category="Retake|UI|Minimap")
    FName ExcludeTag = TEXT("MinimapExclude");

    /** Level placement and band layout the bake needs; Error says what to fix. */
    bool ValidateCapture(FString& Error) const;

#if WITH_EDITOR
    virtual void OnConstruction(const FTransform& Transform) override;
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif

private:
    UPROPERTY(VisibleAnywhere, Category="Retake|UI|Minimap")
    TObjectPtr<UBoxComponent> Bounds;
};
