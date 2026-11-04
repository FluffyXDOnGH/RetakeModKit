// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "RetakeWeaponPoseBaking.h"
#include "RetakeWeaponPoseBasis.generated.h"

/** Versioned editor-only authoring inputs. Runtime contributions contain the checked contract, not these source references. */
UCLASS(NotBlueprintable)
class RETAKEMAPSDKEDITOR_API URetakeWeaponPoseBasis : public UDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, Category="Retake|Weapons|Preparation") FRetakeWeaponPoseContract Body;
    UPROPERTY(EditAnywhere, Category="Retake|Weapons|Preparation") TObjectPtr<USkeletalMesh> ReferenceMesh;
    UPROPERTY(EditAnywhere, Category="Retake|Weapons|Preparation") TObjectPtr<UAnimSequence> StandLower;
    UPROPERTY(EditAnywhere, Category="Retake|Weapons|Preparation") TObjectPtr<UAnimSequence> CrouchLower;
    UPROPERTY(EditAnywhere, Category="Retake|Weapons|Preparation") TObjectPtr<UBlendSpace> StandLean;
    UPROPERTY(EditAnywhere, Category="Retake|Weapons|Preparation") TObjectPtr<UBlendSpace> CrouchLean;
    UPROPERTY(EditAnywhere, Category="Retake|Weapons|Preparation") TObjectPtr<UBlendSpace> StandMovingLean;
    UPROPERTY(EditAnywhere, Category="Retake|Weapons|Preparation") TObjectPtr<UBlendSpace> CrouchMovingLean;
    UPROPERTY(EditAnywhere, Category="Retake|Weapons|Preparation") TObjectPtr<UBlendSpace> ClearanceStand;
    UPROPERTY(EditAnywhere, Category="Retake|Weapons|Preparation") TObjectPtr<UBlendSpace> ClearanceCrouch;
    UPROPERTY(EditAnywhere, Category="Retake|Weapons|Preparation", meta=(ClampMin="0",ClampMax="1")) float LeanStrength=1.f;
    UPROPERTY(EditAnywhere, Category="Retake|Weapons|Preparation") FName SupportSocket=TEXT("FABRIK_Socket");
    RetakeWeaponPoseBaking::FInputs GetInputs() const;
    virtual bool IsEditorOnly() const override { return true; }
};
