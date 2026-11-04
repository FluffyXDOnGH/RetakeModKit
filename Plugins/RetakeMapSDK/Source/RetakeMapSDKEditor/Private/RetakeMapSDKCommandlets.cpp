// Copyright 2026 Codnyx. All Rights Reserved.

#include "RetakeMapSDKCommandlets.h"
#include "RetakeAcousticMap.h"
#include "RetakeMapSDKLog.h"
#include "RetakeMapSDKManifest.h"
#include "RetakeMapSDKProfile.h"
#include "RetakeMapSDKVersion.h"
#include "RetakeMapValidator.h"
#include "RetakeMapSettings.h"
#include "RetakeModWorkspace.h"
#include "RetakeWeaponPack.h"
#include "RetakeWeaponPose.h"
#include "RetakeWeaponSpecification.h"
#include "RetakeVisibilityData.h"
#include "AssetCompilingManager.h"
#include "RetakeMinimapBaker.h"
#include "RetakeMinimapCapture.h"
#include "RetakeMinimapDefinition.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetRegistryState.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "IO/IoDispatcher.h"
#include "IO/IoChunkId.h"
#include "IO/PackageId.h"
#include "Serialization/AsyncLoading2.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Class.h"
#include "UObject/SavePackage.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Misc/PackageName.h"

namespace
{
    const TCHAR* const ProfilePlatforms[] = { TEXT("Windows"), TEXT("Linux") };

