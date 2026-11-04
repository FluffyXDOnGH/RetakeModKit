// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeWeaponPoseBaking.h"
#if WITH_EDITOR
#include "RetakeWeaponSpecification.h"
#include "RetakeWeaponPack.h"
#include "RetakeMapSDKManifest.h"
#include "RetakeMapSDKVersion.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Engine/SkeletalMesh.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "Hash/Blake3.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/MetaData.h"
#include "UObject/StrongObjectPtr.h"

struct FRetakeWeaponPoseBuildAccess
{
    static bool Validate(const URetakeWeaponPoseContribution& Value,const FString& Owner,
        const FRetakeWeaponPoseContract& Body,FString& Error)
    { return Value.ValidatePayload(Owner,Body,Error); }
};

namespace RetakeWeaponPoseBaking
{
bool SourceSignature(const FString& Owner,const FInputs& Inputs,TConstArrayView<const URetakeWeaponSpecification*> Definitions,FString& Out,FString& Error)
{
    check(IsInGameThread());Error.Reset();
    if (!Inputs.Body.Validate(Error)) return false;
    if (!FRetakeModManifest::IsValidModId(Owner) || Definitions.IsEmpty() || Definitions.Num()>512 || !FMath::IsFinite(Inputs.LeanStrength))
    {Error=TEXT("Pose source signature requires a bounded specification set and finite basis settings.");return false;}
    FBlake3 Hash;
    const auto Text=[&](FString Value)
    {
        Value=Value.ToLower();const FTCHARToUTF8 Utf8(*Value);const int32 Size=Utf8.Length();
        Hash.Update(&Size,sizeof(Size));Hash.Update(Utf8.Get(),Size);
    };
    Text(TEXT("RetakeWeaponPoseSources:1"));Text(Owner);Text(Inputs.Body.ComputeIdentity());Text(Inputs.SupportSocket.ToString());
    const int32 Version=RetakeMapSDK::ContractVersion;Hash.Update(&Version,sizeof(Version));
    const uint32 Preparation=RetakeWeaponPoses::PreparationVersion;Hash.Update(&Preparation,sizeof(Preparation));
    Hash.Update(&Inputs.LeanStrength,sizeof(Inputs.LeanStrength));
    TSet<FName> Packages;
    // Ordered binding slots distinguish swapped authoring inputs even when their package set is unchanged.
    const UObject* Basis[]={Inputs.ReferenceMesh,Inputs.StandLower,Inputs.CrouchLower,Inputs.StandLean,Inputs.CrouchLean,
        Inputs.StandMovingLean,Inputs.CrouchMovingLean,Inputs.ClearanceStand,Inputs.ClearanceCrouch};
    for (const UObject* Object:Basis)
    {
        Text(Object ? Object->GetPathName() : FString());
        if (Object) Packages.Add(Object->GetOutermost()->GetFName());
    }
    // The lean generator records its full source as editor metadata, not an asset-registry soft dependency.
    for (const UBlendSpace* Space:{Inputs.StandLean,Inputs.CrouchLean,Inputs.StandMovingLean,Inputs.CrouchMovingLean})
        if (Space) for (const auto& Sample:Space->GetBlendSamples()) if (Sample.Animation)
        {
            const FString Source=Sample.Animation->GetOutermost()->GetMetaData().GetValue(Sample.Animation,TEXT("Retake.FullPoseSource"));
            if (!Source.IsEmpty())
            {
                const FSoftObjectPath Path(Source);
                if (!Path.IsValid()) {Error=TEXT("Invalid full-pose source metadata: ")+Source;return false;}
                Packages.Add(FName(*Path.GetLongPackageName()));
            }
        }
    TArray<FString> SpecificationPaths;
    for (const auto* Definition:Definitions)
    {
        if (!Definition) {Error=TEXT("Null weapon specification in preparation request.");return false;}
        SpecificationPaths.Add(Definition->GetPathName().ToLower());Packages.Add(Definition->GetOutermost()->GetFName());
        TArray<FSoftObjectPath> References;Definition->GetAllReferencedSoftPaths(References);
        for (const auto& Reference:References) Packages.Add(FName(*Reference.GetLongPackageName()));
    }
    SpecificationPaths.Sort();for (const auto& Path:SpecificationPaths) Text(Path);
    auto& Registry=FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    TArray<FName> Queue=Packages.Array();
    for (int32 I=0;I<Queue.Num();++I)
    {
        if (Queue.Num()>20000) {Error=TEXT("Weapon preparation source closure exceeds its bounded package budget.");return false;}
        const FString Name=Queue[I].ToString();
        if (Name.StartsWith(TEXT("/Script/")))
        {
            if (Name.StartsWith(TEXT("/Script/Retake"),ESearchCase::IgnoreCase)
                && !Name.Equals(TEXT("/Script/RetakeMapSDK"),ESearchCase::IgnoreCase)
                && !Name.Equals(TEXT("/Script/RetakeMapSDKEditor"),ESearchCase::IgnoreCase)
                && !Name.Equals(TEXT("/Script/RetakeDestruction"),ESearchCase::IgnoreCase)
                && !Name.Equals(TEXT("/Script/RetakeDestructionEditor"),ESearchCase::IgnoreCase))
            {Error=TEXT("Weapon preparation references the private game module: ")+Name;return false;}
            continue;
        }
        bool Allowed=Name.StartsWith(TEXT("/")+Owner+TEXT("/"),ESearchCase::IgnoreCase);
        for (const TCHAR* Root:RetakeMapSDK::AllowedForeignRoots) Allowed|=Name.StartsWith(Root,ESearchCase::IgnoreCase);
        if (!Allowed) {Error=TEXT("Weapon preparation references private or undeclared source content: ")+Name;return false;}
        TArray<FName> Dependencies;
        if (!Registry.GetDependencies(Queue[I],Dependencies,UE::AssetRegistry::EDependencyCategory::Package))
        {Error=TEXT("Pose source package is not indexed; finish asset discovery before preparing: ")+Name;return false;}
        for (FName Dependency:Dependencies) if (!Packages.Contains(Dependency)) {Packages.Add(Dependency);Queue.Add(Dependency);}
    }
    Queue.Sort([](FName A,FName B){return A.ToString().ToLower()<B.ToString().ToLower();});
    TArray<uint8> Buffer;Buffer.SetNumUninitialized(256*1024);
    for (FName Package:Queue)
    {
        const FString Name=Package.ToString();
        if (Name.StartsWith(TEXT("/Script/"))) continue; // Native SDK/engine code is pinned by the creator build/qualification record.
        if (const UPackage* Loaded=FindPackage(nullptr,*Name);Loaded && Loaded->IsDirty())
        {Error=TEXT("Save pose preparation input before baking: ")+Name;return false;}
        FString File;
        if (!FPackageName::DoesPackageExist(Name,&File)) {Error=TEXT("Missing pose preparation input: ")+Name;return false;}
        TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*File));
        if (!Reader) {Error=TEXT("Cannot read pose preparation input: ")+Name;return false;}
        Text(Name);const int64 Size=Reader->TotalSize();if (Size<0) {Error=TEXT("Invalid pose input file size: ")+Name;return false;}
        Hash.Update(&Size,sizeof(Size));
        for (int64 Remaining=Size;Remaining>0;)
        {
            const int64 Bytes=FMath::Min<int64>(Remaining,Buffer.Num());Reader->Serialize(Buffer.GetData(),Bytes);
            if (Reader->IsError()) {Error=TEXT("Pose preparation input changed or failed while reading: ")+Name;return false;}
            Hash.Update(Buffer.GetData(),Bytes);Remaining-=Bytes;
        }
    }
    Out=LexToString(Hash.Finalize()).ToLower();return true;
}

