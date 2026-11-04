// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "RetakeWeaponPoseRecipe.generated.h"

class URetakeWeaponPoseBasis;
class URetakeWeaponSpecification;
class URetakeWeaponPoseContribution;

/** Editable preparation inputs live with their output; the game never loads this editor implementation. */
UCLASS(NotBlueprintable, EditInlineNew)
class RETAKEMAPSDKEDITOR_API URetakeWeaponPoseRecipe : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY(VisibleAnywhere, Category="Retake|Weapons|Preparation") TSoftObjectPtr<URetakeWeaponPoseBasis> Basis;
    UPROPERTY(VisibleAnywhere, Category="Retake|Weapons|Preparation") TArray<TSoftObjectPtr<URetakeWeaponSpecification>> Specifications;
    virtual bool IsEditorOnly() const override { return true; }
};

namespace RetakeWeaponPosePreparation
{
    /** Completes registry discovery, checks the saved recipe/inputs and rehashes current authoring dependencies. */
    RETAKEMAPSDKEDITOR_API bool ValidateSources(const FString& Owner,const URetakeWeaponPoseContribution& Output,
        FString& Error,TConstArrayView<FSoftObjectPath> RequiredSpecifications={},bool bRequireSavedOutput=true);
}
