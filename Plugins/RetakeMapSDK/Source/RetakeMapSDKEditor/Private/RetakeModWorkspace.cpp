// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeModWorkspace.h"
#include "RetakeMapSDKManifest.h"
#include "RetakeMapSDKVersion.h"
#include "RetakeWeaponPack.h"
#include "RetakeWeaponPose.h"
#include "RetakeWeaponPoseRecipe.h"
#include "RetakeWeaponSpecification.h"
#include "RetakeMapSettings.h"
#include "RetakeModePlayerStart.h"
#include "RetakeAcousticMap.h"
#include "RetakeAcousticMapData.h"
#include "RetakeMinimapCapture.h"
#include "RetakeSpectatorOverview.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Settings/ContentBrowserSettings.h"
#include "Interfaces/IPluginManager.h"
#include "Components/BoxComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Materials/MaterialInterface.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/DataValidation.h"
#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshCompiler.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "Logging/TokenizedMessage.h"
#include "Engine/DataAsset.h"

namespace
{
FString WorkspaceRoot() { return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Mods")); }

bool CheckEditor(FString& Error)
{
    if (FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) != TEXT("RetakeModKit"))
        Error = TEXT("Open the standalone RetakeModKit project to author mods.");
    else if (!GEditor || GEditor->PlayWorld)
        Error = TEXT("Stop the play session before changing mod workspaces.");
    else return true;
    return false;
}

bool Mount(const FString& ModId, FString& Error)
{
    const FString Directory = WorkspaceRoot() / ModId;
    const FString Filename = Directory / (ModId + TEXT(".uplugin"));
    FString Text;
    TSharedPtr<FJsonObject> Descriptor;
    if (!FRetakeModManifest::IsValidModId(ModId) || !FFileHelper::LoadFileToString(Text, *Filename)
        || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Descriptor) || !Descriptor)
    { Error = TEXT("Select an existing mod from this kit's Mods folder."); return false; }
    bool bContent = false;
    if (!Descriptor->TryGetBoolField(TEXT("CanContainContent"), bContent) || !bContent)
    { Error = TEXT("A creator mod must be a content-only plugin."); return false; }
    for (const TCHAR* Field : { TEXT("Modules"), TEXT("Plugins"), TEXT("PreBuildSteps"), TEXT("PostBuildSteps"),
        TEXT("LocalizationTargets"), TEXT("AdditionalPluginDirectories"), TEXT("CanContainVerse"), TEXT("IsPluginExtension") })
        if (Descriptor->HasField(Field))
        { Error = FString::Printf(TEXT("Creator mods cannot declare %s."), Field); return false; }
    for (const TCHAR* Folder : { TEXT("Source"), TEXT("Config"), TEXT("Binaries") })
        if (IFileManager::Get().DirectoryExists(*(Directory / Folder)))
        { Error = FString::Printf(TEXT("Creator mods cannot contain a %s folder."), Folder); return false; }
    IPluginManager& Manager = IPluginManager::Get();
    TSharedPtr<IPlugin> Plugin = Manager.FindPlugin(ModId);
    if (Plugin && !FPaths::IsSamePath(FPaths::ConvertRelativePathToFull(Plugin->GetDescriptorFileName()), Filename))
    { Error = TEXT("Another plugin already uses this mod name."); return false; }
    if (!Plugin)
    {
        FText Failure;
        if (!Manager.AddToPluginsList(Filename, &Failure)) { Error = Failure.ToString(); return false; }
        Plugin = Manager.FindPlugin(ModId);
    }
    if (!Plugin) { Error = TEXT("The editor could not register this mod."); return false; }
    if (!Plugin->IsEnabled()) Manager.MountNewlyCreatedPlugin(ModId);
    if (!Plugin->IsEnabled() || Plugin->GetMountedAssetPath() != TEXT("/") + ModId + TEXT("/"))
    { Error = TEXT("The editor could not mount this mod."); return false; }
    FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get()
        .ScanPathsSynchronous({ TEXT("/") + ModId + TEXT("/") }, true);
    return true;
}
}