    bool ProfileHashes(TMap<FString, FString>& Out, const FString& OutDir)
    {
        for (const TCHAR* Platform : ProfilePlatforms)
        {
            FConfigCacheIni* Config = FConfigCacheIni::ForPlatform(FName(Platform));
            if (!Config)
            {
                UE_LOGF(LogRetakeMapSDK, Error, "event=MapSDKProfileFailed platform=%ls reason=NoPlatformConfig", Platform);
                return false;
            }
            Out.Add(Platform, FRetakeMapSDKProfile::ComputeHash(*Config, Platform));
            if (!OutDir.IsEmpty())
                FFileHelper::SaveStringToFile(FRetakeMapSDKProfile::BuildCanonicalText(*Config, Platform),
                    *FPaths::Combine(OutDir, FString::Printf(TEXT("profile-%s.txt"), Platform)), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        }
        return true;
    }

    /**
     * The playable worlds of a mod plugin and every package it holds. A world another mod world references (streaming sublevel,
     * level instance) is not a playable map of its own.
     */
    bool FindModContent(const FString& ModId, TArray<FAssetData>& OutMaps, TSet<FName>& OutPackages, FString& Error,bool bAllowWeapons=false)
    {
        const FString Kind=RetakeModWorkspace::Kind(ModId);
        if(Kind!=TEXT("map")&&!(bAllowWeapons&&Kind==TEXT("weapons")))
        {Error=TEXT("This operation requires a supported map or weapon export kind; map preparation actions accept maps only.");return false;}

        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(ModId);
        if (!Plugin || !Plugin->IsEnabled()) { Error = TEXT("the mod plugin is not loaded in this project"); return false; }
        IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        const FString Root = TEXT("/") + ModId + TEXT("/");
        Registry.ScanPathsSynchronous({ Root }, true);
        TArray<FAssetData> Assets;
        Registry.GetAssetsByPath(FName(*Root.LeftChop(1)), Assets, true);
        // Maps carry no gameplay scripts (MKD-009): no Blueprint asset of any kind (actor, animation, widget, data-only).
        TSet<FTopLevelAssetPath> BlueprintClasses;
        Registry.GetDerivedClassNames({ UBlueprint::StaticClass()->GetClassPathName() }, {}, BlueprintClasses);
        BlueprintClasses.Add(UBlueprint::StaticClass()->GetClassPathName());
        for (const FAssetData& Asset : Assets)
            if (BlueprintClasses.Contains(Asset.AssetClassPath))
            { Error = FString::Printf(TEXT("%s is a Blueprint; these exports carry native data assets, not gameplay scripts"), *Asset.GetObjectPathString()); return false; }
        TSet<FName> Worlds;
        for (const FAssetData& Asset : Assets)
        {
            OutPackages.Add(Asset.PackageName);
            if (Asset.AssetClassPath == UWorld::StaticClass()->GetClassPathName()) Worlds.Add(Asset.PackageName);
        }
        if(Kind==TEXT("weapons")&&!Worlds.IsEmpty()) {Error=TEXT("A weapon pack cannot contain worlds; use a separate map mod.");return false;}
        for (const FAssetData& Asset : Assets)
        {
            if (!Worlds.Contains(Asset.PackageName) || Asset.AssetClassPath != UWorld::StaticClass()->GetClassPathName()) continue;
            TArray<FName> Referencers;
            Registry.GetReferencers(Asset.PackageName, Referencers);
            if (!Referencers.ContainsByPredicate([&Worlds, &Asset](FName R) { return R != Asset.PackageName && Worlds.Contains(R); })) OutMaps.Add(Asset);
        }
        return true;
    }

    /** The part of a -Modes= spec for one map: "auto", "FFA,TDM" for every map, or "Map=FFA,TDM;Other=auto". */
    FString ModeSpecFor(const FString& Spec, const FString& MapName)
    {
        if (!Spec.Contains(TEXT("="))) return Spec.IsEmpty() ? FString(TEXT("auto")) : Spec;
        TArray<FString> Entries;
        Spec.ParseIntoArray(Entries, TEXT(";"), true);
        for (const FString& Entry : Entries)
        {
            FString Name, Modes;
            if (Entry.Split(TEXT("="), &Name, &Modes) && Name.TrimStartAndEnd().Equals(MapName, ESearchCase::IgnoreCase)) return Modes.TrimStartAndEnd();
        }
        return FString();
    }

    /**
     * Validates one map (RetakeMapValidator, presentation included) and resolves the modes it offers: every mode it passes for
     * "auto", else exactly the listed ones, which must all pass. Fills OutMap's modes and presentation. Logs each issue; false with
     * Error when the map cannot be exported. Validate and export both run this, so they report the same issues.
     */
    bool ResolveMapModes(const FAssetData& Map, const FString& ModRoot, const FString& Spec, FRetakeModMap& OutMap, FString& Error,
        TArray<FRetakeMapIssue>* OutIssues = nullptr, TArray<TSharedPtr<FJsonValue>>* OutPreparation = nullptr)
    {
        TArray<FString>& OutModes = OutMap.SupportedModes;
        const FString MapName = Map.AssetName.ToString();
        const FString MapSpec = ModeSpecFor(Spec, MapName);
        if (MapSpec.IsEmpty()) { Error = FString::Printf(TEXT("-Modes names no modes for map %s"), *MapName); return false; }
        UWorld* World = UEditorLoadingAndSavingUtils::LoadMap(Map.PackageName.ToString());
        if (!World) { Error = FString::Printf(TEXT("could not load %s"), *Map.PackageName.ToString()); return false; }
        TArray<FRetakeMapIssue> Issues;
        RetakeMapValidator::ValidateWorld(*World, Issues);
        bool bValid = !Issues.ContainsByPredicate([](const FRetakeMapIssue& I) { return I.bError; });
        if (MapSpec.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
        {
            for (const RetakeMapSDK::FModeRequirement& Mode : RetakeMapSDK::ModeRequirements)
            {
                TArray<FRetakeMapIssue> ModeIssues;
                if (RetakeMapValidator::ValidateMode(*World, Mode, ModeIssues)) { OutModes.Add(Mode.ModeId); Issues.Append(ModeIssues); }
                else
                {
                    const FRetakeMapIssue* First = ModeIssues.FindByPredicate([](const FRetakeMapIssue& I) { return I.bError; });
                    UE_LOGF(LogRetakeMapSDK, Display, "event=MapModeSkipped map=%ls mode=%ls reason=\"%ls\"", *MapName, Mode.ModeId, First ? *First->Detail : TEXT(""));
                }
            }
            if (OutModes.IsEmpty()) { bValid = false; Issues.Add({ true, TEXT("Modes"), FString(), TEXT("the map offers no mode; see MapModeSkipped") }); }
        }
        else
        {
            TArray<FString> Requested;
            MapSpec.ParseIntoArray(Requested, TEXT(","), true);
            for (FString& ModeId : Requested)
            {
                ModeId.TrimStartAndEndInline();
                const RetakeMapSDK::FModeRequirement* Mode = RetakeMapSDK::FindModeRequirement(ModeId);
                if (!Mode) { bValid = false; Issues.Add({ true, TEXT("Modes"), FString(), FString::Printf(TEXT("unknown mode %s"), *ModeId) }); continue; }
                if (!RetakeMapValidator::ValidateMode(*World, *Mode, Issues)) bValid = false;
                OutModes.AddUnique(Mode->ModeId);
            }
        }
        // Presentation includes per-mode authoring policies; mode eligibility must be resolved first.
        RetakeMapValidator::ValidatePresentation(*World, ModRoot, OutMap, Issues);
        bValid = bValid && !Issues.ContainsByPredicate([](const FRetakeMapIssue& I) { return I.bError; });
        int32 Errors = 0;
        for (const FRetakeMapIssue& Issue : Issues)
        {
            Errors += Issue.bError ? 1 : 0;
            UE_LOGF(LogRetakeMapSDK, Display, "event=MapValidation map=%ls level=%ls code=%ls actor=\"%ls\" detail=\"%ls\"",
                *MapName, Issue.bError ? TEXT("Error") : TEXT("Warning"), *Issue.Code, *Issue.Actor, *Issue.Detail);
        }
        UE_LOGF(LogRetakeMapSDK, Display, "event=MapValidationResult map=%ls modes=%ls errors=%d warnings=%d",
            *MapName, *FString::Join(OutModes, TEXT(",")), Errors, Issues.Num() - Errors);
        if (OutIssues) *OutIssues = Issues;
        // Editor workflow metadata only: use the same freshness checks as export; optional minimaps stay optional.
        if (OutPreparation)
        {
            int32 AcousticMaps = 0, MapSettings = 0;
            for (const ULevel* Level : World->GetLevels()) if (Level)
                for (const AActor* Actor : Level->Actors) if (IsValid(Actor))
                { AcousticMaps += Actor->IsA<ARetakeAcousticMap>(); MapSettings += Actor->IsA<ARetakeMapSettings>(); }
            const auto HasIssue = [&Issues](const TCHAR* Code)
            { return Issues.ContainsByPredicate([Code](const FRetakeMapIssue& Issue) { return Issue.Code == Code; }); };
            const auto Step = [OutPreparation](const TCHAR* Id, const TCHAR* State, const TCHAR* Detail)
            {
                auto Item = MakeShared<FJsonObject>();
                Item->SetStringField(TEXT("id"), Id); Item->SetStringField(TEXT("state"), State); Item->SetStringField(TEXT("detail"), Detail);
                OutPreparation->Add(MakeShared<FJsonValueObject>(Item));
            };
            const bool AcousticCurrent = !OutMap.Acoustics.IsEmpty() && !HasIssue(TEXT("Acoustics"));
            Step(TEXT("acoustics"), AcousticMaps != 1 ? TEXT("blocked") : AcousticCurrent ? TEXT("current") : TEXT("needed"),
                AcousticMaps != 1 ? TEXT("Place exactly one Acoustic Map before baking.") : AcousticCurrent ? TEXT("Acoustic bake is current.") : TEXT("Create or refresh the acoustic bake."));
            const int32 Captures = RetakeMinimapBaker::FindCaptures(*World).Num();
            const bool MinimapUsed = Captures > 0 || !OutMap.Minimap.IsEmpty() || HasIssue(TEXT("Minimap"));
            const bool MinimapCurrent = !OutMap.Minimap.IsEmpty() && !HasIssue(TEXT("Minimap"));
            const URetakeMinimapDefinition* MinimapDefinition = OutMap.Minimap.IsEmpty()?nullptr:LoadObject<URetakeMinimapDefinition>(nullptr,*OutMap.Minimap);
            const bool CustomMinimap = MinimapDefinition && RetakeMinimapBaker::IsCustom(*MinimapDefinition);
            Step(TEXT("minimap"), !MinimapUsed ? TEXT("unused") : Captures != 1 || (CustomMinimap && !MinimapCurrent) ? TEXT("blocked") : MinimapCurrent ? TEXT("current") : TEXT("needed"),
                !MinimapUsed ? TEXT("Optional: set up a generated or custom minimap below.") : Captures != 1 ? TEXT("Keep exactly one Minimap Capture, or remove its unused definition.")
                : CustomMinimap && !MinimapCurrent ? TEXT("Review the custom image and alignment, then Apply custom image. Prepare never replaces it.")
                : MinimapCurrent ? (CustomMinimap?TEXT("Custom image and alignment are current."):TEXT("Generated minimap is current.")) : TEXT("Generate a minimap, or apply a custom image."));
            const bool VisibilityCurrent = OutMap.Visibility == TEXT("Baked") && !HasIssue(TEXT("Visibility"));
            Step(TEXT("visibility"), MapSettings != 1 ? TEXT("blocked") : VisibilityCurrent ? TEXT("current") : TEXT("needed"),
                MapSettings != 1 ? TEXT("Place exactly one Map Settings actor before baking.") : VisibilityCurrent ? TEXT("Geometry preparation and visibility bake are current.") : TEXT("Prepare supported geometry and bake visibility."));
        }
        if (!bValid) Error = FString::Printf(TEXT("map %s failed validation (%d error(s))"), *MapName, Errors);
        return bValid;
    }

    /** What one cooked target of the base game release ships: packages under the allowed foreign roots and /Script module names. */
    struct FShippedTarget
    {
        FString Name;
        TSet<FString> Packages, ScriptModules;
    };

    /**
     * The shipped content of the release the kit was staged from (SDK.lock.json "shipped", copied by modkit.py stage-sdk from the
     * lists packaging writes next to each archive; MKI-030). Every target must ship what a mod references in game.
     */
    bool LoadShippedRelease(FString& OutRelease, TArray<FShippedTarget>& OutTargets, FString& Error)
    {
        FString Text;
        TSharedPtr<FJsonObject> Lock;
        const TSharedPtr<FJsonObject>* Shipped = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
        if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(FPaths::ProjectDir(), TEXT("SDK.lock.json")))
            || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Lock) || !Lock.IsValid()
            || !Lock->TryGetObjectField(TEXT("shipped"), Shipped) || !(*Shipped)->TryGetStringField(TEXT("release"), OutRelease)
            || !(*Shipped)->TryGetArrayField(TEXT("targets"), Targets) || Targets->IsEmpty())
        {
            Error = TEXT("SDK.lock.json has no shipped content of a base game release; run modkit.py stage-sdk");
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Targets)
        {
            const TSharedPtr<FJsonObject>* Target = nullptr;
            const TArray<TSharedPtr<FJsonValue>>* Packages = nullptr;
            const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
            FShippedTarget& Out = OutTargets.AddDefaulted_GetRef();
            if (!Value->TryGetObject(Target) || !(*Target)->TryGetStringField(TEXT("name"), Out.Name)
                || !(*Target)->TryGetArrayField(TEXT("packages"), Packages) || !(*Target)->TryGetArrayField(TEXT("scriptModules"), Modules)
                || Packages->IsEmpty() || Modules->IsEmpty())
            {
                Error = TEXT("SDK.lock.json lists an incomplete shipped target; run modkit.py stage-sdk");
                return false;
            }
            for (const TSharedPtr<FJsonValue>& Package : *Packages) Out.Packages.Add(Package->AsString());
            for (const TSharedPtr<FJsonValue>& Module : *Modules) Out.ScriptModules.Add(Module->AsString());
        }
        return true;
    }

    /**
     * Why a mod package may not reference Dependency, or empty. Besides its own root a mod reaches only /Script and the allowed
     * base roots; a game (cooked) reference there must be shipped by every target of the release. Editor-only references are
     * stripped by the cook. The asset registry records no dependencies on the core script packages (CoreUObject, Engine,
     * BlueprintGraph, UnrealEd), so those never reach this check.
     */
    FString ReferenceProblem(const FString& Dependency, bool bGame, const FString& OwnRoot, const FString& Release, const TArray<FShippedTarget>& Targets)
    {
        if (Dependency.StartsWith(OwnRoot)) return FString();
        const FString ScriptPrefix = TEXT("/Script/");
        const bool bScript = Dependency.StartsWith(ScriptPrefix);
        bool bBaseRoot = false;
        for (const TCHAR* Root : RetakeMapSDK::AllowedForeignRoots) bBaseRoot |= Dependency.StartsWith(Root);
        if (!bScript && !bBaseRoot) return TEXT("outside the mod and the base game roots it may use");
        if (!bGame) return FString();
        for (const FShippedTarget& Target : Targets)
            if (bScript ? !Target.ScriptModules.Contains(Dependency.RightChop(ScriptPrefix.Len())) : !Target.Packages.Contains(Dependency))
                return FString::Printf(TEXT("not shipped by the %s %s build"), *Release, *Target.Name);
        return FString();
    }
}

