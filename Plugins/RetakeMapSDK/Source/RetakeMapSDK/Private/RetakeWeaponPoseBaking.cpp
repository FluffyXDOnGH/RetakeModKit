// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeWeaponPoseBaking.h"
#if WITH_EDITOR
#include "RetakeWeaponGeometry.h"
#include "RetakeWeaponSpecification.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimMontage.h"
#include "Animation/BlendSpace.h"
#include "Animation/AnimationPoseData.h"
#include "Animation/AttributesRuntime.h"
#include "AnimationRuntime.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/IConsoleManager.h"
#include "Misc/PackageName.h"
#include "Misc/MemStack.h"
#include "Misc/SecureHash.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"

namespace RetakeWeaponPoseBaking
{
void FContext::Read(const UObject* Source) { if (Source) Sources.Add(Source->GetOutermost()->GetFName()); }
FSamplingScope::FSamplingScope(int32 ForceRaw)
{
    check(IsInGameThread());
    // Registered by Engine for its lifetime; hundreds of samples share this lookup, not its changing value.
    static IConsoleVariable* const SamplingVariable=IConsoleManager::Get().FindConsoleVariable(TEXT("a.ForceEvalRawData"));
    Variable=SamplingVariable;
    Previous=Variable ? Variable->GetInt() : 0;
    if (Variable) Variable->Set(ForceRaw,ECVF_SetByCode);
}
FSamplingScope::~FSamplingScope() { if (Variable) Variable->Set(Previous,ECVF_SetByCode); }

bool FContext::Initialize(const USkeletalMesh& Reference,TConstArrayView<FRetakePoseBone> Catalogue,FString& Error)
{
    check(IsInGameThread());Error.Reset();
    const auto& Ref=Reference.GetRefSkeleton();
    if (!Reference.GetSkeleton() || Ref.GetNum()<1 || Ref.GetNum()>MAX_uint16 || Catalogue.IsEmpty() || Catalogue.Num()>RetakeWeaponPoses::MaximumBones)
    {Error=TEXT("Weapon-pose preparation needs a reference mesh, skeleton and supported bone catalogue.");return false;}
    TArray<int32> Indices;TSet<FName> Names;
    for (int32 I=0;I<Catalogue.Num();++I)
    {
        const auto& Bone=Catalogue[I];const int32 Index=Ref.FindBoneIndex(Bone.Name);
        if (Index==INDEX_NONE || Names.Contains(Bone.Name) || (I==0 ? Bone.Parent!=INDEX_NONE : Bone.Parent<0 || Bone.Parent>=I)
            || Ref.GetParentIndex(Index)!=(Bone.Parent==INDEX_NONE ? INDEX_NONE:Indices[Bone.Parent])
            || !Ref.GetRefBonePose()[Index].Equals(Bone.Reference(),.00001))
        {Error=TEXT("Reference mesh does not match the weapon-pose body contract at bone: ")+Bone.Name.ToString();return false;}
        Indices.Add(Index);Names.Add(Bone.Name);
    }
    Mesh=&Reference;MeshIndices=MoveTemp(Indices);Required.Reset();Sources.Reset();
    for (int32 I=0;I<Ref.GetNum();++I) Required.Add(FBoneIndexType(I));
    Bones.InitializeTo(Required,UE::Anim::FCurveFilterSettings(),Reference);
    return true;
}

bool SamplePose(FContext& Context, const UAnimSequence* Sequence, FCompactPose& Out, bool bRuntimeData)
{
    if (!Sequence) return false;
    Context.Read(Sequence);
    Out.SetBoneContainer(&Context.Bones);
    FBlendedCurve Curve;
    Curve.InitFrom(Context.Bones);
    UE::Anim::FStackAttributeContainer Attributes;
    FAnimationPoseData Data(Out, Curve, Attributes);
    // Mesh-space additive retargeting on the canonical mesh can differ between raw authoring and
    // runtime data. Lean endpoints must be the same representation the displayed AO player samples.
    const FSamplingScope Sampling(bRuntimeData ? 0 : 1);
    if (bRuntimeData) const_cast<UAnimSequence*>(Sequence)->CacheDerivedDataForCurrentPlatform();
    FAnimExtractContext Extract(0.0, false);
    Extract.bEnforceCompressedDataSampling = bRuntimeData;
    Sequence->GetAnimationPose(Data, Extract);
    return true;
}

static void Store(FContext& Context, const FCompactPose& Pose, FRetakeHitRigHold& Hold, ERetakeHitRigPose Which)
{
    const int32 N = Context.MeshIndices.Num();
    for (int32 I = 0; I < N; ++I)
    {
        const FCompactPoseBoneIndex Index = Context.Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(Context.MeshIndices[I]));
        Hold.Poses[int32(Which) * N + I] = Pose[Index];
    }
}

/** Extreme samples of the authored 1D mesh-space lean offset. */
static void LeanSamples(const UBlendSpace* Offset, const UAnimSequence*& Left, const UAnimSequence*& Right)
{
    Left = Right = nullptr;
    if (!Offset) return;
    float Low = TNumericLimits<float>::Max(), High = TNumericLimits<float>::Lowest();
    for (const FBlendSample& Sample : Offset->GetBlendSamples())
    {
        if (!Sample.Animation) continue;
        if (Sample.SampleValue.X < Low) { Low = Sample.SampleValue.X; Left = Sample.Animation; }
        if (Sample.SampleValue.X > High) { High = Sample.SampleValue.X; Right = Sample.Animation; }
    }
}

