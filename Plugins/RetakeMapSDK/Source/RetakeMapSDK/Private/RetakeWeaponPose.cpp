// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeWeaponPose.h"
#include "RetakeWeaponPack.h"
#include "RetakeWeaponSpecification.h"
#include "RetakeMapSDKManifest.h"
#include "RetakeMapSDKVersion.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimMontage.h"
#include "Animation/Skeleton.h"
#include "Hash/Blake3.h"
#include "Misc/DataValidation.h"
#include "UObject/Package.h"
#include "UObject/Class.h"

namespace
{
    bool Digest(const FString& Value)
    {
        if (Value.Len()!=64) return false;
        for (TCHAR C:Value) if (!((C>=TEXT('0') && C<=TEXT('9')) || (C>=TEXT('a') && C<=TEXT('f')))) return false;
        return true;
    }
    bool OwnedResource(const FString& Owner,const UObject* Object)
    {
        if (!Object) return false;
        const FSoftObjectPath Path(Object);
        if (URetakeWeaponPack::IsOwnedObjectPath(Owner,Path)) return true;
        for (const TCHAR* Root:RetakeMapSDK::AllowedForeignRoots)
            if (Path.GetLongPackageName().StartsWith(Root,ESearchCase::IgnoreCase)) return true;
        return false;
    }
    // Fixed-width scalars, UTF-8 names and explicit components; no padding, FName indices or pointer bytes.
    // The qualified Windows/Linux targets are little endian. Stream samples instead of allocating another full payload.
    struct FIdentity
    {
        static_assert(PLATFORM_LITTLE_ENDIAN,"Weapon pose identity requires a qualified little-endian target.");
        FBlake3 Hash;
        template<typename T> void Number(T Value) { Hash.Update(&Value,sizeof(Value)); }
        void Text(FString Value)
        {
            Value=Value.ToLower(); const FTCHARToUTF8 Utf8(*Value);
            Number(int32(Utf8.Length())); Hash.Update(Utf8.Get(),Utf8.Length());
        }
        template<typename T> void Transform(const T& Value)
        {
            const auto Q=Value.GetRotation();const auto P=Value.GetTranslation();const auto S=Value.GetScale3D();
            const auto Values={Q.X,Q.Y,Q.Z,Q.W,P.X,P.Y,P.Z,S.X,S.Y,S.Z};
            Hash.Update(Values.begin(),Values.size()*sizeof(Q.X));
        }
        template<typename T> void Transforms(const TArray<T>& Values)
        { Number(int32(Values.Num()));for (const auto& Value:Values) Transform(Value); }
        void Clearance(const FRetakeHitRigArmClearance& Value)
        {
            Number(int32(Value.Keys.Num()));for (float Key:Value.Keys) Number(Key);
            Number(int32(Value.Rotations.Num()));
            for (const auto& Q:Value.Rotations) { Number(Q.X);Number(Q.Y);Number(Q.Z);Number(Q.W); }
        }
        void Track(const FRetakeHitRigTrack& Value)
        {
            Text(Value.SourceAnimation ? Value.SourceAnimation->GetPathName() : FString());
            Number(Value.Length);Number(Value.SampleRate);Number(uint8(Value.bSourceKeyAligned));Number(Value.RateScale);
            Number(Value.BlendIn);Number(Value.BlendOut);Number(uint8(Value.bFullBody));
            Number(uint8(Value.MainLayer));Transforms(Value.MainPoses);
            Number(uint8(Value.AdditiveLayer));Transforms(Value.AdditivePoses);
            Number(uint8(Value.AimingLayer));Transforms(Value.AimingPoses);
            Transforms(Value.GunInHand);Number(int32(Value.GunWeight.Num()));
            for (float Weight:Value.GunWeight) Number(Weight);
            Number(Value.GunSampleRate);
        }
        void Hold(const FRetakeHitRigHold& Value)
        {
            Text(Value.ProfileId.ToString());
            const auto& Aim=Value.Aim;
            for (float V:{Aim.Spine01,Aim.Spine02,Aim.Spine03,Aim.Spine04,Aim.Spine05,Aim.Arms,Aim.Neck,Aim.Head,
                Aim.MaxPitchUpDegrees,Aim.MaxPitchDownDegrees,Aim.MaxYawDegrees}) Number(V);
            Transforms(Value.Poses);Number(int32(Value.CarrySequences.Num()));
            for (const UAnimSequence* Clip:Value.CarrySequences) Text(Clip ? Clip->GetPathName() : FString());
            Number(int32(Value.Tracks.Num()));for (const auto& Clip:Value.Tracks) Track(Clip);
            Number(uint8(Value.bContinuousReloadStages));Number(int32(Value.AdditionalBashTracks.Num()));
            for (const auto& Clip:Value.AdditionalBashTracks) Track(Clip);
            Number(uint8(Value.bClearance));Clearance(Value.ClearanceStand);Clearance(Value.ClearanceCrouch);
            Number(uint8(Value.bSupportHand));Transform(Value.SupportInHand);Number(uint8(Value.bConvergence));Transform(Value.GripInHand);
            Number(Value.BarrelInHand.X);Number(Value.BarrelInHand.Y);Number(Value.BarrelInHand.Z);
            Number(Value.ConvergenceDegrees);Number(Value.LeanElbowDownWeight);
        }
        FString Finish() const { return LexToString(Hash.Finalize()).ToLower(); }
    };
    bool SkeletonMatches(const UAnimSequenceBase& Animation,const FRetakeWeaponPoseContract& Body)
    {
        const USkeleton* Skeleton=Animation.GetSkeleton();if (!Skeleton) return false;
        FString Error;return Body.MatchesAnimationSkeleton(*Skeleton,Error);
    }
    bool ValidateTrack(const FRetakeHitRigTrack& Track,const FString& Owner,const FRetakeWeaponPoseContract& Body,int64& Total)
    {
        if (Track.bFullBody || Track.AdditiveLayer==ERetakeHitRigLayer::Absolute || !FMath::IsFinite(Track.Length) || Track.Length<0.f || Track.Length>RetakeWeaponPoses::MaximumTrackSeconds
            || !FMath::IsFinite(Track.SampleRate) || Track.SampleRate<1.f || Track.SampleRate>RetakeWeaponPoses::MaximumSampleRate
            || !FMath::IsFinite(Track.GunSampleRate) || Track.GunSampleRate<1.f || Track.GunSampleRate>RetakeWeaponPoses::MaximumSampleRate
            || !FMath::IsFinite(Track.RateScale) || Track.RateScale<=0.f
            || !FMath::IsFinite(Track.BlendIn) || Track.BlendIn<0.f || !FMath::IsFinite(Track.BlendOut) || Track.BlendOut<0.f) return false;
        if (!Track.SourceAnimation)
            return Track.Length==0.f && Track.MainLayer==ERetakeHitRigLayer::None && Track.AdditiveLayer==ERetakeHitRigLayer::None
                && Track.AimingLayer==ERetakeHitRigLayer::None && Track.MainPoses.IsEmpty() && Track.AdditivePoses.IsEmpty()
                && Track.AimingPoses.IsEmpty() && Track.GunWeight.IsEmpty() && Track.GunInHand.IsEmpty();
        if ((Track.SourceAnimation->GetClass()!=UAnimSequence::StaticClass() && Track.SourceAnimation->GetClass()!=UAnimMontage::StaticClass())
            || !OwnedResource(Owner,Track.SourceAnimation) || !OwnedResource(Owner,Track.SourceAnimation->GetSkeleton())
            || !SkeletonMatches(*Track.SourceAnimation,Body) || Track.Length<=0.f
            || !FMath::IsNearlyEqual(Track.Length,Track.SourceAnimation->GetPlayLength(),.0001f)) return false;
        const int32 Samples=FMath::Max(2,FMath::CeilToInt(Track.Length*Track.SampleRate)+1);
        const int64 Count=int64(Samples)*Body.Bones.Num();
        const auto Layer=[&](ERetakeHitRigLayer Kind,const TArray<FTransform3f>& Poses)
        {
            if (uint8(Kind)>uint8(ERetakeHitRigLayer::MeshAdditive)) return false;
            if (Kind==ERetakeHitRigLayer::None) return Poses.IsEmpty();
            Total+=Poses.Num();if (Poses.Num()!=Count || Total>RetakeWeaponPoses::MaximumTransforms) return false;
            for (const auto& Pose:Poses) if (!Pose.IsValid()) return false;
            return true;
        };
        if (!Track.IsSet() || !Layer(Track.MainLayer,Track.MainPoses) || !Layer(Track.AdditiveLayer,Track.AdditivePoses)
            || !Layer(Track.AimingLayer,Track.AimingPoses) || Track.GunInHand.Num()!=Track.GunWeight.Num()) return false;
        if (!Track.GunInHand.IsEmpty() && Track.GunInHand.Num()!=FMath::Max(2,FMath::CeilToInt(Track.Length*Track.GunSampleRate)+1)) return false;
        Total+=Track.GunInHand.Num();if (Total>RetakeWeaponPoses::MaximumTransforms) return false;
        for (int32 I=0;I<Track.GunWeight.Num();++I)
            if (!Track.GunInHand[I].IsValid() || !FMath::IsFinite(Track.GunWeight[I]) || Track.GunWeight[I]<0.f || Track.GunWeight[I]>1.f) return false;
        return true;
    }
}

