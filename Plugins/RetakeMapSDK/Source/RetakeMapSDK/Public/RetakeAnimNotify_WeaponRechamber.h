// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once

#include "Animation/AnimNotifies/AnimNotify.h"
#include "RetakeAnimNotify_WeaponRechamber.generated.h"

/** Stateless cosmetic event. The weapon definition owns the sound; the weapon instance owns playback. */
UCLASS(NotBlueprintable, meta=(DisplayName="Retake Weapon Rechamber"))
class RETAKEMAPSDK_API URetakeAnimNotify_WeaponRechamber : public UAnimNotify
{
    GENERATED_BODY()
public:
    URetakeAnimNotify_WeaponRechamber(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
    virtual void Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
        const FAnimNotifyEventReference& EventReference) override;
    virtual FString GetNotifyName_Implementation() const override;
#if WITH_EDITOR
    virtual void OnAnimNotifyCreatedInEditor(FAnimNotifyEvent& Event) override;
#endif
};