/** Preserve upper-body component rotations while replacing the shared lower-body pose. */
static void OverrideLowerBody(FContext& Context, FCompactPose& Pose, const FCompactPose* Lower)
{
    if (!Lower) return;
    // The graph layers the weapon upper body in mesh space: keep spine_01's component rotation over the new pelvis.
    const int32 SpineMesh = Context.Mesh->GetRefSkeleton().FindBoneIndex(TEXT("spine_01"));
    const FCompactPoseBoneIndex Spine = SpineMesh != INDEX_NONE ? Context.Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(SpineMesh))
        : FCompactPoseBoneIndex(INDEX_NONE);
    FQuat SpineRotation = FQuat::Identity;
    if (Spine.IsValid())
    {
        FCSPose<FCompactPose> Before;
        Before.InitPose(Pose);
        SpineRotation = Before.GetComponentSpaceTransform(Spine).GetRotation();
    }
    for (const FCompactPoseBoneIndex Index : Pose.ForEachBoneIndex())
    {
        // The body owns root and every branch outside spine_01, not only bones with leg names.
        // This is the same hierarchy mask used by the public body contract and mesh-space layering.
        FCompactPoseBoneIndex Ancestor=Index;
        while (Ancestor.IsValid() && Ancestor!=Spine) Ancestor=Context.Bones.GetParentBoneIndex(Ancestor);
        if (!Ancestor.IsValid()) Pose[Index]=(*Lower)[Index];
    }
    if (Spine.IsValid())
    {
        FCSPose<FCompactPose> After;
        After.InitPose(Pose);
        const FCompactPoseBoneIndex Parent = Context.Bones.GetParentBoneIndex(Spine);
        const FQuat ParentRotation = Parent.IsValid() ? After.GetComponentSpaceTransform(Parent).GetRotation() : FQuat::Identity;
        Pose[Spine].SetRotation((ParentRotation.Inverse() * SpineRotation).GetNormalized());
    }
}

static ERetakeHitRigLayer LayerFor(const UAnimSequenceBase* Clip)
{
    const auto* Sequence = Cast<UAnimSequence>(Clip);
    if (!Sequence || !Sequence->IsValidAdditive()) return ERetakeHitRigLayer::Absolute;
    return Sequence->GetAdditiveAnimType() == AAT_RotationOffsetMeshSpace ? ERetakeHitRigLayer::MeshAdditive : ERetakeHitRigLayer::LocalAdditive;
}

void ForEachRuntimeActionPose(FContext& Context,const FAnimTrack* Slot,const UAnimSequence* Sequence,
    ERetakeHitRigLayer Kind,float Length,float SampleRate,TFunctionRef<void(const FCompactPose&)> Read)
{
    const FSamplingScope RuntimeSampling(0);
    if (Slot)
    {
        for (const auto& Segment:Slot->AnimSegments)
            if (auto* Clip=Cast<UAnimSequence>(Segment.GetAnimReference())) Clip->CacheDerivedDataForCurrentPlatform();
    }
    else if (Sequence) const_cast<UAnimSequence*>(Sequence)->CacheDerivedDataForCurrentPlatform();
    const int32 Samples=FMath::Max(2,FMath::CeilToInt(Length*SampleRate)+1);
    for (int32 S=0;S<Samples;++S)
    {
        FCompactPose Pose;Pose.SetBoneContainer(&Context.Bones);
        if (Kind!=ERetakeHitRigLayer::Absolute) Pose.ResetToAdditiveIdentity();
        FBlendedCurve Curve;Curve.InitFrom(Context.Bones);
        UE::Anim::FStackAttributeContainer Attributes;FAnimationPoseData Data(Pose,Curve,Attributes);
        FAnimExtractContext Extract(FMath::Min(double(S)/SampleRate,double(Length)),false);
        Extract.bEnforceCompressedDataSampling=true;
        if (Slot) Slot->GetAnimationPose(Data,Extract);else Sequence->GetAnimationPose(Data,Extract);
        Read(Pose);
    }
}

void SampleActionLayer(FContext& Context,const FAnimTrack* Slot,const UAnimSequence* Sequence,
    ERetakeHitRigLayer Kind,float Length,float SampleRate,TArray<FTransform3f>& Out)
{
    Out.Reset();
    Out.Reserve(FMath::Max(2,FMath::CeilToInt(Length*SampleRate)+1)*Context.MeshIndices.Num());
    ForEachRuntimeActionPose(Context,Slot,Sequence,Kind,Length,SampleRate,[&](const FCompactPose& Pose)
    {
        for (const int32 MeshIndex:Context.MeshIndices)
            Out.Add(FTransform3f(Pose[Context.Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshIndex))]));
    });
}