bool BakeContribution(const FString& Owner,const FInputs& Inputs,TConstArrayView<const URetakeWeaponSpecification*> Definitions,
    URetakeWeaponPoseContribution& Out,FString& Error)
{
    check(IsInGameThread());Error.Reset();
    if (!FRetakeModManifest::IsValidModId(Owner) || Out.GetClass()!=URetakeWeaponPoseContribution::StaticClass()
        || !URetakeWeaponPack::IsOwnedObjectPath(Owner,FSoftObjectPath(&Out)))
    {Error=TEXT("Choose a native contribution asset inside its owning mod.");return false;}
    TSet<FSoftObjectPath> Unique;
    for (const auto* Definition:Definitions)
    {
        if (!Definition || Definition->GetClass()!=URetakeWeaponSpecification::StaticClass()
            || !URetakeWeaponPack::IsOwnedObjectPath(Owner,FSoftObjectPath(Definition)) || Unique.Contains(FSoftObjectPath(Definition)))
        {Error=TEXT("Contribution inputs must be distinct native specifications owned by the mod.");return false;}
        FString Pack,Local;
        if (!Definition->WeaponId.ToString().Split(TEXT("."),&Pack,&Local) || !Pack.Equals(Owner,ESearchCase::IgnoreCase)
            || !URetakeWeaponPack::IsValidLocalId(Local))
        {Error=TEXT("Weapon preparation requires pack-qualified weapon identities.");return false;}
        Unique.Add(FSoftObjectPath(Definition));
    }
    FString Before;if (!SourceSignature(Owner,Inputs,Definitions,Before,Error)) return false;
    TStrongObjectPtr<URetakeWeaponPoseContribution> Candidate(NewObject<URetakeWeaponPoseContribution>());
    Candidate->Owner=Owner;Candidate->BodyContract=Inputs.Body;Candidate->SourceSignature=Before;
    TMap<FName,FString> Identities;int64 Transforms=0;
    for (const auto* Definition:Definitions)
    {
        FRetakeHitRigHold Hold;TSet<FName> Sources;
        if (!BakeWeapon(*Definition,Inputs,Hold,Sources,Error)) return false;
        const FString Identity=RetakeWeaponPoses::HoldIdentity(Hold);
        if (const FString* Existing=Identities.Find(Hold.ProfileId))
        {
            if (*Existing!=Identity)
            {Error=TEXT("Different weapon pose data uses the same profile ID; assign distinct profiles: ")+Hold.ProfileId.ToString();return false;}
            continue;
        }
        Transforms+=Hold.Poses.Num()+int64(Hold.ClearanceStand.Rotations.Num())+Hold.ClearanceCrouch.Rotations.Num();
        const auto Count=[&](const FRetakeHitRigTrack& Track)
        {Transforms+=int64(Track.MainPoses.Num())+Track.AdditivePoses.Num()+Track.AimingPoses.Num()+Track.GunInHand.Num();};
        for (const auto& Track:Hold.Tracks) Count(Track);
        for (const auto& Track:Hold.AdditionalBashTracks) Count(Track);
        if (Transforms>RetakeWeaponPoses::MaximumTransforms || Candidate->Holds.Num()>=RetakeWeaponPoses::MaximumHolds)
        {Error=TEXT("Selected weapons exceed the contribution hold/sample budget.");return false;}
        Identities.Add(Hold.ProfileId,Identity);Candidate->Holds.Add(MoveTemp(Hold));
    }
    Candidate->Holds.Sort([](const auto& A,const auto& B){return A.ProfileId.ToString().ToLower()<B.ProfileId.ToString().ToLower();});
    Candidate->ContentHash=Candidate->ComputeContentHash();
    if (!FRetakeWeaponPoseBuildAccess::Validate(*Candidate,Owner,Inputs.Body,Error)) return false;
    for (const auto* Definition:Definitions) if (!Candidate->ValidateSpecificationBindings(*Definition,Error)) return false;
    FString After;if (!SourceSignature(Owner,Inputs,Definitions,After,Error)) return false;
    if (After!=Before) {Error=TEXT("Pose preparation inputs changed while baking; save them and prepare again.");return false;}
    // No asset fields are touched before complete payload/source verification. Editor callers own transaction/save UI.
    Out.Owner=Candidate->Owner;Out.BodyContract=MoveTemp(Candidate->BodyContract);Out.SourceSignature=MoveTemp(Candidate->SourceSignature);
    Out.Holds=MoveTemp(Candidate->Holds);Out.ContentHash=MoveTemp(Candidate->ContentHash);Out.MarkPackageDirty();return true;
}
}
#endif
