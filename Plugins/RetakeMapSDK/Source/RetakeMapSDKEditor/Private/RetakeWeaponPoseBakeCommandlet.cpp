// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeWeaponPoseBakeCommandlet.h"
#include "RetakeWeaponPoseBasis.h"
#include "RetakeWeaponPoseRecipe.h"
#include "RetakeWeaponSpecification.h"
#include "RetakeWeaponPack.h"
#include "RetakeMapSDKManifest.h"
#include "RetakeMapSDKVersion.h"
#include "RetakeMapSDKLog.h"
#include "RetakeModWorkspace.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

URetakeWeaponPoseBakeCommandlet::URetakeWeaponPoseBakeCommandlet()
{IsClient=false;IsServer=false;IsEditor=true;LogToConsole=true;}

int32 URetakeWeaponPoseBakeCommandlet::Main(const FString& Params)
{
    const auto Fail=[](const FString& Error)
    {UE_LOGF(LogRetakeMapSDK,Error,"event=WeaponPoseBakeFailed reason=\"%ls\"",*Error);return 1;};
    FString Request,Text,Owner,BasisPath,OutputPath,Selected,Rebuild;
    FParse::Value(*Params,TEXT("Mod="),Selected);
    const bool bRequest=FParse::Value(*Params,TEXT("Request="),Request);
    const bool bRebuild=FParse::Value(*Params,TEXT("Contribution="),Rebuild);
    if(FParse::Param(*Params,TEXT("AllProfiles")))
    {
        const auto Plugin=IPluginManager::Get().FindPlugin(Selected);
        if(bRequest||bRebuild||!FRetakeModManifest::IsValidModId(Selected)||!Plugin||!Plugin->IsEnabled()
            ||RetakeModWorkspace::Kind(Selected)!=TEXT("weapons"))return Fail(TEXT("AllProfiles requires only an enabled weapon mod selection."));
        auto& Registry=FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();Registry.SearchAllAssets(true);
        TArray<FAssetData> Assets;Registry.GetAssetsByPath(FName(*(TEXT("/")+Selected)),Assets,true);
        TStrongObjectPtr<URetakeWeaponPack> Catalogue;
        for(const auto& Asset:Assets)if(Asset.AssetClassPath==URetakeWeaponPack::StaticClass()->GetClassPathName())
        {
            if(Catalogue)return Fail(TEXT("Keep exactly one weapon catalogue in the workspace."));
            Catalogue.Reset(Cast<URetakeWeaponPack>(Asset.GetAsset()));
        }
        FString Error;
        if(!Catalogue||Catalogue->PackId!=Selected||!Catalogue->Validate(Error))return Fail(TEXT("Missing or invalid owned weapon catalogue: ")+Error);
        TMap<FSoftObjectPath,TArray<FSoftObjectPath>> Contributions;
        for(const auto& Profile:Catalogue->Profiles)
        {
            auto& Required=Contributions.FindOrAdd(Profile.PoseContribution);
            for(const auto& Binding:Profile.Weapons)Required.AddUnique(Binding.Value);
        }
        TArray<FSoftObjectPath> Outputs;Contributions.GetKeys(Outputs);
        Outputs.Sort([](const auto& A,const auto& B){return A.ToString().ToLower()<B.ToString().ToLower();});
        for(const auto& Output:Outputs)
        {
            if(Main(FString::Printf(TEXT("-Mod=%ls -Contribution=\"%ls\""),*Selected,*Output.ToString()))!=0)return 1;
            const auto* Data=Cast<URetakeWeaponPoseContribution>(Output.ResolveObject());
            if(!Data||!RetakeWeaponPosePreparation::ValidateSources(Selected,*Data,Error,Contributions.FindChecked(Output)))return Fail(Error);
        }
        return 0;
    }
    if (bRequest==bRebuild) return Fail(TEXT("Provide either Request JSON or an owned Contribution to rebuild from its saved recipe."));
    TSharedPtr<FJsonObject> Job;
    TStrongObjectPtr<URetakeWeaponPoseContribution> Rebuilt;
    if (bRebuild)
    {
        const auto RebuildPlugin=IPluginManager::Get().FindPlugin(Selected);
        if (!FRetakeModManifest::IsValidModId(Selected) || !RebuildPlugin || !RebuildPlugin->IsEnabled()
            || RetakeModWorkspace::Kind(Selected)!=TEXT("weapons") || !URetakeWeaponPack::IsOwnedObjectPath(Selected,FSoftObjectPath(Rebuild)))
            return Fail(TEXT("Select the enabled weapon mod and a canonical owned contribution path."));
        Rebuilt.Reset(Cast<URetakeWeaponPoseContribution>(FSoftObjectPath(Rebuild).TryLoad()));
        if (!Rebuilt || Rebuilt->GetClass()!=URetakeWeaponPoseContribution::StaticClass() || Rebuilt->GetOutermost()->IsDirty())
            return Fail(TEXT("Load and save the native contribution before rebuilding its recipe."));
        const auto* SavedRecipe=Cast<URetakeWeaponPoseRecipe>(Rebuilt->AuthoringRecipe);
        if (!SavedRecipe || SavedRecipe->GetClass()!=URetakeWeaponPoseRecipe::StaticClass() || SavedRecipe->GetOuter()!=Rebuilt.Get())
            return Fail(TEXT("This contribution has no saved native recipe; prepare it once with an explicit Request JSON."));
        Job=MakeShared<FJsonObject>();Job->SetStringField(TEXT("owner"),Selected);
        Job->SetStringField(TEXT("basis"),SavedRecipe->Basis.ToSoftObjectPath().ToString());Job->SetStringField(TEXT("output"),Rebuild);
        TArray<TSharedPtr<FJsonValue>> Sources;
        for (const auto& Source:SavedRecipe->Specifications) Sources.Add(MakeShared<FJsonValueString>(Source.ToSoftObjectPath().ToString()));
        Job->SetArrayField(TEXT("specifications"),Sources);
    }
    else if (IFileManager::Get().FileSize(*Request)<1
        || IFileManager::Get().FileSize(*Request)>1024*1024 || !FFileHelper::LoadFileToString(Text,*Request)
        || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Job)) return Fail(TEXT("Provide a valid preparation JSON request smaller than 1 MiB."));
    if (!Job || !Job->TryGetStringField(TEXT("owner"),Owner) || !Job->TryGetStringField(TEXT("basis"),BasisPath)
        || !Job->TryGetStringField(TEXT("output"),OutputPath)) return Fail(TEXT("Provide an owner/basis/output/specifications JSON request."));
    if (!FRetakeModManifest::IsValidModId(Owner) || !URetakeWeaponPack::IsOwnedObjectPath(Owner,FSoftObjectPath(OutputPath)))
        return Fail(TEXT("The output must be a canonical object path inside the owning mod."));
    if (!Selected.IsEmpty() && Selected!=Owner) return Fail(TEXT("The request owner differs from the selected mod."));
    const auto Plugin=IPluginManager::Get().FindPlugin(Owner);
    if (!Plugin || !Plugin->IsEnabled()) return Fail(TEXT("Enable the weapon mod plugin before preparing its content."));
    if (RetakeModWorkspace::Kind(Owner)!=TEXT("weapons")) return Fail(TEXT("Weapon pose preparation requires a weapon-mod workspace."));
    const FSoftObjectPath BasisReference(BasisPath);
    if (!URetakeWeaponPack::IsOwnedObjectPath(Owner,BasisReference)
        && !URetakeWeaponPack::IsOwnedObjectPath(TEXT("RetakeMapContent"),BasisReference))
        return Fail(TEXT("The authoring basis must be owned by the mod or the shared SDK content pack."));
    const TArray<TSharedPtr<FJsonValue>>* Entries=nullptr;
    if (!Job->TryGetArrayField(TEXT("specifications"),Entries) || Entries->IsEmpty() || Entries->Num()>512)
        return Fail(TEXT("Choose 1-512 owned weapon specifications."));
    TArray<FSoftObjectPath> Paths;
    for (const auto& Entry:*Entries)
    {
        FString Path;
        if (!Entry->TryGetString(Path) || !URetakeWeaponPack::IsOwnedObjectPath(Owner,FSoftObjectPath(Path)))
            return Fail(TEXT("Every specification must be a canonical object path owned by the mod."));
        Paths.Add(FSoftObjectPath(Path));
    }
    auto& Registry=FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();Registry.SearchAllAssets(true);
    TStrongObjectPtr<URetakeWeaponPoseBasis> Basis(Cast<URetakeWeaponPoseBasis>(BasisReference.TryLoad()));
    if (!Basis || Basis->GetClass()!=URetakeWeaponPoseBasis::StaticClass()) return Fail(TEXT("The native authoring basis could not be loaded."));
    if (Basis->GetOutermost()->IsDirty()) return Fail(TEXT("Save the authoring basis before preparing weapon poses."));
    FString ExpectedBodyIdentity;
    if (Job->TryGetStringField(TEXT("expectedBodyIdentity"),ExpectedBodyIdentity)
        && (ExpectedBodyIdentity.Len()!=64 || Basis->Body.ComputeIdentity()!=ExpectedBodyIdentity))
        return Fail(TEXT("The saved authoring body differs from the requested game contract; restore the exact published basis before preparing poses."));
    TArray<TStrongObjectPtr<URetakeWeaponSpecification>> Retained;TArray<const URetakeWeaponSpecification*> Definitions;
    for (const auto& Path:Paths)
    {
        auto* Definition=Cast<URetakeWeaponSpecification>(Path.TryLoad());
        if (!Definition || Definition->GetClass()!=URetakeWeaponSpecification::StaticClass()) return Fail(TEXT("Missing or unsupported specification: ")+Path.ToString());
        Retained.Emplace(Definition);Definitions.Add(Definition);
    }
    const FSoftObjectPath Destination(OutputPath);
    const bool Existing=FPackageName::DoesPackageExist(Destination.GetLongPackageName());
    UPackage* Package=Existing ? LoadPackage(nullptr,*Destination.GetLongPackageName(),LOAD_None):CreatePackage(*Destination.GetLongPackageName());
    if (!Package) return Fail(TEXT("Could not prepare the output package."));
    auto* Output=Existing ? Cast<URetakeWeaponPoseContribution>(Destination.ResolveObject())
        :NewObject<URetakeWeaponPoseContribution>(Package,FName(*Destination.GetAssetName()),RF_Public|RF_Standalone);
    if (!Output || Output->GetClass()!=URetakeWeaponPoseContribution::StaticClass()) return Fail(TEXT("Output already exists with an unsupported asset type."));
    auto* Recipe=Cast<URetakeWeaponPoseRecipe>(Output->AuthoringRecipe);
    if (Output->AuthoringRecipe && (!Recipe || Recipe->GetClass()!=URetakeWeaponPoseRecipe::StaticClass() || Recipe->GetOuter()!=Output))
        return Fail(TEXT("The output has an unsupported authoring recipe object."));
    FString Error;
    if (!RetakeWeaponPoseBaking::BakeContribution(Owner,Basis->GetInputs(),Definitions,*Output,Error)) return Fail(Error);
    if (!Recipe) Recipe=NewObject<URetakeWeaponPoseRecipe>(Output,NAME_None,RF_Transactional);
    Recipe->Basis=Basis.Get();Recipe->Specifications.Reset();
    Paths.Sort([](const auto& A,const auto& B){return A.ToString().ToLower()<B.ToString().ToLower();});
    for (const auto& Path:Paths) Recipe->Specifications.Emplace(Path);
    Output->AuthoringRecipe=Recipe;
    if (!RetakeWeaponPosePreparation::ValidateSources(Owner,*Output,Error,Paths,false)) return Fail(Error);
    if (!UEditorLoadingAndSavingUtils::SavePackages({Package},false)) return Fail(TEXT("Could not save the validated pose contribution."));
    if (!Existing) FAssetRegistryModule::AssetCreated(Output);
    UE_LOGF(LogRetakeMapSDK,Display,"event=WeaponPosePrepared owner=%ls holds=%d hash=%ls source=%ls output=%ls",
        *Owner,Output->Holds.Num(),*Output->ContentHash,*Output->SourceSignature,*OutputPath);
    return 0;
}