/** Dense gun-relative placement and curve samples retain their established sampling policy. */
static void SampleGunBone(FContext& Context, const UAnimMontage* Montage, const FAnimTrack* Main, const UAnimSequence* Sequence, FRetakeHitRigTrack& Track)
{
    static const FName Curve(TEXT("RetakeWeaponOnGunBone"));
    Track.GunInHand.Reset();
    Track.GunWeight.Reset();
    const FReferenceSkeleton& Ref = Context.Mesh->GetRefSkeleton();
    const int32 Gun = Ref.FindBoneIndex(TEXT("ik_hand_gun")), Hand = Ref.FindBoneIndex(TEXT("hand_r"));
    if (Gun == INDEX_NONE || Hand == INDEX_NONE || (!Main && !Sequence) || Track.MainLayer != ERetakeHitRigLayer::Absolute) return;
    const int32 Samples = FMath::Max(2, FMath::CeilToInt(Track.Length * Track.GunSampleRate) + 1);
    TArray<FTransform3f> InHand;
    TArray<float> Weights;
    bool bMoves = false;
    for (int32 S = 0; S < Samples; ++S)
    {
        const double Time = FMath::Min(double(S) / Track.GunSampleRate, double(Track.Length));
        float Weight = 0.f;
        if (Montage)
        {
            for (const FSlotAnimationTrack& Slot : Montage->SlotAnimTracks)
                if (const FAnimSegment* Segment = Slot.AnimTrack.GetSegmentAtTime(float(Time)))
                    if (const UAnimSequenceBase* Clip = Segment->GetAnimReference())
                        Weight = FMath::Max(Weight, Clip->EvaluateCurveData(Curve, FAnimExtractContext(double(Segment->ConvertTrackPosToAnimPos(float(Time))))));
        }
        else Weight = Sequence->EvaluateCurveData(Curve, FAnimExtractContext(Time));
        Weight = FMath::Clamp(Weight, 0.f, 1.f);
        bMoves |= Weight > UE_KINDA_SMALL_NUMBER;
        FCompactPose Pose;
        Pose.SetBoneContainer(&Context.Bones);
        FBlendedCurve Curves;
        Curves.InitFrom(Context.Bones);
        UE::Anim::FStackAttributeContainer Attributes;
        FAnimationPoseData Data(Pose, Curves, Attributes);
        if (Main) Main->GetAnimationPose(Data, FAnimExtractContext(Time, false));
        else Sequence->GetAnimationPose(Data, FAnimExtractContext(Time, false));
        FCSPose<FCompactPose> Space;
        Space.InitPose(Pose);
        const FTransform GunCS = Space.GetComponentSpaceTransform(Context.Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(Gun)));
        const FTransform HandCS = Space.GetComponentSpaceTransform(Context.Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(Hand)));
        InHand.Add(FTransform3f(GunCS.GetRelativeTransform(HandCS)));
        Weights.Add(Weight);
    }
    if (!bMoves) return;
    Track.GunInHand = MoveTemp(InHand);
    Track.GunWeight = MoveTemp(Weights);
}

bool BakeTrack(FContext& Context, const UAnimMontage* Montage, const UAnimSequence* Sequence,
    FRetakeHitRigTrack& Track,float MinimumSampleRate)
{
    Track = {};
    const FAnimTrack* Main = nullptr;
    const FAnimTrack* Extra = nullptr;
    const FAnimTrack* Aiming = nullptr;
    const UAnimSequenceBase* MainClip = Sequence;
    const UAnimSequenceBase* ExtraClip = nullptr;
    const UAnimSequenceBase* AimingClip = nullptr;
    if (Montage)
        for (const FSlotAnimationTrack& Candidate : Montage->SlotAnimTracks)
        {
            if (Candidate.AnimTrack.AnimSegments.IsEmpty()) continue;
            const UAnimSequenceBase* Clip = Candidate.AnimTrack.AnimSegments[0].GetAnimReference();
            if (!Main && (Candidate.SlotName == TEXT("DefaultSlot") || Candidate.SlotName == TEXT("UpperBody"))) { Main = &Candidate.AnimTrack; MainClip = Clip; }
            else if (!Extra && (Candidate.SlotName == TEXT("UpperBodyAdditive") || Candidate.SlotName == TEXT("UpperBodyDynAdditive"))) { Extra = &Candidate.AnimTrack; ExtraClip = Clip; }
            else if (Candidate.SlotName == TEXT("FullBody")) Track.bFullBody = true;
            else if (!Aiming && Candidate.SlotName == TEXT("Aiming")) { Aiming = &Candidate.AnimTrack; AimingClip = Clip; }
        }
    if (Montage && !Main && !Extra) return false;
    if (!Montage && !Sequence) return false;
    Track.SourceAnimation=const_cast<UAnimSequenceBase*>(Montage ? static_cast<const UAnimSequenceBase*>(Montage):Sequence);
    Context.Read(Montage ? static_cast<const UObject*>(Montage) : Sequence);
    Track.Length = Montage ? Montage->GetPlayLength() : Sequence->GetPlayLength();
    Track.RateScale = Montage ? Montage->RateScale : 1.f;
    Track.BlendIn = Montage ? Montage->BlendIn.GetBlendTime() : .035f;
    Track.BlendOut = Montage ? Montage->BlendOut.GetBlendTime() : .08f;
    if (!FMath::IsFinite(Track.Length) || Track.Length<=0.f || Track.Length>RetakeWeaponPoses::MaximumTrackSeconds
        || !FMath::IsFinite(MinimumSampleRate) || MinimumSampleRate<1.f || MinimumSampleRate>RetakeWeaponPoses::MaximumSampleRate
        || Context.MeshIndices.IsEmpty() || Context.MeshIndices.Num()>RetakeWeaponPoses::MaximumBones) return false;
    Track.SampleRate=MinimumSampleRate; // Local-transform interpolation at action speed; authored dense overrides remain supported.
    // Re-interpolating quaternion samples between source keys adds a second
    // normalization path. For simple montages, retain exactly the source
    // grid instead, including compressed additive deltas. Complex/remapped
    // tracks keep their dense bake; authored density overrides are respected.
    if (Montage && Main && !Extra && !Track.bFullBody)
    {
        const auto KeyRate=[&](const FAnimTrack* Slot)->float
        {
            if (!Slot || Slot->AnimSegments.Num()!=1) return 0.f;
            const auto& Segment=Slot->AnimSegments[0];
            const auto* Clip=Cast<UAnimSequence>(Segment.GetAnimReference());
            if (!Clip || Clip->Interpolation!=EAnimInterpolationType::Linear || Segment.LoopingCount!=1
                || !FMath::IsNearlyZero(Segment.StartPos) || !FMath::IsNearlyZero(Segment.AnimStartTime)
                || !FMath::IsNearlyEqual(Segment.AnimPlayRate,1.f) || !FMath::IsNearlyEqual(Clip->RateScale,1.f)
                || !FMath::IsNearlyEqual(Segment.AnimEndTime,Clip->GetPlayLength())
                || !FMath::IsNearlyEqual(Track.Length,Clip->GetPlayLength())
                || Clip->EvaluateCurveData(TEXT("RetakeHitRigSampleRate"),FAnimExtractContext(0.0))>0.f) return 0.f;
            const float Rate=float(Clip->GetSamplingFrameRate().AsDecimal());
            return Rate>=1.f && Rate<=600.f && FMath::IsNearlyEqual(Rate,float(FMath::RoundToInt(Rate))) ? Rate:0.f;
        };
        const float Rate=KeyRate(Main),AimRate=Aiming ? KeyRate(Aiming):Rate;
        if (Rate>0.f && Rate==AimRate) {Track.SampleRate=Rate;Track.bSourceKeyAligned=true;}
    }
    // Bare action clips retain their established dense bake and explicit
    // authored overrides for independently timed contact/weapon curves.
    if (Sequence)
    {
        const float RequestedRate = Sequence->EvaluateCurveData(TEXT("RetakeHitRigSampleRate"), FAnimExtractContext(0.0));
        if (FMath::IsFinite(RequestedRate) && RequestedRate > Track.SampleRate)
            Track.SampleRate = FMath::Clamp(RequestedRate, Track.SampleRate, 600.f);
    }
    const int32 Layers=int32(Main || Sequence)+int32(Extra!=nullptr)+int32(Aiming!=nullptr);
    const int64 PoseSamples=int64(FMath::Max(2,FMath::CeilToInt(Track.Length*Track.SampleRate)+1))*Context.MeshIndices.Num()*Layers;
    const int64 GunSamples=FMath::Max(2,FMath::CeilToInt(Track.Length*FMath::Max(MinimumSampleRate,Track.SampleRate))+1);
    if (PoseSamples+GunSamples>RetakeWeaponPoses::MaximumTransforms) return false;
    if (Main || Sequence)
    {
        Track.GunSampleRate=FMath::Max(MinimumSampleRate,Track.SampleRate);
        Track.MainLayer = LayerFor(MainClip);
        SampleActionLayer(Context,Main,Main ? nullptr:Sequence,Track.MainLayer,Track.Length,Track.SampleRate,Track.MainPoses);
        SampleGunBone(Context, Montage, Main, Main ? nullptr : Sequence, Track);
    }
    if (Extra)
    {
        // The additive slot always adds; a non-additive clip there would be a content error, treated as absolute.
        Track.AdditiveLayer = LayerFor(ExtraClip);
        SampleActionLayer(Context,Extra,nullptr,Track.AdditiveLayer,Track.Length,Track.SampleRate,Track.AdditivePoses);
    }
    if (Aiming)
    {
        Track.AimingLayer = LayerFor(AimingClip);
        SampleActionLayer(Context,Aiming,nullptr,Track.AimingLayer,Track.Length,Track.SampleRate,Track.AimingPoses);
    }
    return true;
}

