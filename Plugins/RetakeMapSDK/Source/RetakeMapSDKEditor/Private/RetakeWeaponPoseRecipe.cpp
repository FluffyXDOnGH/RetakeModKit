// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeWeaponPoseRecipe.h"
#include "AssetRegistry/AssetData.h"
#include "RetakeWeaponPoseBasis.h"
#include "RetakeWeaponPose.h"
#include "RetakeWeaponPoseBaking.h"
#include "RetakeWeaponSpecification.h"
#include "Engine/SkeletalMesh.h"
#include "RetakeWeaponPack.h"
#include "RetakeMapSDKManifest.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

bool RetakeWeaponPosePreparation::ValidateSources(const FString& Owner,const URetakeWeaponPoseContribution& Output,
    FString& Error,TConstArrayView<FSoftObjectPath> RequiredSpecifications,bool bRequireSavedOutput)
{
    Error.Reset();
    if (!IsInGameThread() || !FRetakeModManifest::IsValidModId(Owner)
        || !URetakeWeaponPack::IsOwnedObjectPath(Owner,FSoftObjectPath(&Output)))
    {Error=TEXT("Validate owned weapon preparation on the editor game thread.");return false;}
    if (bRequireSavedOutput && Output.GetOutermost()->IsDirty())
    {Error=TEXT("Save the prepared contribution before validating it for export.");return false;}
    const auto* Recipe=Cast<URetakeWeaponPoseRecipe>(Output.AuthoringRecipe);
    if (!Recipe || Recipe->GetClass()!=URetakeWeaponPoseRecipe::StaticClass() || Recipe->GetOuter()!=&Output)
    {Error=TEXT("Prepared weapon poses have no owned authoring recipe; run prepare-weapons again.");return false;}
    const FSoftObjectPath BasisPath=Recipe->Basis.ToSoftObjectPath();
    if (!URetakeWeaponPack::IsOwnedObjectPath(Owner,BasisPath)
        && !URetakeWeaponPack::IsOwnedObjectPath(TEXT("RetakeMapContent"),BasisPath))
    {Error=TEXT("The authoring recipe references a private or foreign character basis.");return false;}
    TStrongObjectPtr<URetakeWeaponPoseBasis> Basis(Recipe->Basis.LoadSynchronous());
    if (!Basis || Basis->GetClass()!=URetakeWeaponPoseBasis::StaticClass() || Basis->GetOutermost()->IsDirty())
    {Error=TEXT("Load and save the native character basis before verifying weapon preparation.");return false;}
    if (Recipe->Specifications.IsEmpty() || Recipe->Specifications.Num()>512 || RequiredSpecifications.Num()>512)
    {Error=TEXT("The preparation recipe requires 1-512 specifications.");return false;}
    TSet<FSoftObjectPath> Paths;
    TArray<TStrongObjectPtr<URetakeWeaponSpecification>> Retained;
    TArray<const URetakeWeaponSpecification*> Definitions;
    for (const auto& Reference:Recipe->Specifications)
    {
        const FSoftObjectPath Path=Reference.ToSoftObjectPath();
        if (!URetakeWeaponPack::IsOwnedObjectPath(Owner,Path) || Paths.Contains(Path))
        {Error=TEXT("Preparation recipe contains a duplicate, private or foreign specification.");return false;}
        Paths.Add(Path);
        auto* Definition=Reference.LoadSynchronous();
        if (!Definition || Definition->GetClass()!=URetakeWeaponSpecification::StaticClass())
        {Error=TEXT("Missing native preparation specification: ")+Path.ToString();return false;}
        FString Pack,Local;
        if (!Definition->WeaponId.ToString().Split(TEXT("."),&Pack,&Local) || !Pack.Equals(Owner,ESearchCase::IgnoreCase)
            || !URetakeWeaponPack::IsValidLocalId(Local))
        {Error=TEXT("Recipe specification has an invalid owning weapon identity: ")+Path.ToString();return false;}
        Retained.Emplace(Definition);Definitions.Add(Definition);
    }
    for (const auto& Required:RequiredSpecifications) if (!Paths.Contains(Required))
    {Error=TEXT("Selected profile specification was not included in this preparation recipe: ")+Required.ToString();return false;}
    if (!Output.Validate(Owner,Basis->Body,Error)) return false;
    for (const auto* Definition:Definitions)
    {
        const auto& Presentation=Definition->Presentation;
        const FName ThirdPerson=Presentation.ThirdPersonGripSocket.IsNone() ? Presentation.GripSocket : Presentation.ThirdPersonGripSocket;
        const auto HasAttachment=[&](FName Name)
        { return Basis->ReferenceMesh && !Name.IsNone() && (Basis->ReferenceMesh->FindSocket(Name)
            || Basis->ReferenceMesh->GetRefSkeleton().FindBoneIndex(Name)!=INDEX_NONE); };
        if (!HasAttachment(Presentation.GripSocket) || !HasAttachment(ThirdPerson))
        { Error=TEXT("Weapon ")+Definition->WeaponId.ToString()+TEXT(" names a first/third-person grip absent from its prepared character basis. Choose an existing socket or bone."); return false; }
        if (!Definition->Presentation.CharacterProfile.LoadSynchronous())
        {Error=TEXT("The recipe specification's animation profile is missing.");return false;}
        if (!Output.ValidateSpecificationBindings(*Definition,Error)) return false;
    }
    auto& Registry=FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    Registry.SearchAllAssets(true);
    FString Signature;
    if (!RetakeWeaponPoseBaking::SourceSignature(Owner,Basis->GetInputs(),Definitions,Signature,Error)) return false;
    if (Signature!=Output.SourceSignature)
    {Error=TEXT("Weapon preparation is stale; save its changed inputs and run prepare-weapons again.");return false;}
    return true;
}
bool RetakeWeaponPosePreparation::PlanProfiles(const URetakeWeaponPack& Catalogue,TArray<FRetakeWeaponPackProfile>& Out,FString& Error)
{
    Out=Catalogue.Profiles;
    for (auto& Profile:Out)
    {
        if (!URetakeWeaponPack::IsValidLocalId(Profile.ProfileId))
        {Error=TEXT("Choose a valid profile ID before preparing weapons.");return false;}
        if (Profile.PoseContribution.IsNull())
        {
            const FString Name=TEXT("DA_Poses_")+Profile.ProfileId;
            Profile.PoseContribution=FSoftObjectPath(TEXT("/")+Catalogue.PackId+TEXT("/Preparation/")+Name+TEXT(".")+Name);
        }
    }
    TStrongObjectPtr<URetakeWeaponPack> Projected(NewObject<URetakeWeaponPack>());
    Projected->PackId=Catalogue.PackId;Projected->DisplayName=Catalogue.DisplayName;
    Projected->Weapons=Catalogue.Weapons;Projected->Profiles=Out;
    return Projected->Validate(Error);
}