URetakeMapSDKProfileCommandlet::URetakeMapSDKProfileCommandlet()
{
    IsClient = false; IsEditor = true; IsServer = false; LogToConsole = true;
}

int32 URetakeMapSDKProfileCommandlet::Main(const FString& Params)
{
    FString OutDir;
    FParse::Value(*Params, TEXT("Out="), OutDir);
    if (!OutDir.IsEmpty()) IFileManager::Get().MakeDirectory(*OutDir, true);
    TMap<FString, FString> Hashes;
    if (!ProfileHashes(Hashes, OutDir)) return 1;
    for (const auto& Pair : Hashes)
        UE_LOGF(LogRetakeMapSDK, Display, "event=MapSDKProfile contract=%d oldestReadable=%d platform=%ls hash=%ls",
            RetakeMapSDK::ContractVersion, RetakeMapSDK::OldestReadableContract, *Pair.Key, *Pair.Value);
    return 0;
}

URetakeModManifestCommandlet::URetakeModManifestCommandlet()
{
    IsClient = false; IsEditor = true; IsServer = false; LogToConsole = true;
}

int32 URetakeModManifestCommandlet::Main(const FString& Params)
{
    auto Fail = [](const FString& Reason)
    {
        UE_LOGF(LogRetakeMapSDK, Error, "event=ModManifestFailed reason=\"%ls\"", *Reason);
        return 1;
    };
    FRetakeModManifest Manifest;
    FString Dir, Modes;
    FParse::Value(*Params, TEXT("Mod="), Manifest.ModId);
    FParse::Value(*Params, TEXT("Revision="), Manifest.Revision);
    FParse::Value(*Params, TEXT("Dir="), Dir);
    FParse::Value(*Params, TEXT("Modes="), Modes, false); // Comma-separated: do not stop at the first separator.
    FParse::Value(*Params, TEXT("DisplayName="), Manifest.DisplayName);
    FParse::Value(*Params, TEXT("Author="), Manifest.Author);
    Manifest.SchemaVersion = RetakeMapSDK::ManifestSchemaVersion;
    Manifest.SdkContractVersion = RetakeMapSDK::ContractVersion;
    if (Manifest.DisplayName.IsEmpty()) Manifest.DisplayName = Manifest.ModId;
    if (!FRetakeModManifest::IsValidModId(Manifest.ModId)) return Fail(TEXT("-Mod must be RetakeMod_<Name>"));
    if (!FRetakeModManifest::IsValidRevision(Manifest.Revision)) return Fail(TEXT("-Revision must be 1-32 letters or digits"));
    if (Dir.IsEmpty() || !IFileManager::Get().DirectoryExists(*Dir)) return Fail(TEXT("-Dir must name the export folder"));
    // The mod is a content-only plugin: no code, no configuration, and nothing but its own packages.
    TArray<FAssetData> MapAssets;
    TSet<FName> Packages;
    FString Error;
    const bool bWeapons=RetakeModWorkspace::Kind(Manifest.ModId)==TEXT("weapons");
    if (!FindModContent(Manifest.ModId, MapAssets, Packages, Error,true)) return Fail(Error);
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Manifest.ModId);
    const FPluginDescriptor& Descriptor = Plugin->GetDescriptor();
    if (!Descriptor.Modules.IsEmpty() || !Descriptor.LocalizationTargets.IsEmpty() || !Descriptor.Plugins.IsEmpty()
        || !Descriptor.bCanContainContent || Descriptor.bCanContainVerse)
        return Fail(TEXT("the mod plugin must contain content only: no modules, plugin dependencies or localization targets"));
    if (IFileManager::Get().DirectoryExists(*FPaths::Combine(Plugin->GetBaseDir(), TEXT("Config"))))
        return Fail(TEXT("the mod plugin must not contain a Config folder"));

    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    const FString Root = TEXT("/") + Manifest.ModId + TEXT("/");
    TMap<FSoftObjectPath,FTopLevelAssetPath> RequiredWeaponAssets;
    TArray<FString> WeaponProfiles;
    if(bWeapons)
    {
        FString DraftReport;
        if(!RetakeModWorkspace::CheckWeaponDraft(Manifest.ModId,DraftReport,Error)) return Fail(Error);
        TArray<FAssetData> Assets;Registry.GetAssetsByPath(FName(*Root.LeftChop(1)),Assets,true);
        URetakeWeaponPack* Catalogue=nullptr;
        for(const auto& Asset:Assets) if(Asset.AssetClassPath==URetakeWeaponPack::StaticClass()->GetClassPathName())
        {
            if(Catalogue) return Fail(TEXT("A weapon export requires exactly one catalogue."));
            Catalogue=Cast<URetakeWeaponPack>(Asset.GetAsset());
        }
        if(!Catalogue||Catalogue->PackId!=Manifest.ModId||!Catalogue->Validate(Error)) return Fail(TEXT("Missing or invalid owned weapon catalogue: ")+Error);
        Manifest.Format=TEXT("RetakeWeapons");Manifest.WeaponPack=Catalogue->GetPathName();
        RequiredWeaponAssets.Add(FSoftObjectPath(Catalogue),URetakeWeaponPack::StaticClass()->GetClassPathName());
        for(const auto& Profile:Catalogue->Profiles)
        {
            WeaponProfiles.Add(Profile.ProfileId);
            RequiredWeaponAssets.Add(Profile.PoseContribution,URetakeWeaponPoseContribution::StaticClass()->GetClassPathName());
            for(const auto& Binding:Profile.Weapons)RequiredWeaponAssets.Add(Binding.Value,URetakeWeaponSpecification::StaticClass()->GetClassPathName());
        }
    }
    // Each map offers exactly the modes its content supports (MKD-036); a map that fails validation stops the export.
    for (const FAssetData& Asset : MapAssets)
    {
        FRetakeModMap& Map = Manifest.Maps.AddDefaulted_GetRef();
        Map.MapId = Manifest.ModId + TEXT(".") + Asset.AssetName.ToString();
        Map.PackageName = Asset.PackageName.ToString();
        if (!ResolveMapModes(Asset, Root, Modes, Map, Error)) return Fail(Error);
    }
    FString Release;
    TArray<FShippedTarget> Shipped;
    if (!LoadShippedRelease(Release, Shipped, Error)) return Fail(Error);
    int32 Violations = 0;
    for (FName Package : Packages)
    {
        TArray<FName> Dependencies, GameDependencies;
        Registry.GetDependencies(Package, Dependencies);
        Registry.GetDependencies(Package, GameDependencies, UE::AssetRegistry::EDependencyCategory::Package, UE::AssetRegistry::EDependencyQuery::Game);
        for (FName Dependency : Dependencies)
            if (const FString Problem = ReferenceProblem(Dependency.ToString(), false, Root, Release, {}); !Problem.IsEmpty())
            {
                ++Violations;
                UE_LOGF(LogRetakeMapSDK, Error, "event=ModForeignReference reference=\"%ls -> %ls\" reason=\"%ls\"", *Package.ToString(), *Dependency.ToString(), *Problem);
            }
    }
    if (Violations) return Fail(FString::Printf(TEXT("%d reference(s) leave the mod or are not shipped by Retake %s"), Violations, *Release));

    // Every cooked container under <Dir>/<CookPlatform>/ is listed with its size and BLAKE3.
    TArray<FString> PlatformDirs;
    IFileManager::Get().FindFiles(PlatformDirs, *FPaths::Combine(Dir, TEXT("*")), false, true);
    for (const FString& Platform : PlatformDirs)
    {
        TArray<FString> Files;
        IFileManager::Get().FindFiles(Files, *FPaths::Combine(Dir, Platform, TEXT("*")), true, false);
        Files.Sort();
        for (const FString& File : Files)
        {
            const FString Extension = FPaths::GetExtension(File, false).ToLower();
            if (Extension != TEXT("pak") && Extension != TEXT("utoc") && Extension != TEXT("ucas")) continue;
            FRetakeModArtifact& Artifact = Manifest.Artifacts.AddDefaulted_GetRef();
            Artifact.Platform = Platform;
            Artifact.RelativePath = Platform + TEXT("/") + File;
            Artifact.Hash = FRetakeModManifest::HashFile(FPaths::Combine(Dir, Platform, File), Artifact.Size);
            if (Artifact.Hash.IsEmpty()) return Fail(FString::Printf(TEXT("could not read %s"), *Artifact.RelativePath));
        }
    }
    if (Manifest.Artifacts.IsEmpty()) return Fail(TEXT("no cooked containers were found under the export folder"));

    // Use each target's DEVELOPMENT cooked registry. Source registry intersections reject client-only assets on servers.
    // It retains the cooker dependency graph; the small runtime AssetRegistry.bin does not.
    for (const FString& Platform : PlatformDirs)
    {
        if (!Manifest.Artifacts.ContainsByPredicate([&](const auto& A) { return A.Platform == Platform; })) continue;
        FString RegistryPath, RecordPath, RecordText;
        FParse::Value(*Params, *(TEXT("CookedRegistry") + Platform + TEXT("=")), RegistryPath);
        FParse::Value(*Params, *(TEXT("EngineRecord") + Platform + TEXT("=")), RecordPath);
        FAssetRegistryState Cooked;
        FAssetRegistryLoadOptions LoadOptions;
        if (RegistryPath.IsEmpty() || !FAssetRegistryState::LoadFromDisk(*RegistryPath, LoadOptions, Cooked))
            return Fail(Platform + TEXT(" needs its cooked DevelopmentAssetRegistry.bin"));
        TSharedPtr<FJsonObject> Record;
        if (!FFileHelper::LoadFileToString(RecordText, *RecordPath)
            || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(RecordText), Record) || !Record)
            return Fail(Platform + TEXT(" needs its engine cook record"));
        FRetakeModPlatformCompatibility& C = Manifest.Compatibility.AddDefaulted_GetRef();
        C.Platform = Platform;
        if (!Record->TryGetStringField(TEXT("recordId"), C.EngineRecordId)) return Fail(TEXT("engine record has no identity"));
        C.MaterialPath = TEXT("SdkMaterials");
        C.StructuralProfile = FRetakeMapSDKProfile::StructuralEntries(*FConfigCacheIni::ForPlatform(FName(Platform == TEXT("Windows") ? TEXT("Windows") : TEXT("Linux"))));
        const FShippedTarget* Base = Shipped.FindByPredicate([&](const auto& Target) { return Target.Name == Platform; });
        if (!Base) return Fail(Platform + TEXT(" is missing from the shipped release"));
        // The development registry retains records for editor-only/target-stripped assets.
        // Only actual package-data chunks establish cooked presence; never whitelist editor classes.
        TSet<FIoChunkId> ExportChunks;
        for (const auto& Artifact:Manifest.Artifacts)
        {
            if (Artifact.Platform!=Platform || !Artifact.RelativePath.EndsWith(TEXT(".utoc"))) continue;
            FIoStoreReader Reader;
            if (!Reader.Initialize(FPaths::ChangeExtension(FPaths::Combine(Dir,Artifact.RelativePath),FString()),{}).IsOk())
                return Fail(TEXT("Cannot inspect actual cooked package presence."));
            Reader.EnumerateChunks([&](FIoStoreTocChunkInfo&& Info)
            { if (Info.ChunkType==EIoChunkType::ExportBundleData) ExportChunks.Add(Info.Id);return true; });
        }
        if (ExportChunks.IsEmpty()) return Fail(Platform+TEXT(" contains no cooked package data."));
        TArray<FAssetData> CookedAssets;
        Cooked.GetAllAssets({}, CookedAssets, true);
        int32 AbsentRegistryRows=0;
        TSet<FName> CookedPackages;
        for (const FAssetData& Asset : CookedAssets)
        {
            if (!Asset.PackageName.ToString().StartsWith(Root)) continue;
            if (!ExportChunks.Contains(CreatePackageDataChunkId(FPackageId::FromName(Asset.PackageName))))
            { ++AbsentRegistryRows;continue; }
            if(bWeapons&&Asset.AssetClassPath==UWorld::StaticClass()->GetClassPathName())return Fail(TEXT("A cooked weapon pack contains a world."));
            CookedPackages.Add(Asset.PackageName);
            C.NativeClasses.AddUnique(Asset.AssetClassPath.ToString());
            // Static switches/base overrides make their own shader permutations, even on an SDK parent.
            if (Asset.AssetClassPath == UMaterial::StaticClass()->GetClassPathName()) C.MaterialPath = TEXT("CustomShaders");
            if (UMaterialInstance* Instance = Cast<UMaterialInstance>(Asset.GetAsset()))
                if (Instance->bHasStaticPermutationResource || !Instance->GetMaterial()->GetPathName().StartsWith(TEXT("/RetakeMapContent/")))
                    C.MaterialPath = TEXT("CustomShaders");
        }
        UE_LOGF(LogRetakeMapSDK,Display,"event=ModCookRegistryFiltered platform=%ls cooked_packages=%d absent_registry_rows=%d",
            *Platform,CookedPackages.Num(),AbsentRegistryRows);
        for(const auto& Required:RequiredWeaponAssets)
            if(!CookedAssets.ContainsByPredicate([&](const FAssetData& Asset)
                {return CookedPackages.Contains(Asset.PackageName)&&Asset.GetSoftObjectPath()==Required.Key&&Asset.AssetClassPath==Required.Value;}))
                return Fail(Platform+TEXT(" is missing the required native weapon asset: ")+Required.Key.ToString());
        for (FName Package : CookedPackages)
        {
            TArray<FAssetIdentifier> Dependencies;
            Cooked.GetDependencies(FAssetIdentifier(Package), Dependencies, UE::AssetRegistry::EDependencyCategory::Package,
                UE::AssetRegistry::EDependencyQuery::Game);
            for (const FAssetIdentifier& Dependency : Dependencies)
            {
                const FString Name = Dependency.PackageName.ToString();
                if(bWeapons&&Name.StartsWith(Root)&&!CookedPackages.Contains(Dependency.PackageName))
                    return Fail(Platform+TEXT(" is missing a cooked internal weapon dependency: ")+Name);
                if (const FString Problem = ReferenceProblem(Name, true, Root, Release, { *Base }); !Problem.IsEmpty()) return Fail(Name + TEXT(": ") + Problem);
                if (!Name.StartsWith(Root) && !Name.StartsWith(TEXT("/Script/"))) C.ForeignPackages.AddUnique(Name);
            }
        }
        // Loading an editor world creates transient subsystems and editor graphs that are absent from its cook.
        // Resolve only class/default-object imports actually present in this platform's cooked package headers.
        TMap<uint64, FString> NativeClassImports;
        for (TObjectIterator<UClass> It; It; ++It)
        {
            const FString ClassPath = It->GetPathName();
            if (!It->HasAnyClassFlags(CLASS_Native) || !ClassPath.StartsWith(TEXT("/Script/"))) continue;
            NativeClassImports.Add(FPackageObjectIndex::FromScriptPath(ClassPath).Value(), ClassPath);
            if (const UObject* Default = It->GetDefaultObject(false))
                NativeClassImports.Add(FPackageObjectIndex::FromScriptPath(Default->GetPathName()).Value(), ClassPath);
        }
        for (const FRetakeModArtifact& Artifact : Manifest.Artifacts)
        {
            if (Artifact.Platform != Platform || !Artifact.RelativePath.EndsWith(TEXT(".utoc"))) continue;
            FIoStoreReader Reader;
            if (!Reader.Initialize(FPaths::ChangeExtension(FPaths::Combine(Dir, Artifact.RelativePath), FString()), {}).IsOk()) return Fail(TEXT("cannot inspect cooked shader chunks"));
            bool bHeadersValid = true;
            Reader.EnumerateChunks([&](FIoStoreTocChunkInfo&& Info)
            {
                if (Info.ChunkType == EIoChunkType::ShaderCode) C.MaterialPath = TEXT("CustomShaders");
                if (Info.ChunkType != EIoChunkType::ExportBundleData) return true;
                FIoReadOptions Options;
                Options.SetRange(0, sizeof(FZenPackageSummary));
                auto Prefix = Reader.Read(Info.Id, Options);
                if (!Prefix.IsOk() || Prefix.ValueOrDie().GetSize() != sizeof(FZenPackageSummary))
                    { Error = TEXT("cannot inspect cooked package summary"); bHeadersValid = false; return false; }
                FZenPackageSummary Summary;
                FMemory::Memcpy(&Summary, Prefix.ValueOrDie().Data(), sizeof(Summary));
                if (Summary.HeaderSize < sizeof(Summary) || Summary.HeaderSize > 4 * 1024 * 1024 || Summary.HeaderSize > Info.Size)
                    { Error = TEXT("invalid cooked package header bounds"); bHeadersValid = false; return false; }
                Options.SetRange(0, Summary.HeaderSize);
                auto Header = Reader.Read(Info.Id, Options);
                FRetakeModPackageVersions Versions;
                if (!Header.IsOk() || !FRetakeMapCompatibility::InspectPackage(MakeArrayView(
                    static_cast<const uint8*>(Header.ValueOrDie().Data()), static_cast<int32>(Header.ValueOrDie().GetSize())), Versions, Error))
                    { bHeadersValid = false; return false; }
                const int64 ImportBytes = static_cast<int64>(Summary.ExportMapOffset) - Summary.ImportMapOffset;
                const int64 ExportBytes = static_cast<int64>(Summary.ExportBundleEntriesOffset) - Summary.ExportMapOffset;
                if (Summary.ImportMapOffset < static_cast<int32>(sizeof(Summary)) || ImportBytes < 0 || ExportBytes < 0
                    || Summary.ExportBundleEntriesOffset > static_cast<int64>(Summary.HeaderSize)
                    || ImportBytes % sizeof(FPackageObjectIndex) != 0 || ExportBytes % sizeof(FExportMapEntry) != 0)
                    { Error = TEXT("invalid cooked class import/export table bounds"); bHeadersValid = false; return false; }
                const uint8* Bytes = static_cast<const uint8*>(Header.ValueOrDie().Data());
                for (int64 Offset = Summary.ImportMapOffset; Offset < Summary.ExportMapOffset; Offset += sizeof(FPackageObjectIndex))
                {
                    FPackageObjectIndex Index;
                    FMemory::Memcpy(&Index, Bytes + Offset, sizeof(Index));
                    if (Index.IsScriptImport())
                        if (const FString* ClassPath = NativeClassImports.Find(Index.Value())) C.NativeClasses.AddUnique(*ClassPath);
                }
                for (int64 Offset = Summary.ExportMapOffset; Offset < Summary.ExportBundleEntriesOffset; Offset += sizeof(FExportMapEntry))
                {
                    FExportMapEntry Export;
                    FMemory::Memcpy(&Export, Bytes + Offset, sizeof(Export));
                    if (!Export.ClassIndex.IsScriptImport()) continue; // Package-local classes are covered by their native imports.
                    const FString* ClassPath = NativeClassImports.Find(Export.ClassIndex.Value());
                    if (!ClassPath)
                    {
                        Error = FString::Printf(TEXT("cannot resolve cooked native class import %llu in %s"),
                            Export.ClassIndex.Value(), *Info.FileName);
                        bHeadersValid = false;
                        return false;
                    }
                    C.NativeClasses.AddUnique(*ClassPath);
                }
                if (Versions.UE4 != Record->GetIntegerField(TEXT("packageUE4")) || Versions.UE5 != Record->GetIntegerField(TEXT("packageUE5"))
                    || Versions.Licensee != Record->GetIntegerField(TEXT("licenseeVersion")))
                    { Error = TEXT("the cook output does not match its engine record"); bHeadersValid = false; return false; }
                for (const auto& Version : Versions.CustomVersions)
                {
                    if (const int32* Existing = C.CustomVersions.Find(Version.Key); Existing && *Existing != Version.Value)
                        { Error = TEXT("the cook contains mixed custom-version values"); bHeadersValid = false; return false; }
                    C.CustomVersions.Add(Version.Key, Version.Value);
                }
                return true;
            });
            if (!bHeadersValid) return Fail(Error);
        }
        for (const FString& ClassPath : C.NativeClasses)
        {
            if(bWeapons&&ClassPath.StartsWith(TEXT("/Script/RetakeMapSDKEditor.")))
                return Fail(TEXT("Editor-only weapon preparation data escaped into the cooked output: ")+ClassPath);
            if(bWeapons&&ClassPath.StartsWith(TEXT("/Script/Retake"))
                &&!ClassPath.StartsWith(TEXT("/Script/RetakeMapSDK."))&&!ClassPath.StartsWith(TEXT("/Script/RetakeDestruction.")))
                return Fail(TEXT("Weapon output references a private or editor implementation class: ")+ClassPath);
            FString Module, ClassName;
            if (!ClassPath.StartsWith(TEXT("/Script/")) || !ClassPath.Mid(8).Split(TEXT("."), &Module, &ClassName)
                || !Base->ScriptModules.Contains(Module))
                return Fail(FString::Printf(TEXT("%s cook requires native class %s, which the selected game does not ship"), *Platform, *ClassPath));
        }
        C.ForeignPackages.Sort(); C.NativeClasses.Sort();
        C.CustomVersions.KeySort(TLess<FString>());
    }

    if (!Manifest.Validate(Error)) return Fail(Error);
    const FString ManifestPath=FPaths::Combine(Dir,TEXT("manifest.json"));
    if (!Manifest.SaveToFile(ManifestPath, Error)) return Fail(Error);
    if(bWeapons)
    {
        // Optional launcher hints use the native digest. Runtime admission still derives its
        // own identity and profile set from the verified manifest and cooked catalogue.
        FString Json;TSharedPtr<FJsonObject> Object;
        if(!FFileHelper::LoadFileToString(Json,*ManifestPath)
            || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json),Object) || !Object)
            return Fail(TEXT("Could not read the generated weapon manifest for launch metadata."));
        auto Launch=MakeShared<FJsonObject>();
        Launch->SetStringField(TEXT("contentDigest"),Manifest.GetContentDigest());
        WeaponProfiles.Sort();TArray<TSharedPtr<FJsonValue>> Profiles;
        for(const auto& Profile:WeaponProfiles)Profiles.Add(MakeShared<FJsonValueString>(Profile));
        Launch->SetArrayField(TEXT("weaponProfiles"),Profiles);
        Object->SetObjectField(TEXT("testLaunch"),Launch);
        Json.Reset();
        if(!FJsonSerializer::Serialize(Object.ToSharedRef(),TJsonWriterFactory<>::Create(&Json))
            || !FFileHelper::SaveStringToFile(Json,*ManifestPath,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
            return Fail(TEXT("Could not save weapon launch metadata."));
    }
    UE_LOGF(LogRetakeMapSDK, Display, "event=ModManifestWritten mod=%ls revision=%ls format=%ls maps=%d artifacts=%d",
        *Manifest.ModId, *Manifest.Revision,*Manifest.Format, Manifest.Maps.Num(), Manifest.Artifacts.Num());
    return 0;
}