bool BakeHold(FContext& Context, const FPlayerAnims& Anims, const FRetakeThirdPersonAimPose& Aim, const UBlendSpace* StandLean,
    const UBlendSpace* CrouchLean, const UBlendSpace* StandMovingLean,const UBlendSpace* CrouchMovingLean,
    float LeanStrength, FName ProfileId, const FCompactPose* StandLower, const FCompactPose* CrouchLower,
    FRetakeHitRigHold& Hold, FString& Error)
{
    Hold = {};
    Hold.ProfileId = ProfileId;
    Hold.Aim = Aim;
    Hold.Poses.SetNum(Context.MeshIndices.Num() * int32(ERetakeHitRigPose::Count));
    const int32 SpineMesh=Context.Mesh->GetRefSkeleton().FindBoneIndex(TEXT("spine_01"));
    if (SpineMesh==INDEX_NONE) {Error=TEXT("Weapon posture requires the shared spine_01 upper-body root.");return false;}
    const FCompactPoseBoneIndex Spine=Context.Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(SpineMesh));
    const UAnimSequence* Stand = Anims.IdleAnim;
    const UAnimSequence* Crouch = Anims.CrouchIdleAnim ? Anims.CrouchIdleAnim.Get() : Stand;
    const UAnimSequence* Sights = Anims.AimIdleAnim ? Anims.AimIdleAnim.Get() : Stand;
    if (!Stand) { Error = FString::Printf(TEXT("%s has no third-person idle"), *ProfileId.ToString()); return false; }
    Hold.CarrySequences = {Anims.IdleAnim.Get(), Anims.CrouchIdleAnim ? Anims.CrouchIdleAnim.Get() : Anims.IdleAnim.Get(),
        Anims.AimIdleAnim ? Anims.AimIdleAnim.Get() : Anims.IdleAnim.Get(),
        Anims.SprintAnim ? Anims.SprintAnim.Get() : Anims.IdleAnim.Get()};
    if (!ProfileId.IsNone()) for (const UAnimSequence* Clip : Hold.CarrySequences)
        if (!Clip || !Clip->Notifies.IsEmpty())
        { Error = FString::Printf(TEXT("%s carry source %s needs an explicit notify policy"), *ProfileId.ToString(), *GetNameSafe(Clip)); return false; }
    FCompactPose StandPose, CrouchPose, Scratch;
    SamplePose(Context, Stand, StandPose);
    SamplePose(Context, Crouch, CrouchPose);
    OverrideLowerBody(Context, StandPose, StandLower);
    OverrideLowerBody(Context, CrouchPose, CrouchLower);
    Store(Context, StandPose, Hold, ERetakeHitRigPose::Stand);
    Store(Context, CrouchPose, Hold, ERetakeHitRigPose::Crouch);
    if (Anims.SprintAnim)
    {
        FCompactPose SprintPose;
        SamplePose(Context, Anims.SprintAnim, SprintPose);
        OverrideLowerBody(Context, SprintPose, StandLower);
        Store(Context, SprintPose, Hold, ERetakeHitRigPose::Sprint);
    }
    else Store(Context, StandPose, Hold, ERetakeHitRigPose::Sprint);
    SamplePose(Context, Sights, Scratch);
    OverrideLowerBody(Context, Scratch, StandLower);
    Store(Context, Scratch, Hold, ERetakeHitRigPose::StandAim);
    // No separate crouched aim asset: the crouched aim keeps the crouch legs and the aim upper body.
    Store(Context, Scratch, Hold, ERetakeHitRigPose::CrouchAim);
    for (int32 I = 0; I < Context.MeshIndices.Num(); ++I)
    {
        FCompactPoseBoneIndex Ancestor=Context.Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(Context.MeshIndices[I]));
        while (Ancestor.IsValid() && Ancestor!=Spine) Ancestor=Context.Bones.GetParentBoneIndex(Ancestor);
        const int32 N = Context.MeshIndices.Num();
        if (!Ancestor.IsValid()) Hold.Poses[int32(ERetakeHitRigPose::CrouchAim) * N + I] = Hold.Poses[int32(ERetakeHitRigPose::Crouch) * N + I];
    }
    const auto BakeLean = [&](const UBlendSpace* Offset, const FCompactPose& Base, ERetakeHitRigPose LeftPose, ERetakeHitRigPose RightPose)
    {
        const UAnimSequence *Left = nullptr, *Right = nullptr;
        LeanSamples(Offset, Left, Right);
        Context.Read(Offset);
        for (const TPair<const UAnimSequence*, ERetakeHitRigPose> Side : {TPair<const UAnimSequence*, ERetakeHitRigPose>(Left, LeftPose),
            TPair<const UAnimSequence*, ERetakeHitRigPose>(Right, RightPose)})
        {
            FCompactPose Pose = Base;
            if (Side.Key)
            {
                Context.Read(Side.Key->RefPoseSeq);
                auto& Metadata=Side.Key->GetOutermost()->GetMetaData();
                const FString SourcePath=Metadata.GetValue(Side.Key,TEXT("Retake.FullPoseSource"));
                if (!SourcePath.IsEmpty())
                {
                    const UAnimSequence* Source=LoadObject<UAnimSequence>(nullptr,*SourcePath);
                    FString File;
                    const FString Expected=Metadata.GetValue(Side.Key,TEXT("Retake.FullPoseSourceMD5"));
                    if (!Source || Source->GetOutermost()->IsDirty() || Expected.IsEmpty()
                        || !FPackageName::DoesPackageExist(Source->GetOutermost()->GetName(),&File)
                        || !Expected.Equals(LexToString(FMD5Hash::HashFile(*File)),ESearchCase::IgnoreCase))
                    {
                        Error=FString::Printf(TEXT("Generated lean offset %s is stale: save its source and regenerate the lean offset before preparing weapon poses."),*Side.Key->GetName());
                        return false;
                    }
                    Context.Read(Source);
                }
                FCompactPose Additive;
                if (!SamplePose(Context, Side.Key, Additive, true)) return false;
                FBlendedCurve BaseCurve, AdditiveCurve;
                BaseCurve.InitFrom(Context.Bones);
                AdditiveCurve.InitFrom(Context.Bones);
                UE::Anim::FStackAttributeContainer BaseAttributes, AdditiveAttributes;
                FAnimationPoseData BaseData(Pose, BaseCurve, BaseAttributes);
                const FAnimationPoseData AdditiveData(Additive, AdditiveCurve, AdditiveAttributes);
                FAnimationRuntime::AccumulateAdditivePose(BaseData, AdditiveData, LeanStrength, AAT_RotationOffsetMeshSpace);
                // Retake_WeaponProcedurals masks voluntary lean from spine_01 in mesh space.
                // Preserve the planted lower body and the additive's upper-body component rotations.
                OverrideLowerBody(Context, Pose, &Base);
            }
            Store(Context, Pose, Hold, Side.Value);
        }
        return true;
    };
    if (!BakeLean(StandLean, StandPose, ERetakeHitRigPose::StandLeanLeft, ERetakeHitRigPose::StandLeanRight)
        || !BakeLean(CrouchLean, CrouchPose, ERetakeHitRigPose::CrouchLeanLeft, ERetakeHitRigPose::CrouchLeanRight)
        || !BakeLean(StandMovingLean,StandPose,ERetakeHitRigPose::StandMovingLeanLeft,ERetakeHitRigPose::StandMovingLeanRight)
        || !BakeLean(CrouchMovingLean,CrouchPose,ERetakeHitRigPose::CrouchMovingLeanLeft,ERetakeHitRigPose::CrouchMovingLeanRight)) return false;
    return true;
}
void BakeHands(const USkeletalMesh& ReferenceMesh,const URetakeWeaponSpecification& Definition,FName SupportSocket,FRetakeHitRigHold& Hold)
{
    Hold.bSupportHand=false;Hold.SupportInHand=FTransform::Identity;
    Hold.bConvergence=false;Hold.GripInHand=FTransform::Identity;Hold.BarrelInHand=FVector::ZeroVector;
    Hold.bWeaponHeight=Hold.bSight=false;Hold.MuzzleInHand=FVector::ZeroVector;Hold.SightInHand=FTransform::Identity;
    Hold.ConvergenceDegrees=0.f;Hold.LeanElbowDownWeight=0.f;
    // Reference-pose geometry: weapon on the body's grip socket, then the weapon mesh's own sockets, all in hand_r space.
    const auto& P = Definition.Presentation;
    const USkeletalMesh* Weapon = P.WorldMesh.LoadSynchronous();
    const FReferenceSkeleton& Body = ReferenceMesh.GetRefSkeleton();
    const int32 HandBone = Body.FindBoneIndex(TEXT("hand_r"));
    const FName GripName = !P.ThirdPersonGripSocket.IsNone() ? P.ThirdPersonGripSocket : P.GripSocket;
    const USkeletalMeshSocket* Grip = ReferenceMesh.FindSocket(GripName);
    if (!Weapon || HandBone == INDEX_NONE || !Grip) return;
    const int32 GripBone = Body.FindBoneIndex(Grip->BoneName);
    if (GripBone == INDEX_NONE) return;
    const FTransform HandCS = FAnimationRuntime::GetComponentSpaceTransformRefPose(Body, HandBone);
    const FTransform GripBoneCS = FAnimationRuntime::GetComponentSpaceTransformRefPose(Body, GripBone);
    const FTransform GripInHand = Grip->GetSocketLocalTransform() * GripBoneCS.GetRelativeTransform(HandCS);
    const FTransform WeaponInHand = P.WorldOffset * GripInHand;
    Hold.GripInHand=GripInHand;
    const auto WeaponSocket=[Weapon](FName Name,FTransform& Out)
    { return RetakeWeaponGeometry::SocketTransform(*Weapon,Name,Out); };
    FTransform Support;
    if (WeaponSocket(SupportSocket, Support)) { Hold.bSupportHand = true; Hold.SupportInHand = Support * WeaponInHand; }
    FTransform Muzzle, AimInWeapon;
    Hold.bWeaponHeight=WeaponSocket(P.MuzzleSocket,Muzzle);
    if (Hold.bWeaponHeight) Hold.MuzzleInHand=WeaponInHand.TransformPosition(Muzzle.GetLocation());
    Hold.bSight=RetakeWeaponGeometry::ResolveAimAxis(Weapon,Definition.Aim.AimPoint,P.Attachments,AimInWeapon,true);
    if (Hold.bSight) Hold.SightInHand=AimInWeapon*WeaponInHand;
    if (P.Clearance.bEnabled && P.Clearance.ThirdPersonAimConvergenceDegrees > 0.f)
    {
        // The authored aim point's sight line when the weapon or one of its parts declares one (the same axis the
        // clearance component converges live); else the mesh's longest bounds axis, signed toward the muzzle.
        FVector Local = FVector::ZeroVector;
        bool bAxis = false;
        if (Hold.bSight)
        {
            Local = AimInWeapon.GetUnitAxis(EAxis::X);
            bAxis = !Local.IsNearlyZero();
        }
        else if (Hold.bWeaponHeight)
        {
            const FBox Box = Weapon->GetBounds().GetBox();
            const FVector Extent = Box.GetExtent();
            const int32 Axis = Extent.X >= Extent.Y ? (Extent.X >= Extent.Z ? 0 : 2) : (Extent.Y >= Extent.Z ? 1 : 2);
            const double Side = (Muzzle.GetLocation() - Box.GetCenter())[Axis];
            if (FMath::Abs(Side) >= 1.) { Local[Axis] = FMath::Sign(Side); bAxis = true; }
        }
        if (bAxis)
        {
            Hold.bConvergence = true;
            Hold.GripInHand = GripInHand;
            Hold.BarrelInHand = WeaponInHand.TransformVectorNoScale(Local).GetSafeNormal();
            Hold.ConvergenceDegrees = P.Clearance.ThirdPersonAimConvergenceDegrees;
            Hold.LeanElbowDownWeight = FMath::IsFinite(P.Clearance.ThirdPersonLeanElbowDownWeight)
                ? FMath::Clamp(P.Clearance.ThirdPersonLeanElbowDownWeight, 0.f, 1.f) : 0.f;
        }
    }
}

