// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeMapSDKEditorLibrary.h"
#include "RetakeMapSDKManifest.h"
#include "RetakeWeaponPack.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Editor.h"
#include "ScopedTransaction.h"
#include "UObject/SoftObjectPath.h"
#include "MovieScene.h"
#include "MovieSceneSequence.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Sections/MovieSceneParameterSection.h"
#include "Math/QuatRotationTranslationMatrix.h"
#include "Rigs/FKControlRig.h"
#include "Rigs/RigHierarchy.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"

FString URetakeMapSDKEditorLibrary::SetAnimationBoneRotationKeys(UAnimSequence* Animation,const FString& OwnerModId,
    FName BoneName,const TArray<FQuat>& Rotations)
{
    if (!IsInGameThread() || !Animation || (GEditor && GEditor->PlayWorld)
        || !FRetakeModManifest::IsValidModId(OwnerModId)
        || !URetakeWeaponPack::IsOwnedObjectPath(OwnerModId,FSoftObjectPath(Animation)) || BoneName.IsNone())
        return TEXT("Select a mod-owned animation and bone on the editor game thread with PIE stopped.");
    const auto Model=Animation->GetDataModelInterface();
    auto* Sequence=Model.GetObject() ? Cast<UMovieSceneSequence>(Model.GetObject()):nullptr;
    UMovieScene* Scene=Sequence ? Sequence->GetMovieScene():nullptr;
    if (!Model || !Scene || !Model->IsValidBoneTrackName(BoneName) || Animation->IsValidAdditive()
        || Rotations.Num()!=Model->GetNumberOfKeys() || Rotations.Num()<2 || Rotations.Num()>100001
        || Scene->GetTickResolution()!=Model->GetFrameRate())
        return TEXT("This operation requires an existing nonadditive sequence-backed track, one rotation per authored key and matching tick/sample rates.");
    TArray<FQuat> Expected;TArray<FVector> EulerKeys;
    Expected.Reserve(Rotations.Num());EulerKeys.Reserve(Rotations.Num());
    for (const FQuat& Rotation:Rotations)
    {
        if (Rotation.ContainsNaN() || !Rotation.IsNormalized()) return TEXT("Every rotation must be finite and normalized.");
        const FQuat Q=Rotation.GetNormalized();
        // Quat::Rotator snaps near-pole values to +/-90. The matrix route retains the requested orientation.
        const FRotator R=FQuatRotationMatrix(Q).Rotator();
        FVector Euler=R.Euler();
        if (!EulerKeys.IsEmpty())
        {
            const FVector Previous=EulerKeys.Last();
            const auto Unwind=[&](FVector Value)
            {
                for (int32 Axis=0;Axis<3;++Axis) Value[Axis]=Previous[Axis]+FMath::FindDeltaAngleDegrees(Previous[Axis],Value[Axis]);
                return Value;
            };
            Euler=Unwind(Euler);
            const FVector Alternative=Unwind(R.GetEquivalentRotator().Euler());
            if ((Alternative-Previous).SizeSquared()<(Euler-Previous).SizeSquared()) Euler=Alternative;
        }
        // Verify the channels' actual float storage precision before touching the asset.
        Euler=FVector(FVector3f(Euler));
        const FQuat RoundTrip=FRotator(Euler.Y,Euler.Z,Euler.X).Quaternion().GetNormalized();
        if (RoundTrip.AngularDistance(Q)>1.e-5) return TEXT("The requested rotation cannot be represented accurately by this animation's channels.");
        Expected.Add(Q);EulerKeys.Add(Euler);
    }
    UMovieSceneControlRigParameterSection* Selected=nullptr;FTransformParameterNameAndCurves* Parameters=nullptr;
    for (UMovieSceneSection* Candidate:Scene->GetAllSections())
        if (auto* Section=Cast<UMovieSceneControlRigParameterSection>(Candidate))
            if (auto* Rig=Cast<UFKControlRig>(Section->GetControlRig());Rig && Rig->GetApplyMode()==EControlRigFKRigExecuteMode::Direct)
                if (URigHierarchy* Hierarchy=Rig->GetHierarchy())
                {
                    const FRigElementKey Bone(Hierarchy->GetSanitizedName(BoneName,true),ERigElementType::Bone);
                    const FName Control=UFKControlRig::GetControlName(Bone.Name,ERigElementType::Bone);
                    auto* Found=Section->GetTransformParameterNamesAndCurves().FindByPredicate(
                        [&](const FTransformParameterNameAndCurves& Value){return Value.ParameterName==Control;});
                    if (!Found) continue;
                    if (Selected) return TEXT("The bone has ambiguous FK animation sections.");
                    Selected=Section;Parameters=Found;
                }
    if (!Selected || !Parameters) return TEXT("The animation has no direct FK section for this bone.");
    IAnimationDataModel::FEvaluationAndModificationLock Lock(*Model);
    const FMovieSceneFloatChannel Previous[]={Parameters->Rotation[0],Parameters->Rotation[1],Parameters->Rotation[2]};
    const FScopedTransaction Transaction(NSLOCTEXT("RetakeModKit","SetBoneRotationKeys","Set mod animation bone rotation keys"));
    Animation->Modify();Model.GetObject()->Modify();Scene->Modify();Selected->Modify();
    TArray<FFrameNumber> Times;Times.Reserve(Expected.Num());
    for (int32 Index=0;Index<Expected.Num();++Index) Times.Add(FFrameNumber(Index));
    for (int32 Axis=0;Axis<3;++Axis)
    {
        TArray<FMovieSceneFloatValue> Values;Values.Reserve(Expected.Num());
        for (const FVector& Euler:EulerKeys)
        {
            FMovieSceneFloatValue& Value=Values.AddDefaulted_GetRef();
            Value.Value=float(Euler[Axis]);Value.InterpMode=ERichCurveInterpMode::RCIM_Linear;
        }
        Parameters->Rotation[Axis].Reset();Parameters->Rotation[Axis].SetKeysOnly(Times,Values);
    }
    Animation->GetController().NotifyPopulated();
    TArray<FTransform> Actual;Model->GetBoneTrackTransforms(BoneName,Actual);
    bool bMatches=Actual.Num()==Expected.Num();
    for (int32 Index=0;bMatches && Index<Actual.Num();++Index)
        bMatches=Actual[Index].GetRotation().GetNormalized().AngularDistance(Expected[Index])<=1.e-5;
    if (!bMatches)
    {
        for (int32 Axis=0;Axis<3;++Axis) Parameters->Rotation[Axis]=Previous[Axis];
        Animation->GetController().NotifyPopulated();
        return TEXT("Rotation readback did not preserve the requested keys. Previous channels restored; do not save this failed operation.");
    }
    Animation->MarkPackageDirty();
    return FString();
}