URetakeMapValidateCommandlet::URetakeMapValidateCommandlet()
{
    IsClient = false; IsEditor = true; IsServer = false; LogToConsole = true;
}

int32 URetakeMapValidateCommandlet::Main(const FString& Params)
{
    FString ModId, Modes, Error, ReportFile;
    FParse::Value(*Params, TEXT("Mod="), ModId);
    FParse::Value(*Params, TEXT("Modes="), Modes, false); // Lists and per-map specs contain separators.
    FParse::Value(*Params, TEXT("Report="), ReportFile);
    if (!FRetakeModManifest::IsValidModId(ModId)) { UE_LOGF(LogRetakeMapSDK, Error, "event=MapValidateFailed reason=\"-Mod must be RetakeMod_<Name>\""); return 1; }
    if(FParse::Param(*Params,TEXT("AuthoringDraft"))||RetakeModWorkspace::Kind(ModId)==TEXT("weapons"))
    {
        FString Json;const bool Valid=RetakeModWorkspace::CheckWeaponDraft(ModId,Json,Error);
        if(!Json.IsEmpty()&&!ReportFile.IsEmpty())
        {IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReportFile),true);if(!FFileHelper::SaveStringToFile(Json,*ReportFile,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))return 1;}
        if(!Valid)UE_LOGF(LogRetakeMapSDK,Error,"event=WeaponDraftCheckFailed reason=\"%ls\"",*Error);
        return Valid?0:1;
    }
    TArray<FAssetData> Maps;
    TSet<FName> Packages;
    if (!FindModContent(ModId, Maps, Packages, Error)) { UE_LOGF(LogRetakeMapSDK, Error, "event=MapValidateFailed reason=\"%ls\"", *Error); return 1; }
    int32 Failed = 0;
    TArray<TSharedPtr<FJsonValue>> MapReports, ContentIssues;
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    for (FName Package : Packages)
    {
        TArray<FName> Dependencies;
        Registry.GetDependencies(Package, Dependencies);
        for (FName Dependency : Dependencies)
        {
            const FString Path = Dependency.ToString();
            FString Problem = ReferenceProblem(Path, false, TEXT("/") + ModId + TEXT("/"), FString(), {});
            if (Path == TEXT("/Script/Retake") || Path == TEXT("/Script/RetakeEditor"))
                Problem = TEXT("private Retake gameplay/editor code cannot be used by a map mod");
            if (Problem.IsEmpty()) continue;
            auto Issue = MakeShared<FJsonObject>();
            Issue->SetBoolField(TEXT("error"), true); Issue->SetStringField(TEXT("code"), TEXT("Dependencies"));
            Issue->SetStringField(TEXT("actor"), Package.ToString());
            Issue->SetStringField(TEXT("detail"), FString::Printf(TEXT("%s -> %s: %s"), *Package.ToString(), *Path, *Problem));
            ContentIssues.Add(MakeShared<FJsonValueObject>(Issue));
        }
    }
    for (const FAssetData& Map : Maps)
    {
        FRetakeModMap Entry;
        TArray<FRetakeMapIssue> Issues;
        TArray<TSharedPtr<FJsonValue>> Preparation;
        const bool bValid = ResolveMapModes(Map, TEXT("/") + ModId + TEXT("/"), Modes, Entry, Error, &Issues, &Preparation);
        if (!bValid) { ++Failed; UE_LOGF(LogRetakeMapSDK, Error, "event=MapValidateFailed reason=\"%ls\"", *Error); }
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Map.AssetName.ToString());
        Row->SetStringField(TEXT("package"), Map.PackageName.ToString());
        Row->SetStringField(TEXT("displayName"), Entry.DisplayName);
        Row->SetBoolField(TEXT("valid"), bValid);
        Row->SetStringField(TEXT("visibility"), Entry.Visibility);
        Row->SetStringField(TEXT("acoustics"), Entry.Acoustics);
        Row->SetStringField(TEXT("minimap"), Entry.Minimap);
        Row->SetStringField(TEXT("ambience"), Entry.Ambience);
        TArray<TSharedPtr<FJsonValue>> ModeRows, IssueRows;
        for (const FString& Mode : Entry.SupportedModes) ModeRows.Add(MakeShared<FJsonValueString>(Mode));
        for (const FRetakeMapIssue& Issue : Issues)
        {
            auto Item = MakeShared<FJsonObject>();
            Item->SetBoolField(TEXT("error"), Issue.bError);
            Item->SetStringField(TEXT("code"), Issue.Code);
            Item->SetStringField(TEXT("actor"), Issue.Actor);
            Item->SetStringField(TEXT("detail"), Issue.Detail);
            IssueRows.Add(MakeShared<FJsonValueObject>(Item));
        }
        if (!bValid && Issues.IsEmpty())
        {
            auto Item = MakeShared<FJsonObject>();
            Item->SetBoolField(TEXT("error"), true); Item->SetStringField(TEXT("code"), TEXT("Map"));
            Item->SetStringField(TEXT("actor"), TEXT("")); Item->SetStringField(TEXT("detail"), Error);
            IssueRows.Add(MakeShared<FJsonValueObject>(Item));
        }
        Row->SetArrayField(TEXT("modes"), ModeRows); Row->SetArrayField(TEXT("issues"), IssueRows);
        if (Preparation.IsEmpty() && !bValid)
        {
            // A failed map load/mode selection is an authoring failure, not an outdated-tool protocol.
            for (const TCHAR* Id : {TEXT("acoustics"), TEXT("minimap"), TEXT("visibility")})
            {
                auto Step = MakeShared<FJsonObject>();
                Step->SetStringField(TEXT("id"), Id); Step->SetStringField(TEXT("state"), TEXT("blocked"));
                Step->SetStringField(TEXT("detail"), Error); Preparation.Add(MakeShared<FJsonValueObject>(Step));
            }
        }
        Row->SetArrayField(TEXT("preparation"), Preparation);
        MapReports.Add(MakeShared<FJsonValueObject>(Row));
    }
    if (!ReportFile.IsEmpty())
    {
        auto Report = MakeShared<FJsonObject>();
        Report->SetNumberField(TEXT("format"), 1); Report->SetStringField(TEXT("modId"), ModId);
        Report->SetBoolField(TEXT("complete"), true); Report->SetBoolField(TEXT("valid"), Failed == 0 && ContentIssues.IsEmpty() && !Maps.IsEmpty());
        Report->SetArrayField(TEXT("maps"), MapReports);
        Report->SetArrayField(TEXT("issues"), ContentIssues);
        FString JSON;
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReportFile), true);
        if (!FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&JSON))
            || !FFileHelper::SaveStringToFile(JSON, *ReportFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        { UE_LOGF(LogRetakeMapSDK, Error, "event=MapValidateFailed reason=\"Cannot write validation report\""); return 1; }
    }
    UE_LOGF(LogRetakeMapSDK, Display, "event=MapValidateDone mod=%ls maps=%d failed=%d", *ModId, Maps.Num(), Failed);
    return Failed == 0 && ContentIssues.IsEmpty() && !Maps.IsEmpty() ? 0 : 1;
}