bool CaptureClearance(FContext& Context,const UBlendSpace* Space,FClearanceSource& Out,FString& Error)
{
    if (!Space) {Error=TEXT("Enabled weapon clearance requires standing and crouched authoring offsets.");return false;}
    Context.Read(Space);TArray<const FBlendSample*> Samples;
    for (const auto& Sample:Space->GetBlendSamples()) if (Sample.Animation) Samples.Add(&Sample);
    if (Samples.Num()<2 || Samples.Num()>1024) {Error=TEXT("Clearance requires 2-1024 authored samples.");return false;}
    Samples.Sort([](const auto& A,const auto& B){return A.SampleValue.X<B.SampleValue.X;});
    FClearanceSource Candidate;
    for (const auto* Sample:Samples)
    {
        Context.Read(Sample->Animation);TArray<FTransform3f> Poses;
        SampleActionLayer(Context,nullptr,Sample->Animation,ERetakeHitRigLayer::MeshAdditive,0.f,30.f,Poses);
        if (Poses.Num()<Context.MeshIndices.Num()) {Error=TEXT("Clearance source did not produce a complete pose.");return false;}
        Candidate.Keys.Add(Sample->SampleValue.X);Candidate.Poses.Append(Poses.GetData(),Context.MeshIndices.Num());
    }
    Out=MoveTemp(Candidate);return true;
}