const TArray<RetakeModWorkspace::FKind>& RetakeModWorkspace::Kinds()
{
    static const TArray<FKind> Values={
        {TEXT("map"),TEXT("Maps"),TEXT("Build levels with Retake elements, preparation, minimaps and map exports.")},
        {TEXT("weapons"),TEXT("Weapons"),TEXT("Author weapon specifications, resources, catalogues and profiles as a separate pack.")},
        {TEXT("game-mode"),TEXT("Game modes"),TEXT("Create new match rules. The creator execution and authority contract is still planned.")}};
    return Values;
}
FString RetakeModWorkspace::KindLabel(const FString& Id)
{
    for(const FKind& Entry:Kinds())if(Id==Entry.Id)return Entry.Name;
    return TEXT("Unsupported type");
}
FString RetakeModWorkspace::Kind(const FString& ModId)
{
    if(!FRetakeModManifest::IsValidModId(ModId))return TEXT("unknown");
    FString Filename=WorkspaceRoot()/ModId/(ModId+TEXT(".uplugin"));
    if(const TSharedPtr<IPlugin> Plugin=IPluginManager::Get().FindPlugin(ModId))Filename=Plugin->GetDescriptorFileName();
    FString Text;TSharedPtr<FJsonObject> Descriptor;
    if(!FFileHelper::LoadFileToString(Text,*Filename)||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Descriptor)||!Descriptor)return TEXT("unknown");
    // Optional editor workspace metadata: the existing map-only descriptors have the defined map default.
    if(!Descriptor->HasField(TEXT("RetakeModKind")))return TEXT("map");
    FString Id;if(!Descriptor->TryGetStringField(TEXT("RetakeModKind"),Id))return TEXT("unknown");
    for(const FKind& Entry:Kinds())if(Id==Entry.Id)return Id;
    return TEXT("unknown");
}
bool RetakeModWorkspace::CanCreateKind(const FString& Id,FString& Reason)
{
    Reason.Reset();
    if(Id==TEXT("map"))return true;
    if(Id==TEXT("weapons"))
    {
        if(FindObject<UClass>(nullptr,TEXT("/Script/RetakeMapSDK.RetakeWeaponPack"))
            &&FindObject<UClass>(nullptr,TEXT("/Script/RetakeMapSDK.RetakeWeaponSpecification")))
        {Reason=TEXT("Authoring draft: playable weapon export, registration and testing are still being implemented.");return true;}
        Reason=TEXT("This kit needs the public weapon-authoring SDK before weapon drafts can be created.");return false;
    }
    Reason=Id==TEXT("game-mode")?TEXT("Game-mode mod creation is planned. New match-rule execution, validation and server authority are not enabled yet."):TEXT("This mod type is not supported by the installed kit.");
    return false;
}
bool RetakeModWorkspace::CanExportKind(const FString& Id) { return Id==TEXT("map")||Id==TEXT("weapons"); }

