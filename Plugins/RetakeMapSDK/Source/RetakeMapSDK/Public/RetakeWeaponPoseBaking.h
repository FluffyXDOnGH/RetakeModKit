// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "RetakeWeaponPose.h"

#if WITH_EDITOR
#include "BoneContainer.h"
#include "BonePose.h"

class IConsoleVariable;
class USkeletalMesh;
class UAnimMontage;
class UBlendSpace;
class URetakeWeaponSpecification;
struct FAnimTrack;

/** Shared editor preparation. No game modules, hit shapes, movement simulation or world are required. */
namespace RetakeWeaponPoseBaking
{
    /** Explicit shared authoring inputs; callers supply the delivered character basis, never a private game rig. */
    struct FInputs
    {
        const USkeletalMesh* ReferenceMesh=nullptr;
        FRetakeWeaponPoseContract Body;
        const UAnimSequence* StandLower=nullptr;
        const UAnimSequence* CrouchLower=nullptr;
        const UBlendSpace* StandLean=nullptr;
        const UBlendSpace* CrouchLean=nullptr;
        const UBlendSpace* StandMovingLean=nullptr;
        const UBlendSpace* CrouchMovingLean=nullptr;
        const UBlendSpace* ClearanceStand=nullptr;
        const UBlendSpace* ClearanceCrouch=nullptr;
        float LeanStrength=1.f;
        FName SupportSocket=TEXT("FABRIK_Socket");
    };
    struct FClearanceSource
    {
        TArray<float> Keys;
        TArray<FTransform3f> Poses;
    };
    /** Caller retains its assets and an FMemMark for the lifetime of compact poses sampled from this context. */
    struct RETAKEMAPSDK_API FContext
    {
        const USkeletalMesh* Mesh=nullptr;
        TArray<int32> MeshIndices;
        TArray<FBoneIndexType> Required;
        FBoneContainer Bones;
        TSet<FName> Sources;
        bool Initialize(const USkeletalMesh& Reference,TConstArrayView<FRetakePoseBone> Catalogue,FString& Error);
        void Read(const UObject* Source);
    };

    /** Preserves the established raw/runtime sampling policy; only used during game-thread authoring. */
    struct RETAKEMAPSDK_API FSamplingScope
    {
        explicit FSamplingScope(int32 ForceRaw=1);
        ~FSamplingScope();
        FSamplingScope(const FSamplingScope&)=delete;
        FSamplingScope& operator=(const FSamplingScope&)=delete;
    private:
        IConsoleVariable* Variable=nullptr;
        int32 Previous=0;
    };

    RETAKEMAPSDK_API bool SamplePose(FContext& Context,const UAnimSequence* Sequence,FCompactPose& Out,bool bRuntimeData=false);
    RETAKEMAPSDK_API void ForEachRuntimeActionPose(FContext& Context,const FAnimTrack* Slot,const UAnimSequence* Sequence,
        ERetakeHitRigLayer Kind,float Length,float SampleRate,TFunctionRef<void(const FCompactPose&)> Read);
    RETAKEMAPSDK_API void SampleActionLayer(FContext& Context,const FAnimTrack* Slot,const UAnimSequence* Sequence,
        ERetakeHitRigLayer Kind,float Length,float SampleRate,TArray<FTransform3f>& Out);
    RETAKEMAPSDK_API bool BakeTrack(FContext& Context,const UAnimMontage* Montage,const UAnimSequence* Sequence,
        FRetakeHitRigTrack& Track,float MinimumSampleRate=120.f);
    RETAKEMAPSDK_API bool BakeHold(FContext& Context,const FPlayerAnims& Anims,const FRetakeThirdPersonAimPose& Aim,
        const UBlendSpace* StandLean,const UBlendSpace* CrouchLean,const UBlendSpace* StandMovingLean,const UBlendSpace* CrouchMovingLean,
        float LeanStrength,FName ProfileId,const FCompactPose* StandLower,const FCompactPose* CrouchLower,
        FRetakeHitRigHold& Hold,FString& Error);
    /** Prepares support-hand and barrel geometry from the reference body and the public weapon specification. */
    RETAKEMAPSDK_API void BakeHands(const USkeletalMesh& ReferenceMesh,const URetakeWeaponSpecification& Definition,
        FName SupportSocket,FRetakeHitRigHold& Hold);
    RETAKEMAPSDK_API bool CaptureClearance(FContext& Context,const UBlendSpace* Space,FClearanceSource& Out,FString& Error);
    RETAKEMAPSDK_API bool BakeClearance(TConstArrayView<FRetakePoseBone> Bones,const FClearanceSource& Source,
        const FRetakeHitRigHold& Hold,ERetakeHitRigPose Pose,FRetakeHitRigArmClearance& Out,FString& Error);
    /** Complete selected weapon hold/actions/clearance. Failure leaves both outputs unchanged; no save or gameplay execution. */
    RETAKEMAPSDK_API bool BakeWeapon(const URetakeWeaponSpecification& Definition,const FInputs& Inputs,
        FRetakeHitRigHold& Out,TSet<FName>& OutSources,FString& Error);
    /** Reproducible basis/specification/dependency signature. Missing or dirty source packages are refused. */
    RETAKEMAPSDK_API bool SourceSignature(const FString& Owner,const FInputs& Inputs,TConstArrayView<const URetakeWeaponSpecification*> Definitions,
        FString& Out,FString& Error);
    /** Atomically replaces an owned contribution after all selected weapons and source stability checks succeed; caller saves. */
    RETAKEMAPSDK_API bool BakeContribution(const FString& Owner,const FInputs& Inputs,
        TConstArrayView<const URetakeWeaponSpecification*> Definitions,URetakeWeaponPoseContribution& Out,FString& Error);
}
#endif