URetakeMinimapBakeCommandlet::URetakeMinimapBakeCommandlet()
{
    IsClient = false; IsEditor = true; IsServer = false; LogToConsole = true;
}

URetakeAcousticsBakeCommandlet::URetakeAcousticsBakeCommandlet()
{
    IsClient = false; IsEditor = true; IsServer = false; LogToConsole = true;
}

URetakeVisibilityBakeCommandlet::URetakeVisibilityBakeCommandlet()
{
    IsClient = false; IsEditor = true; IsServer = false; LogToConsole = true;
}

URetakeModCreateCommandlet::URetakeModCreateCommandlet()
{
    IsClient = false; IsEditor = true; IsServer = false; LogToConsole = true;
}

int32 URetakeModCreateCommandlet::Main(const FString& Params)
{
    FString Name, ModId, Error, Kind=TEXT("map");
    FParse::Value(*Params,TEXT("Kind="),Kind);
    FParse::Value(*Params, TEXT("Name="), Name);
    if (!RetakeModWorkspace::Create(Name, FParse::Param(*Params, TEXT("Basic")), ModId, Error, Kind))
    { UE_LOGF(LogRetakeMapSDK, Error, "event=ModWorkspaceCreateFailed reason=\"%ls\"", *Error); return 1; }
    UE_LOGF(LogRetakeMapSDK, Display, "event=ModWorkspaceCreated mod=%ls template=%ls", *ModId,
        FParse::Param(*Params, TEXT("Basic")) ? TEXT("Basic") : TEXT("Empty"));
    return 0;
}