bool BakeClearance(TConstArrayView<FRetakePoseBone> Bones,const FClearanceSource& Source,const FRetakeHitRigHold& Hold,
    ERetakeHitRigPose Pose,FRetakeHitRigArmClearance& Out,FString& Error)
{
    const FName ArmNames[]={TEXT("upperarm_r"),TEXT("lowerarm_r"),TEXT("hand_r"),TEXT("upperarm_l"),TEXT("lowerarm_l"),TEXT("hand_l")};
    const int32 N=Bones.Num();
    if (!N || N>RetakeWeaponPoses::MaximumBones || uint8(Pose)>=uint8(ERetakeHitRigPose::Count) || Source.Keys.Num()<2 || Source.Keys.Num()>1024
        || Source.Poses.Num()!=Source.Keys.Num()*N || Hold.Poses.Num()!=int32(ERetakeHitRigPose::Count)*N)
    {Error=TEXT("Incomplete clearance source or hold pose layout.");return false;}
    int32 Arms[6];
    for (int32 I=0;I<6;++I)
    {
        Arms[I]=INDEX_NONE;for (int32 Bone=0;Bone<N;++Bone) if (Bones[Bone].Name==ArmNames[I]) Arms[I]=Bone;
        if (Arms[I]==INDEX_NONE || Bones[Arms[I]].Parent<0 || Bones[Arms[I]].Parent>=Arms[I])
        {Error=TEXT("Weapon clearance requires the complete shared arm hierarchy.");return false;}
    }
    TArray<FQuat> Base;Base.SetNum(N);
    for (int32 I=0;I<N;++I)
    {
        if (Bones[I].Parent>=I || Bones[I].Parent<INDEX_NONE) {Error=TEXT("Invalid clearance bone hierarchy.");return false;}
        Base[I]=((Bones[I].Parent==INDEX_NONE ? FQuat::Identity:Base[Bones[I].Parent])*Hold.Poses[int32(Pose)*N+I].GetRotation()).GetNormalized();
    }
    FRetakeHitRigArmClearance Candidate;Candidate.Keys=Source.Keys;
    for (int32 Key=0;Key<Source.Keys.Num();++Key) for (int32 Arm:Arms)
    {
        const int32 Parent=Bones[Arm].Parent;
        const FQuat ParentRotation=FQuat(Source.Poses[Key*N+Parent].GetRotation())*Base[Parent];
        const FQuat ArmRotation=FQuat(Source.Poses[Key*N+Arm].GetRotation())*Base[Arm];
        Candidate.Rotations.Add(FQuat4f((ParentRotation.Inverse()*ArmRotation).GetNormalized()));
    }
    if (!Candidate.IsValid()) {Error=TEXT("Clearance offsets require finite ordered keys covering 0..1 and valid arm rotations.");return false;}
    Out=MoveTemp(Candidate);return true;
}

