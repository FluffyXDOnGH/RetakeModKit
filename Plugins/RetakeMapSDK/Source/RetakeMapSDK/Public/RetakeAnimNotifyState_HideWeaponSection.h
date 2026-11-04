// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once

#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "RetakeAnimNotifyState_HideWeaponSection.generated.h"

/**
 * Hides one material section of the weapon held by the animated mesh for the notify's duration (the weapon's own
 * magazine while a Retake Hand Prop shows the spare in the hand), matching Mohaa's "surface Clip +nodraw/-nodraw".
 * The weapon is found among the animated mesh's attached skeletal meshes that belong to another actor.
 */
UCLASS(NotBlueprintable, meta=(DisplayName="Retake Hide Weapon Section"))
class RETAKEMAPSDK_API URetakeAnimNotifyState_HideWeaponSection : public UAnimNotifyState
{
    GENERATED_BODY()
public:
    /** Material slot name on the weapon mesh (for example "clip"); matched case-insensitively. */
    UPROPERTY(EditAnywhere, Category="Retake|Animation") FName MaterialSlot = TEXT("clip");

    virtual void NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration,
        const FAnimNotifyEventReference& EventReference) override;
    virtual void NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
        const FAnimNotifyEventReference& EventReference) override;
    virtual FString GetNotifyName_Implementation() const override;

};
