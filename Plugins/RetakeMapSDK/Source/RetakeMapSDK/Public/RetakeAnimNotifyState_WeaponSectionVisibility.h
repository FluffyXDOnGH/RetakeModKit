// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "RetakeAnimNotifyState_WeaponSectionVisibility.generated.h"

/** Cosmetic material-slot override with automatic restoration on completion or interruption.
 * Use the animated mesh for weapon clips; attached weapons for character hand/reload clips. */
UCLASS(NotBlueprintable, meta=(DisplayName="Retake Weapon Section Visibility"))
class RETAKEMAPSDK_API URetakeAnimNotifyState_WeaponSectionVisibility : public UAnimNotifyState
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, Category="Retake|Animation") FName MaterialSlot;
    UPROPERTY(EditAnywhere, Category="Retake|Animation") bool bVisible=true;
    UPROPERTY(EditAnywhere, Category="Retake|Animation") bool bAnimatedMesh=true;
    virtual void NotifyBegin(USkeletalMeshComponent* MeshComp,UAnimSequenceBase* Animation,float TotalDuration,
        const FAnimNotifyEventReference& EventReference) override;
    virtual void NotifyEnd(USkeletalMeshComponent* MeshComp,UAnimSequenceBase* Animation,
        const FAnimNotifyEventReference& EventReference) override;
    virtual FString GetNotifyName_Implementation() const override;
};