bool RetakeModWorkspace::CheckWeaponDraft(const FString& ModId,FString& ReportJson,FString& Error)
{
    if(Kind(ModId)!=TEXT("weapons")){Error=TEXT("Select a weapon draft for this check.");return false;}
    if(!CanCreateKind(TEXT("weapons"),Error)||!Open(ModId,Error))return false;
    const FString Root=TEXT("/")+ModId+TEXT("/");
    IAssetRegistry& Registry=FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    TArray<FAssetData> Assets;Registry.GetAssetsByPath(FName(*(TEXT("/")+ModId)),Assets,true);
    TArray<TSharedPtr<FJsonValue>> Issues;int32 Errors=0,Warnings=0,Checked=0,Catalogues=0,Specifications=0;
    const auto Add=[&](bool IsError,const FString& Asset,const FString& Detail)
    {
        auto Item=MakeShared<FJsonObject>();Item->SetBoolField(TEXT("error"),IsError);Item->SetStringField(TEXT("code"),TEXT("Draft"));
        Item->SetStringField(TEXT("actor"),Asset);Item->SetStringField(TEXT("detail"),Detail);Issues.Add(MakeShared<FJsonValueObject>(Item));
        IsError?++Errors:++Warnings;
    };
    for(const FAssetData& Asset:Assets)
    {
        const FString ClassPath=Asset.AssetClassPath.ToString();
        if(ClassPath==TEXT("/Script/Engine.World")){Add(true,Asset.PackageName.ToString(),TEXT("Weapon packs do not contain playable maps; keep levels in a separate map mod."));continue;}
        if(ClassPath.Contains(TEXT("Blueprint"))){Add(true,Asset.PackageName.ToString(),TEXT("Weapon drafts use supported native data assets, not gameplay Blueprint classes."));continue;}
        TArray<FName> Dependencies;Registry.GetDependencies(Asset.PackageName,Dependencies);
        for(const FName& Dependency:Dependencies)
        {
            const FString Path=Dependency.ToString();
            const bool PrivateNative=Path.StartsWith(TEXT("/Script/Retake"))&&Path!=TEXT("/Script/RetakeMapSDK")
                &&Path!=TEXT("/Script/RetakeMapSDKEditor")&&Path!=TEXT("/Script/RetakeDestruction")&&Path!=TEXT("/Script/RetakeDestructionEditor");
            bool Allowed=Path.StartsWith(Root)||(Path.StartsWith(TEXT("/Script/"))&&!PrivateNative);
            for(const TCHAR* Shared:RetakeMapSDK::AllowedForeignRoots)Allowed|=Path.StartsWith(Shared);
            if(!Allowed)Add(true,Asset.PackageName.ToString(),TEXT("Unsupported draft dependency: ")+Path+TEXT(". Use owned or SDK/shared resources."));
        }
        if(ClassPath==TEXT("/Script/RetakeMapSDK.RetakeWeaponPack"))++Catalogues;
        if(ClassPath==TEXT("/Script/RetakeMapSDK.RetakeWeaponSpecification"))++Specifications;
        if(ClassPath.StartsWith(TEXT("/Script/Retake."))){Add(true,Asset.PackageName.ToString(),TEXT("Private game definitions cannot be shipped by a weapon pack; use the public specification."));continue;}
        if(!ClassPath.StartsWith(TEXT("/Script/RetakeMapSDK.")))continue;
        UObject* Object=Asset.GetAsset();if(!Object){Add(true,Asset.PackageName.ToString(),TEXT("Could not load this authoring asset."));continue;}
        FDataValidationContext Context;const EDataValidationResult Result=Object->IsDataValid(Context);++Checked;
        for(const FDataValidationContext::FIssue& Issue:Context.GetIssues())
            Add(Issue.Severity==EMessageSeverity::Error,Asset.PackageName.ToString(),Issue.TokenizedMessage?Issue.TokenizedMessage->ToText().ToString():Issue.Message.ToString());
        if(Result==EDataValidationResult::Invalid&&Context.GetIssues().IsEmpty())Add(true,Asset.PackageName.ToString(),TEXT("Native authoring validation failed."));
        if(const auto* Poses=Cast<URetakeWeaponPoseContribution>(Object))
        {
            FString PreparationError;
            if(!RetakeWeaponPosePreparation::ValidateSources(ModId,*Poses,PreparationError))Add(true,Asset.PackageName.ToString(),PreparationError);
        }
        if(const auto* Catalogue=Cast<URetakeWeaponPack>(Object))
        {
            FString CatalogueError;
            if(Catalogue->PackId!=ModId)Add(true,Asset.PackageName.ToString(),TEXT("The catalogue must declare this workspace's owning mod ID."));
            if(Catalogue->Validate(CatalogueError)) for(const auto& Profile:Catalogue->Profiles)
            {
                auto* Poses=Cast<URetakeWeaponPoseContribution>(Profile.PoseContribution.TryLoad());
                if(!Poses||Poses->GetClass()!=URetakeWeaponPoseContribution::StaticClass())
                {Add(true,Asset.PackageName.ToString(),TEXT("Profile ")+Profile.ProfileId+TEXT(" is missing its native prepared pose contribution."));continue;}
                TArray<FSoftObjectPath> Required;TArray<const URetakeWeaponSpecification*> StartingDefinitions;
                for(const auto& Binding:Profile.Weapons)
                {
                    Required.Add(Binding.Value);
                    auto* Specification=Cast<URetakeWeaponSpecification>(Binding.Value.TryLoad());
                    if (Specification)
                    {
                        Specification->Ammo.LoadSynchronous();Specification->SecondaryAmmo.LoadSynchronous();
                        StartingDefinitions.Add(Specification);
                    }
                    if(!Specification||Specification->GetClass()!=URetakeWeaponSpecification::StaticClass()
                        ||Specification->WeaponId!=FName(*(ModId+TEXT(".")+Binding.Key)))
                        Add(true,Asset.PackageName.ToString(),TEXT("Profile ")+Profile.ProfileId+TEXT(" has a missing or incorrectly identified native specification: ")+Binding.Key);
                }
                FString StartingError;
                if (!URetakeWeaponSpecification::ValidateStartingReferences(StartingDefinitions,StartingError))
                    Add(true,Asset.PackageName.ToString(),TEXT("Profile ")+Profile.ProfileId+TEXT(": ")+StartingError);
                FString PreparationError;
                if(!RetakeWeaponPosePreparation::ValidateSources(ModId,*Poses,PreparationError,Required))
                    Add(true,Asset.PackageName.ToString(),TEXT("Profile ")+Profile.ProfileId+TEXT(": ")+PreparationError);
            }
        }
    }
    if(Catalogues!=1)Add(true,Root,TEXT("A weapon draft needs exactly one weapon catalogue."));
    if(Specifications==0)Add(true,Root,TEXT("Add at least one public weapon specification and bind it in a catalogue profile."));
    auto Report=MakeShared<FJsonObject>();Report->SetNumberField(TEXT("format"),1);Report->SetBoolField(TEXT("complete"),true);
    Report->SetStringField(TEXT("modId"),ModId);Report->SetStringField(TEXT("kind"),TEXT("weapons"));Report->SetBoolField(TEXT("valid"),Errors==0);
    Report->SetBoolField(TEXT("exportReady"),false);Report->SetNumberField(TEXT("assetsChecked"),Checked);Report->SetArrayField(TEXT("issues"),Issues);
    FJsonSerializer::Serialize(Report,TJsonWriterFactory<>::Create(&ReportJson));
    if(Errors)Error=FString::Printf(TEXT("Weapon draft has %d error(s), %d warning(s)."),Errors,Warnings);
    return Errors==0;
}