URetakeWeaponPoseBasis* RetakeWeaponPosePreparation::FindBasis(const FString& Owner,const FSoftObjectPath& Explicit,
    const URetakeWeaponPoseContribution* Output,FString& Error)
{
    FSoftObjectPath Path=Explicit;
    if (Path.IsNull() && Output)
        if (const auto* Recipe=Cast<URetakeWeaponPoseRecipe>(Output->AuthoringRecipe))
            Path=Recipe->Basis.ToSoftObjectPath();
    if (Path.IsNull())
    {
        auto& Registry=FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        TArray<FAssetData> Assets;Registry.GetAssetsByPath(FName(*(TEXT("/")+Owner)),Assets,true);
        Assets.RemoveAll([](const FAssetData& Asset){return Asset.AssetClassPath!=URetakeWeaponPoseBasis::StaticClass()->GetClassPathName();});
        if (Assets.Num()!=1)
        {Error=Assets.IsEmpty()?TEXT("Create the weapon animation basis first (weapon-basis)."):
            TEXT("This workspace has multiple authoring bases; select one explicitly with --basis.");return nullptr;}
        Path=Assets[0].GetSoftObjectPath();
    }
    if (!URetakeWeaponPack::IsOwnedObjectPath(Owner,Path) && !URetakeWeaponPack::IsOwnedObjectPath(TEXT("RetakeMapContent"),Path))
    {Error=TEXT("Choose an owned or shared SDK authoring basis.");return nullptr;}
    auto* Basis=Cast<URetakeWeaponPoseBasis>(Path.TryLoad());
    if (!Basis || Basis->GetClass()!=URetakeWeaponPoseBasis::StaticClass() || Basis->GetOutermost()->IsDirty())
    {Error=TEXT("Load and save the selected native authoring basis.");return nullptr;}
    if (!Basis->Body.Validate(Error)) return nullptr;
    if (!Basis->ReferenceMesh) {Error=TEXT("The authoring basis needs its reference mesh.");return nullptr;}
    return Basis;
}