int32 URetakeVisibilityBakeCommandlet::Main(const FString& Params)
{
    FString ModId, MapName, Phase, File, Error;
    FParse::Value(*Params, TEXT("Mod="), ModId);
    FParse::Value(*Params, TEXT("Map="), MapName);
    FParse::Value(*Params, TEXT("Phase="), Phase);
    FParse::Value(*Params, TEXT("File="), File);
    const auto Fail = [](const FString& Reason)
    { UE_LOGF(LogRetakeMapSDK, Error, "event=VisibilityBakeFailed reason=\"%ls\"", *Reason); return 1; };
    if (!FRetakeModManifest::IsValidModId(ModId) || MapName.IsEmpty()
        || (Phase != TEXT("Prepare") && Phase != TEXT("Export") && Phase != TEXT("Import"))
        || (Phase != TEXT("Prepare") && File.IsEmpty()))
        return Fail(TEXT("Specify -Mod, -Map and -Phase=Prepare|Export|Import; Export/Import also require -File"));
    TArray<FAssetData> Maps;
    TSet<FName> Packages;
    if (!FindModContent(ModId, Maps, Packages, Error)) return Fail(Error);
    const FAssetData* Map = Maps.FindByPredicate([&](const FAssetData& Asset) { return Asset.AssetName.ToString() == MapName; });
    if (!Map) return Fail(TEXT("No matching playable map in the selected mod"));
    UWorld* World = UEditorLoadingAndSavingUtils::LoadMap(Map->PackageName.ToString());
    if (!World || !World->PersistentLevel) return Fail(TEXT("Cannot load the selected map"));
    FAssetCompilingManager::Get().FinishAllCompilation();
    ARetakeMapSettings* Settings = nullptr;
    for (AActor* Actor : World->PersistentLevel->Actors)
        if (auto* Candidate = Cast<ARetakeMapSettings>(Actor))
        {
            if (Settings) return Fail(TEXT("Place exactly one Retake Map Settings actor"));
            Settings = Candidate;
        }
    if (!Settings) return Fail(TEXT("Place a Retake Map Settings actor before baking"));
    const FString SceneJSON = URetakeVisibilityData::ExportScene(World);
    TSharedPtr<FJsonObject> Scene;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SceneJSON), Scene) || !Scene)
        return Fail(TEXT("Cannot encode visibility geometry"));
    if (Scene->TryGetStringField(TEXT("error"), Error)) return Fail(Error);
    if (Phase == TEXT("Prepare"))
    {
        // Reuse the exporter's actual eligibility decisions. Do not introduce another Nanite/LOD classifier,
        // change unsupported geometry, or silently modify engine/shared content to make a map bake.
        const TSharedPtr<FJsonObject>* Diagnostics = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
        if (!Scene->TryGetObjectField(TEXT("diagnostics"), Diagnostics) || !Diagnostics || !Diagnostics->IsValid()
            || !(*Diagnostics)->TryGetArrayField(TEXT("components"), Components))
            return Fail(TEXT("Visibility export did not provide geometry eligibility diagnostics"));
        const FString ModRoot = TEXT("/") + ModId + TEXT("/");
        TSet<UStaticMesh*> Meshes;
        int32 ForeignComponents = 0;
        for (const TSharedPtr<FJsonValue>& Value : *Components)
        {
            const TSharedPtr<FJsonObject> Component = Value.IsValid() ? Value->AsObject() : nullptr;
            bool bCandidate = false, bCPUAccess = false;
            FString MeshPath;
            if (!Component || !Component->TryGetBoolField(TEXT("proof_candidate"), bCandidate)
                || !Component->TryGetBoolField(TEXT("cpu_access"), bCPUAccess)
                || !Component->TryGetStringField(TEXT("mesh"), MeshPath))
                return Fail(TEXT("Invalid visibility geometry eligibility diagnostics"));
            if (!bCandidate || bCPUAccess) continue;
            if (!MeshPath.StartsWith(ModRoot)) { ++ForeignComponents; continue; }
            UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
            if (!Mesh || !Mesh->GetOutermost()->GetName().StartsWith(ModRoot))
                return Fail(FString::Printf(TEXT("Cannot prepare mod-owned mesh %s"), *MeshPath));
            Meshes.Add(Mesh);
        }
        FProperty* CPUAccess = FindFProperty<FProperty>(UStaticMesh::StaticClass(), GET_MEMBER_NAME_CHECKED(UStaticMesh, bAllowCPUAccess));
        if (!CPUAccess) return Fail(TEXT("The engine does not expose static-mesh CPU access preparation"));
        TArray<UPackage*> ChangedPackages;
        for (UStaticMesh* Mesh : Meshes)
        {
            Mesh->Modify();
            Mesh->bAllowCPUAccess = true;
            FPropertyChangedEvent Changed(CPUAccess);
            Mesh->PostEditChangeProperty(Changed);
            Mesh->MarkPackageDirty();
            ChangedPackages.AddUnique(Mesh->GetOutermost());
        }
        FAssetCompilingManager::Get().FinishAllCompilation();
        if (!ChangedPackages.IsEmpty() && !UEditorLoadingAndSavingUtils::SavePackages(ChangedPackages, false))
            return Fail(TEXT("Could not save prepared geometry; inspect the reported mesh packages before retrying"));
        UE_LOGF(LogRetakeMapSDK, Display, "event=VisibilityGeometryPrepared map=%ls meshes=%d foreignComponentsUnchanged=%d",
            *Map->PackageName.ToString(), Meshes.Num(), ForeignComponents);
        return 0;
    }
    if (Phase == TEXT("Export"))
    {
        const TArray<TSharedPtr<FJsonValue>>* Polygons = nullptr;
        if (!Scene->TryGetArrayField(TEXT("polygons"), Polygons) || Polygons->IsEmpty())
            return Fail(TEXT("No certified opaque geometry: run geometry preparation, then check CPU access, materials and supported mesh settings. No bake was written."));
        if (!FFileHelper::SaveStringToFile(SceneJSON, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
            return Fail(TEXT("Cannot write visibility scene JSON"));
        UE_LOGF(LogRetakeMapSDK, Display, "event=VisibilitySceneExported map=%ls file=%ls", *Map->PackageName.ToString(), *File);
        return 0;
    }
    FString JSON;
    if (!FFileHelper::LoadFileToString(JSON, *File)) return Fail(TEXT("Cannot read visibility bake JSON"));
    URetakeVisibilityData* Candidate = NewObject<URetakeVisibilityData>();
    if (!Candidate->ImportBakedJSON(JSON, Error)) return Fail(Error);
    FString Geometry, Source;
    if (!Scene->TryGetStringField(TEXT("fingerprint"), Geometry) || !Scene->TryGetStringField(TEXT("source_fingerprint"), Source)
        || !Candidate->Validate(Map->PackageName.ToString(), Geometry, Error) || Candidate->SourceFingerprint != Source)
        return Fail(TEXT("Bake does not match the saved map's current geometry and source identity; export and bake again"));
    const FString Root = TEXT("/") + ModId + TEXT("/");
    URetakeVisibilityData* Bake = Settings->VisibilityBake;
    if (Bake && !Bake->GetOutermost()->GetName().StartsWith(Root))
        return Fail(TEXT("Existing visibility asset is outside this mod"));
    if (!Bake)
    {
        const FString ObjectPath = URetakeVisibilityData::AutomaticAssetPath(Map->PackageName.ToString(), Root + TEXT("Visibility"));
        const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
        // Do not overwrite an unrelated package merely because the generated name matches.
        if (FPackageName::DoesPackageExist(PackageName)) return Fail(TEXT("Generated visibility package already exists; assign it explicitly before rebaking"));
        UPackage* Package = CreatePackage(*PackageName);
        Bake = NewObject<URetakeVisibilityData>(Package, *FPackageName::GetShortName(PackageName), RF_Public | RF_Standalone);
        FAssetRegistryModule::AssetCreated(Bake);
    }
    if (!Bake->ImportBakedJSON(JSON, Error)) return Fail(Error);
    Settings->Modify();
    Settings->VisibilityBake = Bake;
    World->MarkPackageDirty();
    if (!UEditorLoadingAndSavingUtils::SavePackages({ Bake->GetOutermost(), World->GetOutermost() }, false))
        return Fail(TEXT("Could not save visibility preparation and map binding"));
    UE_LOGF(LogRetakeMapSDK, Display, "event=VisibilityBakeImported map=%ls bake=%ls source=%ls geometry=%ls",
        *Map->PackageName.ToString(), *Bake->GetPathName(), *Source, *Geometry);
    return 0;
}

int32 URetakeAcousticsBakeCommandlet::Main(const FString& Params)
{
    FString ModId, MapName, Error;
    FParse::Value(*Params, TEXT("Mod="), ModId);
    FParse::Value(*Params, TEXT("Map="), MapName);
    const auto Fail = [](const FString& Reason)
    { UE_LOGF(LogRetakeMapSDK, Error, "event=AcousticsBakeFailed reason=\"%ls\"", *Reason); return 1; };
    if (!FRetakeModManifest::IsValidModId(ModId)) return Fail(TEXT("-Mod must name a mod workspace"));
    TArray<FAssetData> Maps;
    TSet<FName> Packages;
    if (!FindModContent(ModId, Maps, Packages, Error)) return Fail(Error);
    const FString Root = TEXT("/") + ModId + TEXT("/");
    int32 Baked = 0;
    for (const FAssetData& Map : Maps)
    {
        if (!MapName.IsEmpty() && Map.AssetName.ToString() != MapName) continue;
        UWorld* World = UEditorLoadingAndSavingUtils::LoadMap(Map.PackageName.ToString());
        if (!World || !World->PersistentLevel) return Fail(TEXT("could not load the map"));
        ARetakeAcousticMap* AcousticMap = nullptr;
        for (AActor* Actor : World->PersistentLevel->Actors)
            if (auto* Candidate = Cast<ARetakeAcousticMap>(Actor))
            {
                if (AcousticMap) return Fail(TEXT("place exactly one Retake Acoustic Map actor"));
                AcousticMap = Candidate;
            }
        if (!AcousticMap) return Fail(TEXT("place a Retake Acoustic Map and author its regions before baking"));
        if (!AcousticMap->Data)
        {
            const FString Name = TEXT("DA_") + Map.AssetName.ToString() + TEXT("_Acoustics");
            UPackage* Package = CreatePackage(*(Root + TEXT("Audio/") + Name));
            UObject* Existing = FindObject<UObject>(Package, *Name);
            if (Existing && !Existing->IsA<URetakeAcousticMapData>()) return Fail(TEXT("the acoustic asset path is occupied by another asset type"));
            AcousticMap->Modify();
            AcousticMap->Data = Existing ? CastChecked<URetakeAcousticMapData>(Existing)
                : NewObject<URetakeAcousticMapData>(Package, *Name, RF_Public | RF_Standalone);
            if (!Existing) FAssetRegistryModule::AssetCreated(AcousticMap->Data);
            AcousticMap->MarkPackageDirty();
        }
        if (!AcousticMap->Data->GetOutermost()->GetName().StartsWith(Root)) return Fail(TEXT("the acoustic bake must be inside this mod"));
        AcousticMap->Bake();
        if (!AcousticMap->ValidateBake(Error)) return Fail(Error);
        UPackage* Package = AcousticMap->Data->GetOutermost();
        const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
        FSavePackageArgs Save;
        Save.TopLevelFlags = RF_Public | RF_Standalone;
        Save.SaveFlags = SAVE_NoError;
        if (!UPackage::SavePackage(Package, AcousticMap->Data, *Filename, Save)
            || !UEditorLoadingAndSavingUtils::SaveMap(World, Map.PackageName.ToString())) return Fail(TEXT("could not save the acoustic bake and map"));
        ++Baked;
    }
    if (!Baked) return Fail(TEXT("no matching mod map was found"));
    UE_LOGF(LogRetakeMapSDK, Display, "event=AcousticsBakeDone mod=%ls maps=%d", *ModId, Baked);
    return 0;
}

int32 URetakeMinimapBakeCommandlet::Main(const FString& Params)
{
    FString ModId, MapName, Error, CustomTexture;
    FParse::Value(*Params, TEXT("CustomTexture="), CustomTexture);
    const bool ReplaceCustom=FParse::Param(*Params,TEXT("ReplaceCustom"));
    FParse::Value(*Params, TEXT("Mod="), ModId);
    FParse::Value(*Params, TEXT("Map="), MapName);
    auto Fail = [](const FString& Reason)
    {
        UE_LOGF(LogRetakeMapSDK, Error, "event=MinimapBakeFailed reason=\"%ls\"", *Reason);
        return 1;
    };
    if (!FRetakeModManifest::IsValidModId(ModId)) return Fail(TEXT("-Mod must be RetakeMod_<Name>"));
    TArray<FAssetData> Maps;
    TSet<FName> Packages;
    if (!FindModContent(ModId, Maps, Packages, Error)) return Fail(Error);
    if (!MapName.IsEmpty())
    {
        Maps.RemoveAll([&MapName](const FAssetData& Map) { return !Map.AssetName.ToString().Equals(MapName, ESearchCase::IgnoreCase); });
        if (Maps.IsEmpty()) return Fail(FString::Printf(TEXT("%s has no map %s"), *ModId, *MapName));
    }
    if(!CustomTexture.IsEmpty() && MapName.IsEmpty()) return Fail(TEXT("Select one -Map when applying custom minimap artwork"));
    int32 Baked = 0;
    for (const FAssetData& Map : Maps)
    {
        const FString Package = Map.PackageName.ToString();
        UWorld* World = UEditorLoadingAndSavingUtils::LoadMap(Package);
        if (!World) return Fail(FString::Printf(TEXT("could not load %s"), *Package));
        const TArray<const ARetakeMinimapCapture*> Captures = RetakeMinimapBaker::FindCaptures(*World);
        if (Captures.IsEmpty() && MapName.IsEmpty())
        {
            UE_LOGF(LogRetakeMapSDK, Display, "event=MinimapBakeSkipped map=%ls reason=\"no Retake Minimap Capture\"", *Map.AssetName.ToString());
            continue;
        }
        if (Captures.Num() != 1) return Fail(FString::Printf(TEXT("%s needs exactly one Retake Minimap Capture; it has %d"), *Package, Captures.Num()));
        // The minimap caption is the map's display name, set on its Retake Map Settings.
        const ARetakeMapSettings* Settings = nullptr;
        for (const AActor* Actor : World->PersistentLevel->Actors)
            if (const ARetakeMapSettings* Found = Cast<ARetakeMapSettings>(Actor)) Settings = Found;
        if (!Settings || !FRetakeModManifest::IsValidDisplayName(Settings->DisplayName))
            return Fail(FString::Printf(TEXT("%s needs a Retake Map Settings actor with a Display Name; the minimap shows it"), *Package));
        FString Definition;
        const FString ModRoot=TEXT("/")+ModId+TEXT("/");
        bool Succeeded=false;
        if(!CustomTexture.IsEmpty())
        {
            if(!CustomTexture.StartsWith(ModRoot))return Fail(TEXT("Custom image must belong to this mod"));
            UTexture2D* Texture=LoadObject<UTexture2D>(nullptr,*CustomTexture);
            if(!Texture)return Fail(TEXT("The selected custom image is not a Texture2D asset"));
            Succeeded=RetakeMinimapBaker::ApplyCustom(*World,*Captures[0],*Texture,ModRoot,FText::FromString(Settings->DisplayName),Definition,Error);
        }
        else Succeeded=RetakeMinimapBaker::Bake(*World,*Captures[0],ModRoot,FText::FromString(Settings->DisplayName),Definition,Error,ReplaceCustom);
        if(!Succeeded)return Fail(FString::Printf(TEXT("%s: %s"),*Package,*Error));
        ++Baked;
    }
    UE_LOGF(LogRetakeMapSDK, Display, "event=MinimapBakeDone mod=%ls maps=%d baked=%d", *ModId, Maps.Num(), Baked);
    return 0;
}
