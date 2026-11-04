// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once

#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "RetakeAnimNotifyState_HandProp.generated.h"

class UStreamableRenderAsset;

/**
 * Cosmetic prop held during part of an animation (a stripper clip or a round during a reload). A mesh component is
 * attached to Bone on the animated mesh for the notify's duration and removed at its end, matching Mohaa's
 * attachmodel/removeattachedmodel. Mesh may be a skeletal or static mesh. It inherits the owner's first-person primitive type, so it draws with the arms.
 */
UCLASS(NotBlueprintable, meta=(DisplayName="Retake Hand Prop"))
class RETAKEMAPSDK_API URetakeAnimNotifyState_HandProp : public UAnimNotifyState
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, Category="Retake|Animation", meta=(AllowedClasses="/Script/Engine.SkeletalMesh,/Script/Engine.StaticMesh"))
    TObjectPtr<UStreamableRenderAsset> Mesh;
    UPROPERTY(EditAnywhere, Category="Retake|Animation") FName Bone = TEXT("ik_hand_r");
    /** Prop placement relative to Bone. */
    UPROPERTY(EditAnywhere, Category="Retake|Animation") FTransform Offset;

    virtual void NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration,
        const FAnimNotifyEventReference& EventReference) override;
    virtual void NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
        const FAnimNotifyEventReference& EventReference) override;
    virtual FString GetNotifyName_Implementation() const override;
};
