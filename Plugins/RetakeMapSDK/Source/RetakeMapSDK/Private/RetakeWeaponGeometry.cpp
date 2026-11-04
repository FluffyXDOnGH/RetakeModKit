// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeWeaponGeometry.h"
#include "RetakeWeaponValidation.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "AnimationRuntime.h"

bool RetakeWeaponGeometry::SocketTransform(const USkeletalMesh& Mesh,FName Name,FTransform& Out)
{
    const USkeletalMeshSocket* Socket=Mesh.FindSocket(Name);
    const int32 Bone=Mesh.GetRefSkeleton().FindBoneIndex(Socket ? Socket->BoneName:Name);
    if (Bone==INDEX_NONE) return false;
    Out=(Socket ? Socket->GetSocketLocalTransform():FTransform::Identity)
        *FAnimationRuntime::GetComponentSpaceTransformRefPose(Mesh.GetRefSkeleton(),Bone);
    return true;
}

bool RetakeWeaponGeometry::ResolveAimAxis(const USkeletalMesh* WeaponMesh,const FRetakeWeaponAimPoint& WeaponAimPoint,const TArray<FRetakeWeaponAttachment>& Attachments,FTransform& OutAimInWeapon,bool bLoadMeshes)
{
    FRetakeWeaponAimPoint Point; FName Slot;
    if (!WeaponMesh || !RetakeWeaponValidation::SelectAimPointProvider(WeaponAimPoint,Attachments,Point,Slot)) return false;
    const auto WeaponSocket=[WeaponMesh](FName Name,FTransform& Out)
    { return SocketTransform(*WeaponMesh,Name,Out); };
    FTransform SocketInWeapon;
    if (Slot.IsNone()) { if (!WeaponSocket(Point.Socket,SocketInWeapon)) return false; }
    else
    {
        struct FParent { UStaticMesh* Mesh=nullptr; FTransform World; };
        TMap<FName,FParent> Parents; bool bFound=false;
        for (const auto& Part:Attachments)
        {
            UStaticMesh* Mesh=Part.Mesh.Get();
            if (!Mesh && bLoadMeshes) Mesh=Part.Mesh.LoadSynchronous();
            if (!Mesh) return false;
            FTransform SocketWorld;
            if (Part.ParentSlot.IsNone()) { if (!WeaponSocket(Part.Socket,SocketWorld)) return false; }
            else
            {
                const auto* Parent=Parents.Find(Part.ParentSlot);
                const auto* Socket=Parent && Parent->Mesh ? Parent->Mesh->FindSocket(Part.Socket):nullptr;
                if (!Socket) return false;
                SocketWorld=FTransform(Socket->RelativeRotation,Socket->RelativeLocation,Socket->RelativeScale)*Parent->World;
            }
            FParent Entry; Entry.Mesh=Mesh; Entry.World=Part.Offset*SocketWorld; Parents.Add(Part.Slot,Entry);
            if (Part.Slot!=Slot) continue;
            const auto* Socket=Mesh->FindSocket(Point.Socket);
            if (!Socket) return false;
            SocketInWeapon=FTransform(Socket->RelativeRotation,Socket->RelativeLocation)*Entry.World; bFound=true; break;
        }
        if (!bFound) return false;
    }
    OutAimInWeapon=FTransform(Point.Offset.GetRotation().GetNormalized(),Point.Offset.GetTranslation())*SocketInWeapon;
    OutAimInWeapon.SetScale3D(FVector::OneVector);
    return !OutAimInWeapon.ContainsNaN();
}

bool RetakeWeaponGeometry::SolveAimRig(const FTransform& SightInArms,float EyeReliefCm,const FVector& ArmsScale,FTransform& OutRig)
{
    if (SightInArms.ContainsNaN() || !FMath::IsFinite(EyeReliefCm) || EyeReliefCm<=0.f || ArmsScale.ContainsNaN() || ArmsScale.GetMin()<=0.f) return false;
    // Arms space to camera space: rotation cancels the sight's rotation so its +X lies along the view; the translation
    // then carries the (scaled) sight origin onto the axis at the eye relief.
    const FQuat Rotation=SightInArms.GetRotation().Inverse().GetNormalized();
    const FVector Translation=FVector(EyeReliefCm,0.f,0.f)-Rotation.RotateVector(ArmsScale*SightInArms.GetTranslation());
    OutRig=FTransform(Rotation,Translation,ArmsScale);
    return !OutRig.ContainsNaN();
}