bool FRetakeHitRigArmClearance::IsValid() const
{
    if (Keys.Num()<2 || Rotations.Num()!=Keys.Num()*6 || Keys[0]!=0.f || Keys.Last()!=1.f) return false;
    for (int32 I=0;I<Keys.Num();++I)
        if (!FMath::IsFinite(Keys[I]) || (I && Keys[I]<=Keys[I-1])) return false;
    for (const auto& Rotation:Rotations) if (Rotation.ContainsNaN() || !Rotation.IsNormalized()) return false;
    return true;
}

void FRetakeWeaponPoseContract::GetLowerBodyBones(TArray<int32>& Out) const
{
    Out.Reset();TArray<bool> Upper;Upper.SetNumZeroed(Bones.Num());
    for (int32 I=0;I<Bones.Num();++I)
    {
        const int32 Parent=Bones[I].Parent;
        Upper[I]=Bones[I].Name==UpperBodyRoot || (Parent>=0 && Parent<I && Upper[Parent]);
        if (!Upper[I]) Out.Add(I);
    }
}
bool FRetakeWeaponPoseContract::Validate(FString& Error) const
{
    Error.Reset();
    if (FormatVersion!=RetakeWeaponPoses::FormatVersion || RecipeVersion!=RetakeWeaponPoses::RecipeVersion
        || Bones.IsEmpty() || Bones.Num()>RetakeWeaponPoses::MaximumBones || UpperBodyRoot.IsNone() || Bones[0].Name==UpperBodyRoot)
    { Error=TEXT("Unsupported weapon-pose format, recipe or body topology.");return false; }
    TSet<FName> Names;
    for (int32 I=0;I<Bones.Num();++I)
    {
        const auto& Bone=Bones[I];
        if (Bone.Name.IsNone() || Names.Contains(Bone.Name) || !Bone.Reference().IsValid()
            || (I==0 ? Bone.Parent!=INDEX_NONE : Bone.Parent<0 || Bone.Parent>=I))
        { Error=TEXT("Weapon-pose body contract requires a finite, closed bone hierarchy.");return false; }
        Names.Add(Bone.Name);
    }
    if (AnimationBones.Num()!=Bones.Num())
    { Error=TEXT("Weapon-pose contract requires the separate animation-skeleton reference; regenerate the authoring basis.");return false; }
    for (int32 I=0;I<Bones.Num();++I)
        if (AnimationBones[I].Name!=Bones[I].Name || AnimationBones[I].Parent!=Bones[I].Parent || !AnimationBones[I].Reference().IsValid())
        { Error=TEXT("Animation-skeleton reference must preserve the closed mesh topology and contain finite transforms.");return false; }
    TArray<int32> Lower;GetLowerBodyBones(Lower);
    if (!Names.Contains(UpperBodyRoot) || LowerBodyPoses.Num()!=int32(ERetakeHitRigPose::Count)*Lower.Num())
    { Error=TEXT("Weapon-pose body contract has no complete shared lower-body basis.");return false; }
    for (const auto& Pose:LowerBodyPoses) if (!Pose.Get().IsValid())
    { Error=TEXT("Weapon-pose lower-body basis contains an invalid transform.");return false; }
    return true;
}
bool FRetakeWeaponPoseContract::SetAnimationSkeleton(const USkeleton& Skeleton,FString& Error)
{
    Error.Reset();
    if (Bones.IsEmpty() || Bones.Num()>RetakeWeaponPoses::MaximumBones)
    { Error=TEXT("Assign the closed mesh catalogue before capturing its animation skeleton.");return false; }
    const auto& Ref=Skeleton.GetReferenceSkeleton();TArray<int32> Indices;TSet<FName> Names;
    TArray<FRetakePoseBone> Captured;Captured.Reserve(Bones.Num());
    for (int32 I=0;I<Bones.Num();++I)
    {
        const auto& Bone=Bones[I];const int32 Index=Ref.FindBoneIndex(Bone.Name);
        if (Bone.Name.IsNone() || Names.Contains(Bone.Name) || (I==0 ? Bone.Parent!=INDEX_NONE : Bone.Parent<0 || Bone.Parent>=I)
            || Index==INDEX_NONE || Ref.GetParentIndex(Index)!=(Bone.Parent==INDEX_NONE ? INDEX_NONE:Indices[Bone.Parent])
            || !Ref.GetRefBonePose()[Index].IsValid())
        { Error=TEXT("Animation skeleton does not preserve the closed mesh topology at bone: ")+Bone.Name.ToString();return false; }
        auto& Value=Captured.Add_GetRef(Bone);const auto& Transform=Ref.GetRefBonePose()[Index];
        Value.ReferencePosition=FRetakePoseVector(Transform.GetTranslation());Value.ReferenceRotation=FRetakePoseQuaternion(Transform.GetRotation());
        Value.ReferenceScale=FRetakePoseVector(Transform.GetScale3D());Indices.Add(Index);Names.Add(Bone.Name);
    }
    AnimationBones=MoveTemp(Captured);return true;
}
bool FRetakeWeaponPoseContract::MatchesAnimationSkeleton(const USkeleton& Skeleton,FString& Error) const
{
    if (!Validate(Error)) return false;
    const auto& Ref=Skeleton.GetReferenceSkeleton();TArray<int32> Indices;
    for (const auto& Bone:AnimationBones)
    {
        const int32 Index=Ref.FindBoneIndex(Bone.Name);
        if (Index==INDEX_NONE || Ref.GetParentIndex(Index)!=(Bone.Parent==INDEX_NONE ? INDEX_NONE:Indices[Bone.Parent])
            || !Ref.GetRefBonePose()[Index].Equals(Bone.Reference(),.00001))
        { Error=TEXT("Animation skeleton differs from its prepared reference at bone: ")+Bone.Name.ToString();return false; }
        Indices.Add(Index);
    }
    return true;
}
FString FRetakeWeaponPoseContract::ComputeIdentity() const
{
    FString Error;if (!Validate(Error)) return {};
    FIdentity Out;Out.Text(TEXT("RetakeWeaponPoseContract"));Out.Number(FormatVersion);Out.Number(RecipeVersion);
    Out.Number(uint8(ERetakeHitRigPose::Count));Out.Number(uint8(ERetakeHitRigTrack::Count));Out.Text(UpperBodyRoot.ToString());
    Out.Number(int32(Bones.Num()));
    for (const auto& Bone:Bones) { Out.Text(Bone.Name.ToString());Out.Number(Bone.Parent);Out.Transform(Bone.Reference()); }
    Out.Number(int32(AnimationBones.Num()));
    for (const auto& Bone:AnimationBones) { Out.Text(Bone.Name.ToString());Out.Number(Bone.Parent);Out.Transform(Bone.Reference()); }
    Out.Number(int32(LowerBodyPoses.Num()));for (const auto& Pose:LowerBodyPoses) Out.Transform(Pose.Get());
    return Out.Finish();
}
FString URetakeWeaponPoseContribution::ComputeContentHash() const
{
    FIdentity Out;Out.Text(TEXT("RetakeWeaponPoseContribution:1"));Out.Text(Owner);Out.Text(BodyContract.ComputeIdentity());Out.Text(SourceSignature);
    TArray<const FRetakeHitRigHold*> Sorted;for (const auto& Hold:Holds) Sorted.Add(&Hold);
    Sorted.Sort([](const auto& A,const auto& B){return A.ProfileId.ToString().ToLower()<B.ProfileId.ToString().ToLower();});
    Out.Number(int32(Sorted.Num()));for (const auto* Hold:Sorted) Out.Hold(*Hold);
    return Out.Finish();
}
bool URetakeWeaponPoseContribution::Validate(const FString& ExpectedOwner,const FRetakeWeaponPoseContract& ExpectedBody,FString& Error) const
{
    if (!URetakeWeaponPack::IsOwnedObjectPath(ExpectedOwner,FSoftObjectPath(this)))
    {Error=TEXT("Weapon pose contribution asset is not owned by its declared pack.");return false;}
    return ValidatePayload(ExpectedOwner,ExpectedBody,Error);
}
bool URetakeWeaponPoseContribution::ValidatePayload(const FString& ExpectedOwner,const FRetakeWeaponPoseContract& ExpectedBody,FString& Error) const
{
    Error.Reset();
    if (GetClass()!=StaticClass() || Owner!=ExpectedOwner || !FRetakeModManifest::IsValidModId(Owner)
        || !Digest(SourceSignature))
    { Error=TEXT("Weapon pose contribution requires native, pack-owned data and a full source digest.");return false; }
    if (!ExpectedBody.Validate(Error) || !BodyContract.Validate(Error)) return false;
    if (BodyContract.ComputeIdentity()!=ExpectedBody.ComputeIdentity())
    { Error=TEXT("Weapon pose contribution was prepared for another body contract; rebuild it with the qualified kit.");return false; }
    if (Holds.IsEmpty() || Holds.Num()>RetakeWeaponPoses::MaximumHolds)
    { Error=TEXT("Weapon pose contribution requires 1-255 prepared holds.");return false; }
    TSet<FName> Profiles;TArray<int32> Lower;BodyContract.GetLowerBodyBones(Lower);int64 Total=0;
    for (const auto& Hold:Holds)
    {
        const auto Fail=[&](const TCHAR* Detail){Error=FString::Printf(TEXT("Weapon pose %s: %s"),*Hold.ProfileId.ToString(),Detail);return false;};
        FString Pack,Local;
        if (!Hold.ProfileId.ToString().Split(TEXT("."),&Pack,&Local) || !Pack.Equals(Owner,ESearchCase::IgnoreCase)
            || !URetakeWeaponPack::IsValidLocalId(Local) || Profiles.Contains(Hold.ProfileId)) return Fail(TEXT("invalid or duplicate pack-qualified profile ID"));
        Profiles.Add(Hold.ProfileId);
        if (Hold.Poses.Num()!=int32(ERetakeHitRigPose::Count)*BodyContract.Bones.Num() || Hold.Tracks.Num()!=int32(ERetakeHitRigTrack::Count)
            || Hold.AdditionalBashTracks.Num()>7 || Hold.CarrySequences.Num()!=4) return Fail(TEXT("incomplete pose/action/carry layout"));
        Total+=Hold.Poses.Num();if (Total>RetakeWeaponPoses::MaximumTransforms) return Fail(TEXT("prepared pose budget exceeded"));
        for (const auto& Pose:Hold.Poses) if (!Pose.IsValid()) return Fail(TEXT("invalid hold transform"));
        for (int32 Pose=0;Pose<int32(ERetakeHitRigPose::Count);++Pose)
            for (int32 I=0;I<Lower.Num();++I)
                if (!Hold.Poses[Pose*BodyContract.Bones.Num()+Lower[I]].Equals(BodyContract.LowerBodyPoses[Pose*Lower.Num()+I].Get(),.00001))
                    return Fail(TEXT("weapon contribution changes the shared lower-body basis"));
        for (const UAnimSequence* Clip:Hold.CarrySequences)
            if (!Clip || Clip->GetClass()!=UAnimSequence::StaticClass() || !OwnedResource(Owner,Clip)
                || !OwnedResource(Owner,Clip->GetSkeleton()) || !SkeletonMatches(*Clip,BodyContract) || !Clip->Notifies.IsEmpty())
                return Fail(TEXT("unsupported, foreign or notifying carry sequence"));
        for (const auto& Track:Hold.Tracks) if (!ValidateTrack(Track,Owner,BodyContract,Total)) return Fail(TEXT("malformed, incompatible or foreign action track"));
        for (const auto& Track:Hold.AdditionalBashTracks)
            if (!Track.IsSet() || !ValidateTrack(Track,Owner,BodyContract,Total)) return Fail(TEXT("malformed alternate bash track"));
        for (const auto* Clearance:{&Hold.ClearanceStand,&Hold.ClearanceCrouch})
        {
            if (Clearance->Keys.Num()>1024 || ((Hold.bClearance || !Clearance->Keys.IsEmpty() || !Clearance->Rotations.IsEmpty()) && !Clearance->IsValid()))
                return Fail(TEXT("invalid arm clearance"));
            Total+=Clearance->Rotations.Num();
        }
        if (Total>RetakeWeaponPoses::MaximumTransforms) return Fail(TEXT("prepared pose budget exceeded"));
        if (!Hold.SupportInHand.IsValid() || !Hold.GripInHand.IsValid() || Hold.BarrelInHand.ContainsNaN()
            || !FMath::IsFinite(Hold.ConvergenceDegrees) || Hold.ConvergenceDegrees<0.f || Hold.ConvergenceDegrees>180.f
            || (Hold.bConvergence && !Hold.BarrelInHand.IsNormalized()) || !FMath::IsFinite(Hold.LeanElbowDownWeight)
            || Hold.LeanElbowDownWeight<0.f || Hold.LeanElbowDownWeight>1.f) return Fail(TEXT("invalid support/convergence settings"));
        const auto& Aim=Hold.Aim;
        for (float Value:{Aim.Spine01,Aim.Spine02,Aim.Spine03,Aim.Spine04,Aim.Spine05,Aim.Arms,Aim.Neck,Aim.Head})
            if (!FMath::IsFinite(Value) || Value<0.f || Value>1.f) return Fail(TEXT("invalid aim shares"));
        if (!FMath::IsFinite(Aim.MaxPitchUpDegrees) || Aim.MaxPitchUpDegrees<0.f || Aim.MaxPitchUpDegrees>89.f
            || !FMath::IsFinite(Aim.MaxPitchDownDegrees) || Aim.MaxPitchDownDegrees<0.f || Aim.MaxPitchDownDegrees>89.f
            || !FMath::IsFinite(Aim.MaxYawDegrees) || Aim.MaxYawDegrees<0.f || Aim.MaxYawDegrees>180.f) return Fail(TEXT("invalid aim range"));
    }
    if (!Digest(ContentHash) || ContentHash!=ComputeContentHash())
    { Error=TEXT("Weapon pose contribution content digest is missing or stale; prepare and save it again.");return false; }
    return true;
}
FString RetakeWeaponPoses::HoldIdentity(const FRetakeHitRigHold& Hold)
{
    FIdentity Out;Out.Text(TEXT("RetakeWeaponHold:1"));Out.Hold(Hold);return Out.Finish();
}
const FRetakeHitRigHold* URetakeWeaponPoseContribution::FindHold(FName ProfileId) const
{ return Holds.FindByPredicate([&](const auto& Hold){return Hold.ProfileId==ProfileId;}); }