TArray<FString> RetakeModWorkspace::List()
{
    TArray<FString> Names;
    IFileManager::Get().FindFiles(Names, *(WorkspaceRoot() / TEXT("RetakeMod_*")), false, true);
    Names.RemoveAll([](const FString& Name)
    { return !FRetakeModManifest::IsValidModId(Name) || !FPaths::FileExists(WorkspaceRoot() / Name / (Name + TEXT(".uplugin"))); });
    Names.Sort();
    return Names;
}

void RetakeModWorkspace::Reveal(const FString& ModId)
{
    if (IsRunningCommandlet()) return;
    UContentBrowserSettings* Settings = GetMutableDefault<UContentBrowserSettings>();
    Settings->SetDisplayPluginFolders(true);
    Settings->PostEditChange();
    Settings->SaveConfig();
    FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get()
        .SetSelectedPaths({ TEXT("/") + ModId });
}

bool RetakeModWorkspace::Open(const FString& ModId, FString& Error)
{
    if (!CheckEditor(Error) || !Mount(ModId, Error)) return false;
    Reveal(ModId);
    return true;
}

bool RetakeModWorkspace::Create(const FString& Name, bool bBasicLevel, FString& OutModId, FString& Error, const FString& ModKind)
{
    OutModId.Reset(); Error.Reset();
    if (!CheckEditor(Error)) return false;
    if(!CanCreateKind(ModKind,Error))return false;
    Error.Reset();
    if(ModKind!=TEXT("map")&&bBasicLevel){Error=TEXT("Empty/Basic level templates apply only to map mods.");return false;}
    FString ShortName = Name.TrimStartAndEnd();
    ShortName.RemoveFromStart(TEXT("RetakeMod_"));
    const FString ModId = TEXT("RetakeMod_") + ShortName;
    if (!FRetakeModManifest::IsValidModId(ModId))
    { Error = TEXT("Use a short mod name containing letters, numbers and underscores."); return false; }
    const FString Directory = WorkspaceRoot() / ModId;
    if (IFileManager::Get().DirectoryExists(*Directory) || List().ContainsByPredicate([&](const FString& Existing)
        { return Existing.Equals(ModId, ESearchCase::IgnoreCase); }))
    { Error = TEXT("That mod folder already exists. Open it or choose another name."); return false; }
    TArray<UPackage*> DirtyMaps;
    UEditorLoadingAndSavingUtils::GetDirtyMapPackages(DirtyMaps);
    if (ModKind==TEXT("map") && !DirtyMaps.IsEmpty())
    { Error = TEXT("Save or revert your current level before creating another map mod."); return false; }
    const TArray<FString> Folders=ModKind==TEXT("map")
        ? TArray<FString>{TEXT("Maps"),TEXT("Meshes"),TEXT("Materials"),TEXT("Textures"),TEXT("Audio"),TEXT("Physics")}
        : TArray<FString>{TEXT("Data"),TEXT("Weapons"),TEXT("Ammo"),TEXT("Profiles"),TEXT("Meshes"),TEXT("Materials"),TEXT("Textures"),TEXT("Audio"),TEXT("Animations"),TEXT("VFX")};
    for (const FString& Folder : Folders)
        if (!IFileManager::Get().MakeDirectory(*(Directory / TEXT("Content") / Folder), true))
        { Error = TEXT("Could not create the mod's content folders."); return false; }
    TSharedRef<FJsonObject> Descriptor = MakeShared<FJsonObject>();
    Descriptor->SetNumberField(TEXT("FileVersion"), 3);
    Descriptor->SetNumberField(TEXT("Version"), 1);
    Descriptor->SetStringField(TEXT("VersionName"), TEXT("0.1.0"));
    Descriptor->SetStringField(TEXT("FriendlyName"), ShortName);
    Descriptor->SetStringField(TEXT("Category"), TEXT("Retake Mods"));
    Descriptor->SetStringField(TEXT("RetakeModKind"),ModKind);
    Descriptor->SetBoolField(TEXT("CanContainContent"), true);
    FString Json;
    if (!FJsonSerializer::Serialize(Descriptor, TJsonWriterFactory<>::Create(&Json))
        || !FFileHelper::SaveStringToFile(Json, *(Directory / (ModId + TEXT(".uplugin"))), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    { Error = TEXT("Could not write the mod descriptor."); return false; }
    if (!Mount(ModId, Error)) return false;
    if(ModKind==TEXT("weapons"))
    {
        UClass* Class=FindObject<UClass>(nullptr,TEXT("/Script/RetakeMapSDK.RetakeWeaponPack"));
        if(!Class||!Class->IsChildOf(UDataAsset::StaticClass())){Error=TEXT("The public weapon catalogue class is unavailable.");return false;}
        UPackage* Package=CreatePackage(*(TEXT("/")+ModId+TEXT("/Data/DA_WeaponPack")));
        UObject* Catalogue=NewObject<UObject>(Package,Class,TEXT("DA_WeaponPack"),RF_Public|RF_Standalone|RF_Transactional);
        if(FStrProperty* Property=FindFProperty<FStrProperty>(Class,TEXT("PackId")))Property->SetPropertyValue_InContainer(Catalogue,ModId);
        if(FStrProperty* Property=FindFProperty<FStrProperty>(Class,TEXT("DisplayName")))Property->SetPropertyValue_InContainer(Catalogue,ShortName);
        FAssetRegistryModule::AssetCreated(Catalogue);Package->MarkPackageDirty();
        if(!UEditorLoadingAndSavingUtils::SavePackages({Package},false)){Error=TEXT("The weapon draft exists, but its catalogue could not be saved.");return false;}
        OutModId=ModId;Reveal(ModId);return true;
    }
    UWorld* World = UEditorLoadingAndSavingUtils::NewBlankMap(false);
    if (!World) { Error = TEXT("The workspace exists, but the editor could not create its level."); return false; }
    const FString Root = TEXT("/") + ModId + TEXT("/");
    // Establish the final world identity before generating preparation data. The empty level is valid to save;
    // creating an unbound Acoustic Map first would emit a misleading validation error during starter creation.
    if (!UEditorLoadingAndSavingUtils::SaveMap(World, Root + TEXT("Maps/Main")))
    { Error = TEXT("The workspace exists, but its starter level could not be saved."); return false; }
    TArray<UPackage*> Packages;
    if (bBasicLevel)
    {
        UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/RetakeMapContent/Materials/M_Retake_Surface.M_Retake_Surface"));
        if (!Cube || !Material) { Error = TEXT("The Basic template needs the installed SDK material and engine cube."); return false; }
        UStaticMesh* Block = Cast<UStaticMesh>(FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get()
            .DuplicateAsset(TEXT("SM_BasicBlock"), Root + TEXT("Meshes"), Cube));
        if (!Block) { Error = TEXT("Could not create the Basic template mesh."); return false; }
        Block->GetNaniteSettings().bEnabled = false;
        Block->bAllowCPUAccess = true;
        Block->SetMaterial(0, Material);
        Block->PostEditChange();
        FStaticMeshCompilingManager::Get().FinishCompilation({ Block });
        Block->MarkPackageDirty(); Packages.Add(Block->GetOutermost());
        UPackage* PhysicsPackage = CreatePackage(*(Root + TEXT("Physics/PM_Concrete")));
        UPhysicalMaterial* Concrete = NewObject<UPhysicalMaterial>(PhysicsPackage, TEXT("PM_Concrete"), RF_Public | RF_Standalone | RF_Transactional);
        Concrete->SurfaceType = SurfaceType2; // Canonical Concrete; enforced by the SDK's structural profile.
        FAssetRegistryModule::AssetCreated(Concrete);
        Concrete->MarkPackageDirty(); Packages.Add(PhysicsPackage);
        auto Spawn = [World](UClass* Class, const FVector& Location, const FRotator& Rotation = FRotator::ZeroRotator)
        { return GEditor->AddActor(World->PersistentLevel, Class, FTransform(Rotation, Location), true, RF_Transactional, false); };
        AStaticMeshActor* Floor = CastChecked<AStaticMeshActor>(Spawn(AStaticMeshActor::StaticClass(), FVector(0, 0, -50)));
        Floor->SetActorLabel(TEXT("Basic Floor"));
        Floor->GetStaticMeshComponent()->SetStaticMesh(Block);
        Floor->GetStaticMeshComponent()->SetMaterial(0, Material);
        Floor->GetStaticMeshComponent()->SetPhysMaterialOverride(Concrete);
        Floor->SetActorScale3D(FVector(50, 50, 1));
        ADirectionalLight* Sun = CastChecked<ADirectionalLight>(Spawn(ADirectionalLight::StaticClass(), FVector(0, 0, 1000), FRotator(-40, 30, 0)));
        Sun->GetLightComponent()->SetMobility(EComponentMobility::Movable);
        CastChecked<UDirectionalLightComponent>(Sun->GetLightComponent())->SetAtmosphereSunLight(true);
        Spawn(ASkyAtmosphere::StaticClass(), FVector::ZeroVector);
        ASkyLight* Sky = CastChecked<ASkyLight>(Spawn(ASkyLight::StaticClass(), FVector(0, 0, 1000)));
        Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
        Sky->GetLightComponent()->SetRealTimeCaptureEnabled(true);
        ARetakeMapSettings* Settings = CastChecked<ARetakeMapSettings>(Spawn(ARetakeMapSettings::StaticClass(), FVector::ZeroVector));
        Settings->DisplayName = ShortName;
        for (int32 Side = 0; Side < 2; ++Side)
            for (int32 Row = 0; Row < 2; ++Row)
            {
                const FVector Location(Side ? 1500 : -1500, Row ? 500 : -500, 100);
                ARetakeModePlayerStart* Start = CastChecked<ARetakeModePlayerStart>(Spawn(ARetakeModePlayerStart::StaticClass(), Location, FRotator(0, Side ? 180 : 0, 0)));
                Start->StartRole = ERetakeModeStartRole::Team; Start->TeamId = Side;
                Start->SetActorLabel(FString::Printf(TEXT("Team %d Start %d"), Side + 1, Row + 1));
                ARetakeModePlayerStart* Shared = CastChecked<ARetakeModePlayerStart>(Spawn(ARetakeModePlayerStart::StaticClass(), Location + FVector(0, 200, 0), FRotator(0, Side ? 180 : 0, 0)));
                Shared->SetActorLabel(FString::Printf(TEXT("Shared Start %d"), Side * 2 + Row + 1));
            }
        ARetakeAcousticMap* Acoustics = CastChecked<ARetakeAcousticMap>(Spawn(ARetakeAcousticMap::StaticClass(), FVector::ZeroVector));
        ARetakeAcousticRegion* Region = CastChecked<ARetakeAcousticRegion>(Spawn(ARetakeAcousticRegion::StaticClass(), FVector(0, 0, 500)));
        Region->RegionId = TEXT("Exterior");
        Region->Bounds->SetBoxExtent(FVector(2600, 2600, 1500));
        ARetakeMinimapCapture* Capture = CastChecked<ARetakeMinimapCapture>(Spawn(ARetakeMinimapCapture::StaticClass(), FVector::ZeroVector));
        Capture->CaptureSize = 5200;
        Spawn(ARetakeSpectatorOverview::StaticClass(), FVector(-1500, 0, 1600), FRotator(-45, 0, 0));
        World->GetWorldSettings()->KillZ = -2000;
        UPackage* AcousticPackage = CreatePackage(*(Root + TEXT("Audio/DA_Acoustics_Main")));
        Acoustics->Data = NewObject<URetakeAcousticMapData>(AcousticPackage, TEXT("DA_Acoustics_Main"), RF_Public | RF_Standalone | RF_Transactional);
        FAssetRegistryModule::AssetCreated(Acoustics->Data);
        Acoustics->Bake();
        if (!Acoustics->ValidateBake(Error)) return false;
        Packages.Add(AcousticPackage);
    }
    if (!Packages.IsEmpty() && !UEditorLoadingAndSavingUtils::SavePackages(Packages, false))
    { Error = TEXT("The workspace exists, but its template assets could not be saved."); return false; }
    if (!UEditorLoadingAndSavingUtils::SaveMap(World, Root + TEXT("Maps/Main")))
    { Error = TEXT("The workspace exists, but its starter level could not be saved."); return false; }
    OutModId = ModId;
    Reveal(ModId);
    return true;
}
