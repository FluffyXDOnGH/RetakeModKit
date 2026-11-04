// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "RetakeMinimapDefinition.generated.h"

class UTexture2D;

/**
 * A cooked, flat cartographic image (RetakeMapSDK asset). Baking and geometry discovery are editor/offline work: built-in maps use
 * Scripts/Minimap, mods bake with -run=RetakeMinimapBake from a Retake Minimap Capture.
 * Frozen per content generation: UPROPERTY names, order, types and defaults.
 */
UCLASS(BlueprintType)
class RETAKEMAPSDK_API URetakeMinimapDefinition : public UDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|UI|Minimap") TSoftObjectPtr<UWorld> World;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|UI|Minimap") TObjectPtr<UTexture2D> Floorplan;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|UI|Minimap") FText DisplayName;
    /** XY at the centre of the square texture. Unreal centimetres. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|UI|Minimap") FVector2D WorldCenter = FVector2D::ZeroVector;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|UI|Minimap", meta=(ClampMin="1", Units="cm")) double WorldSpan = 5000;
    /** World yaw pointing towards the top of the image; +X north by default. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|UI|Minimap", meta=(Units="deg")) double NorthYaw = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|UI|Minimap", meta=(Units="cm")) double ElevationDatum = 0;
    /** Provenance, not a runtime geometry hash or gameplay admission gate. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|UI|Minimap") FString GeometryFingerprint;
    bool ValidateDefinition(FString& Error) const;
    UFUNCTION(BlueprintPure, Category="Retake|UI|Minimap")
    FVector2D WorldToUV(const FVector& Location) const;
#if WITH_EDITOR
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};