int64 URetakeWeaponPoseContribution::PreparedTransformCount() const
{
    int64 Count=0;
    const auto TrackCount=[](const FRetakeHitRigTrack& Track)
    {return int64(Track.MainPoses.Num())+Track.AdditivePoses.Num()+Track.AimingPoses.Num()+Track.GunInHand.Num();};
    for (const auto& Hold:Holds)
    {
        Count+=int64(Hold.Poses.Num())+Hold.ClearanceStand.Rotations.Num()+Hold.ClearanceCrouch.Rotations.Num();
        for (const auto& Track:Hold.Tracks) Count+=TrackCount(Track);
        for (const auto& Track:Hold.AdditionalBashTracks) Count+=TrackCount(Track);
    }
    return Count;
}

bool URetakeWeaponPoseContribution::ValidateSpecificationBindings(const URetakeWeaponSpecification& Specification,FString& Error) const
{
    Error.Reset();
    const auto& P=Specification.Presentation;
    const auto* Profile=P.CharacterProfile.Get();
    if (!Profile || Profile->GetClass()!=URetakeWeaponAnimationProfile::StaticClass()
        || !URetakeWeaponPack::IsOwnedObjectPath(Owner,FSoftObjectPath(Profile)))
    {Error=TEXT("The selected weapon needs a loaded native animation profile owned by its pack.");return false;}
    const auto* Hold=FindHold(Profile->ProfileId);
    if (!Hold || Hold->CarrySequences.Num()!=4 || Hold->Tracks.Num()!=int32(ERetakeHitRigTrack::Count))
    {Error=TEXT("Prepared contribution has no complete hold for ")+Profile->ProfileId.ToString();return false;}
    if (Hold->bClearance!=P.Clearance.bEnabled || Hold->bContinuousReloadStages!=Profile->bContinuousThirdPersonReloadStages
        || !FRetakeThirdPersonAimPose::StaticStruct()->CompareScriptStruct(&Hold->Aim,&Profile->ThirdPersonAim,0))
    {Error=TEXT("Prepared pose settings differ from the selected weapon/profile: ")+Specification.WeaponId.ToString();return false;}
    const auto& A=Profile->ThirdPersonAnims;
    const UAnimSequence* Carry[]={A.IdleAnim.Get(),A.CrouchIdleAnim ? A.CrouchIdleAnim.Get():A.IdleAnim.Get(),
        A.AimIdleAnim ? A.AimIdleAnim.Get():A.IdleAnim.Get(),A.SprintAnim ? A.SprintAnim.Get():A.IdleAnim.Get()};
    for (int32 I=0;I<4;++I) if (Hold->CarrySequences[I]!=Carry[I])
    {Error=TEXT("Prepared carry source differs from the selected profile: ")+Profile->ProfileId.ToString();return false;}
    const auto Check=[&](ERetakeHitRigTrack Kind,const FSoftObjectPath& Expected)
    {
        if (FSoftObjectPath(Hold->Tracks[int32(Kind)].SourceAnimation)!=Expected)
        {Error=FString::Printf(TEXT("Prepared action %d differs from selected weapon %s."),int32(Kind),*Specification.WeaponId.ToString());return false;}
        return true;
    };
    const auto& M=Profile->ThirdPersonMontages;
    if (!Check(ERetakeHitRigTrack::Reload,FSoftObjectPath(M.ReloadMontage))
        || !Check(ERetakeHitRigTrack::Equip,FSoftObjectPath(M.EquipMontage))
        || !Check(ERetakeHitRigTrack::Holster,FSoftObjectPath(M.HolsterMontage))
        || !Check(ERetakeHitRigTrack::Drop,FSoftObjectPath(M.DropMontage))
        || !Check(ERetakeHitRigTrack::Inspect,FSoftObjectPath(M.InspectMontage))
        || !Check(ERetakeHitRigTrack::BoltCycle,FSoftObjectPath(Profile->ThirdPersonBoltCycle))) return false;
    if (P.BashVariants.Num()>8 || Hold->AdditionalBashTracks.Num()!=FMath::Max(0,P.BashVariants.Num()-1))
    {Error=TEXT("Prepared bash variants differ from the selected weapon.");return false;}
    if (!Check(ERetakeHitRigTrack::Bash,(P.BashVariants.IsEmpty() ? P.ThirdPersonBashAnimation:P.BashVariants[0].ThirdPersonAnimation).ToSoftObjectPath())) return false;
    for (int32 I=1;I<P.BashVariants.Num();++I)
        if (FSoftObjectPath(Hold->AdditionalBashTracks[I-1].SourceAnimation)!=P.BashVariants[I].ThirdPersonAnimation.ToSoftObjectPath())
        {Error=TEXT("Prepared alternate bash source differs from the selected weapon.");return false;}
    return Check(ERetakeHitRigTrack::ReloadStart,P.ThirdPersonReloadStart.ToSoftObjectPath())
        && Check(ERetakeHitRigTrack::ReloadInsert,P.ThirdPersonReloadInsert.ToSoftObjectPath())
        && Check(ERetakeHitRigTrack::ReloadEnd,P.ThirdPersonReloadEnd.ToSoftObjectPath())
        && Check(ERetakeHitRigTrack::GrenadePrepareLong,P.ThirdPersonGrenade.PrepareLong.ToSoftObjectPath())
        && Check(ERetakeHitRigTrack::GrenadePrepareShort,P.ThirdPersonGrenade.PrepareShort.ToSoftObjectPath())
        && Check(ERetakeHitRigTrack::GrenadeReleaseLong,P.ThirdPersonGrenade.ReleaseLong.ToSoftObjectPath())
        && Check(ERetakeHitRigTrack::GrenadeReleaseShort,P.ThirdPersonGrenade.ReleaseShort.ToSoftObjectPath());
}
#if WITH_EDITOR
EDataValidationResult URetakeWeaponPoseContribution::IsDataValid(FDataValidationContext& Context) const
{
    FString Error;if (!Validate(Owner,BodyContract,Error))
    { Context.AddError(FText::FromString(Error));return EDataValidationResult::Invalid; }
    return EDataValidationResult::Valid;
}
#endif