bool BakeWeapon(const URetakeWeaponSpecification& Definition,const FInputs& Inputs,FRetakeHitRigHold& Out,
    TSet<FName>& OutSources,FString& Error)
{
    check(IsInGameThread());Error.Reset();
    if (!Inputs.ReferenceMesh || !Inputs.StandLower || !Inputs.CrouchLower || !FMath::IsFinite(Inputs.LeanStrength)
        || Inputs.LeanStrength<0.f || Inputs.LeanStrength>1.f || Inputs.SupportSocket.IsNone())
    {Error=TEXT("Weapon preparation requires a complete reference body/lower-body basis and valid lean/support settings.");return false;}
    if (!Inputs.Body.Validate(Error)) return false;
    if (!Inputs.ReferenceMesh->GetSkeleton()) {Error=TEXT("Reference mesh has no animation skeleton.");return false;}
    if (!Inputs.Body.MatchesAnimationSkeleton(*Inputs.ReferenceMesh->GetSkeleton(),Error)) return false;
    const auto& P=Definition.Presentation;
    const URetakeWeaponAnimationProfile* Profile=P.CharacterProfile.LoadSynchronous();
    if (!Profile || Profile->GetClass()!=URetakeWeaponAnimationProfile::StaticClass() || Profile->ProfileId.IsNone())
    {Error=TEXT("Weapon preparation requires a native, identified character profile.");return false;}
    FMemMark Mark(FMemStack::Get());FSamplingScope Sampling;
    FContext Context;if (!Context.Initialize(*Inputs.ReferenceMesh,Inputs.Body.Bones,Error)) return false;
    Context.Read(&Definition);Context.Read(Profile);Context.Read(Inputs.ReferenceMesh);
    FCompactPose Stand,Crouch;
    if (!SamplePose(Context,Inputs.StandLower,Stand) || !SamplePose(Context,Inputs.CrouchLower,Crouch))
    {Error=TEXT("Could not sample the shared lower-body basis.");return false;}
    FRetakeHitRigHold Candidate;
    if (!BakeHold(Context,Profile->ThirdPersonAnims,Profile->ThirdPersonAim,Inputs.StandLean,Inputs.CrouchLean,
        Inputs.StandMovingLean,Inputs.CrouchMovingLean,Inputs.LeanStrength,Profile->ProfileId,&Stand,&Crouch,Candidate,Error)) return false;
    Candidate.Tracks.SetNum(int32(ERetakeHitRigTrack::Count));Candidate.bContinuousReloadStages=Profile->bContinuousThirdPersonReloadStages;
    int64 TransformCount=Candidate.Poses.Num();
    const auto Track=[&](ERetakeHitRigTrack Kind,const UAnimMontage* Montage,const UAnimSequence* Sequence,float Rate=120.f)
    {
        if (!Montage && !Sequence) return true;
        auto& Result=Candidate.Tracks[int32(Kind)];
        if (!BakeTrack(Context,Montage,Sequence,Result,Rate) || !Result.IsSet() || Result.bFullBody || Result.AdditiveLayer==ERetakeHitRigLayer::Absolute)
        {Error=FString::Printf(TEXT("Unsupported or incomplete weapon action track %d for %s."),int32(Kind),*Profile->ProfileId.ToString());return false;}
        TransformCount+=int64(Result.MainPoses.Num())+Result.AdditivePoses.Num()+Result.AimingPoses.Num()+Result.GunInHand.Num();
        if (TransformCount>RetakeWeaponPoses::MaximumTransforms) {Error=TEXT("Prepared weapon exceeds the pose sample budget.");return false;}
        return true;
    };
    const auto& M=Profile->ThirdPersonMontages;
    if (!Track(ERetakeHitRigTrack::Reload,M.ReloadMontage,nullptr) || !Track(ERetakeHitRigTrack::Equip,M.EquipMontage,nullptr)
        || !Track(ERetakeHitRigTrack::Holster,M.HolsterMontage,nullptr) || !Track(ERetakeHitRigTrack::Drop,M.DropMontage,nullptr)
        || !Track(ERetakeHitRigTrack::Inspect,M.InspectMontage,nullptr) || !Track(ERetakeHitRigTrack::BoltCycle,Profile->ThirdPersonBoltCycle,nullptr)) return false;
    const auto Sequence=[&](const TSoftObjectPtr<UAnimSequence>& Source,ERetakeHitRigTrack Kind,float Rate=120.f,bool bReload=false)
    {
        if (Source.IsNull()) return true;
        const UAnimSequence* Clip=Source.LoadSynchronous();
        if (!Clip) {Error=TEXT("Missing declared weapon action source: ")+Source.ToString();return false;}
        if (!Track(Kind,nullptr,Clip,Rate)) return false;
        if (bReload) {Candidate.Tracks[int32(Kind)].BlendIn=.025f;Candidate.Tracks[int32(Kind)].BlendOut=.04f;}
        return true;
    };
    if (P.BashVariants.Num()>8) {Error=TEXT("Weapon preparation supports at most eight paired bash variants.");return false;}
    if (!P.BashVariants.IsEmpty() && P.BashVariants[0].ThirdPersonAnimation.IsNull())
    {Error=TEXT("The declared primary bash variant has no third-person source.");return false;}
    if (!Sequence(P.BashVariants.IsEmpty() ? P.ThirdPersonBashAnimation:P.BashVariants[0].ThirdPersonAnimation,ERetakeHitRigTrack::Bash)) return false;
    for (int32 I=1;I<P.BashVariants.Num();++I)
    {
        const UAnimSequence* Clip=P.BashVariants[I].ThirdPersonAnimation.LoadSynchronous();
        auto& Result=Candidate.AdditionalBashTracks.AddDefaulted_GetRef();
        if (!Clip || !BakeTrack(Context,nullptr,Clip,Result) || !Result.IsSet() || Result.bFullBody)
        {Error=TEXT("Missing or unsupported alternate bash source.");return false;}
        TransformCount+=int64(Result.MainPoses.Num())+Result.AdditivePoses.Num()+Result.AimingPoses.Num()+Result.GunInHand.Num();
        if (TransformCount>RetakeWeaponPoses::MaximumTransforms) {Error=TEXT("Prepared weapon exceeds the pose sample budget.");return false;}
    }
    if (!Sequence(P.ThirdPersonReloadStart,ERetakeHitRigTrack::ReloadStart,120.f,true)
        || !Sequence(P.ThirdPersonReloadInsert,ERetakeHitRigTrack::ReloadInsert,120.f,true)
        || !Sequence(P.ThirdPersonReloadEnd,ERetakeHitRigTrack::ReloadEnd,120.f,true)
        || !Sequence(P.ThirdPersonGrenade.PrepareLong,ERetakeHitRigTrack::GrenadePrepareLong,600.f)
        || !Sequence(P.ThirdPersonGrenade.PrepareShort,ERetakeHitRigTrack::GrenadePrepareShort,600.f)
        || !Sequence(P.ThirdPersonGrenade.ReleaseLong,ERetakeHitRigTrack::GrenadeReleaseLong,600.f)
        || !Sequence(P.ThirdPersonGrenade.ReleaseShort,ERetakeHitRigTrack::GrenadeReleaseShort,600.f)) return false;
    Candidate.bClearance=P.Clearance.bEnabled;
    BakeHands(*Inputs.ReferenceMesh,Definition,Inputs.SupportSocket,Candidate);
    if (Candidate.bClearance)
    {
        FClearanceSource StandOffset,CrouchOffset;
        if (!CaptureClearance(Context,Inputs.ClearanceStand,StandOffset,Error) || !CaptureClearance(Context,Inputs.ClearanceCrouch,CrouchOffset,Error)
            || !BakeClearance(Inputs.Body.Bones,StandOffset,Candidate,ERetakeHitRigPose::Stand,Candidate.ClearanceStand,Error)
            || !BakeClearance(Inputs.Body.Bones,CrouchOffset,Candidate,ERetakeHitRigPose::Crouch,Candidate.ClearanceCrouch,Error)) return false;
    }
    TArray<int32> Lower;Inputs.Body.GetLowerBodyBones(Lower);
    for (int32 Pose=0;Pose<int32(ERetakeHitRigPose::Count);++Pose)
        for (int32 I=0;I<Lower.Num();++I)
        {
            const FTransform& Actual=Candidate.Poses[Pose*Inputs.Body.Bones.Num()+Lower[I]];
            const FTransform Expected=Inputs.Body.LowerBodyPoses[Pose*Lower.Num()+I].Get();
            if (!Actual.Equals(Expected,.00001))
            {
                Error=FString::Printf(TEXT("Prepared weapon %ls changes the shared lower-body contract at pose %d, bone %ls (position delta %.9g cm, rotation delta %.9g degrees, scale delta %.9g); correct its authoring basis/source pose."),
                    *Profile->ProfileId.ToString(),Pose,*Inputs.Body.Bones[Lower[I]].Name.ToString(),
                    FVector::Distance(Actual.GetTranslation(),Expected.GetTranslation()),
                    FMath::RadiansToDegrees(Actual.GetRotation().AngularDistance(Expected.GetRotation())),
                    FVector::Distance(Actual.GetScale3D(),Expected.GetScale3D()));
                return false;
            }
        }
    Out=MoveTemp(Candidate);OutSources=MoveTemp(Context.Sources);return true;
}

}
#endif
