// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeModKitState.h"
#include "RetakeModWorkspace.h"
#include "RetakeMapSettings.h"
#include "RetakeMinimapCapture.h"
#include "RetakeMinimapDefinition.h"
#include "RetakeMinimapBaker.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Texture2D.h"
#include "Misc/PackageName.h"
#include "Selection.h"
#include "RetakeModePlayerStart.h"
#include "RetakeModeObjectiveMarker.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "AssetCompilingManager.h"
#include "Engine/HitResult.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/DataAssetFactory.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "LevelEditorViewport.h"
#include "FileHelpers.h"
#include "PackageTools.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/DataAsset.h"
#include "Components/CapsuleComponent.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace
{
FString Kit() { return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()); }
FString Preferences() { return Kit() / TEXT("Saved/ModKit/creator-preferences.json"); }
TSharedPtr<FJsonObject> ReadJSON(const FString& Path)
{
    FString Text; TSharedPtr<FJsonObject> Value;
    if (FFileHelper::LoadFileToString(Text, *Path)) FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Value);
    return Value;
}
FString TextField(const TSharedPtr<FJsonObject>& Value, const TCHAR* Name)
{ FString Text; if (Value) Value->TryGetStringField(Name, Text); return Text; }
bool Flag(const TSharedPtr<FJsonObject>& Value, const TCHAR* Name)
{ bool Result = false; if (Value) Value->TryGetBoolField(Name, Result); return Result; }
FText Label(const FString& Value) { return FText::FromString(Value); }
}

FRetakeModKitState::FRetakeModKitState()
{
    Revision = TEXT("r") + FDateTime::UtcNow().ToString(TEXT("%Y%m%d%H%M%S"));
    if (const TSharedPtr<FJsonObject> Saved = ReadJSON(Preferences()))
    { ModId = TextField(Saved, TEXT("mod")); GameExe = TextField(Saved, TEXT("game"));
        const FString Kind=TextField(Saved,TEXT("kind"));if(!Kind.IsEmpty())ActiveKind=Kind;
        ServerPath = TextField(Saved, TEXT("server")); UseServer = Flag(Saved,TEXT("useServer"));
        WslDistribution = TextField(Saved,TEXT("wslDistribution")); WslUser = TextField(Saved,TEXT("wslUser"));
        double Port=17777; if(Saved->TryGetNumberField(TEXT("testPort"),Port)) TestPort=static_cast<int32>(Port);
        const TSharedPtr<FJsonObject>* Kinds=nullptr;
        if(Saved->TryGetObjectField(TEXT("workspaces"),Kinds))for(const auto& Entry:(*Kinds)->Values)
        {FString Id;if(Entry.Value->TryGetString(Id))LastWorkspaceByKind.Add(FString::ConstructFromPtrSize(*Entry.Key,Entry.Key.Len()),Id);}
        const TSharedPtr<FJsonObject>* Previous = nullptr;
        if(Saved->TryGetObjectField(TEXT("exports"),Previous)) for(const auto& Entry:(*Previous)->Values)
        { FString Path; if(Entry.Value->TryGetString(Path)) RememberedExports.Add(FString::ConstructFromPtrSize(*Entry.Key, Entry.Key.Len()),Path); }
    }
    if(!GameExe.IsEmpty()) GameExe=FPaths::ConvertRelativePathToFull(GameExe);
    if(!ServerPath.IsEmpty()) ServerPath=FPaths::ConvertRelativePathToFull(ServerPath);
    Checks = {{TEXT("Saved content"), TEXT("Choose a mod to begin")}, {TEXT("Spawns & game modes"), TEXT("Not checked")},
        {TEXT("Elements & surfaces"), TEXT("Not checked")}, {TEXT("Sound & acoustics"), TEXT("Not checked")},
        {TEXT("Visibility bake"), TEXT("Not checked")}, {TEXT("Content isolation"), TEXT("Not checked")},
        {TEXT("Game compatibility"), TEXT("Not checked")}};
    Refresh();
    Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FRetakeModKitState::Tick), .25f);
    if(IsKit() && (GameExe.IsEmpty() || ServerPath.IsEmpty())) DiscoverInstallations();
}

FRetakeModKitState::~FRetakeModKitState()
{
    StopService();
}

void FRetakeModKitState::StopService()
{
    FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
    if (Process.IsValid()) { FPlatformProcess::TerminateProc(Process, true); FPlatformProcess::CloseProc(Process); }
    Process.Reset(); Queue.Reset();
    FPlatformProcess::ClosePipe(ReadPipe, WritePipe); ReadPipe = WritePipe = nullptr;
}

bool FRetakeModKitState::IsKit() const { return FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) == TEXT("RetakeModKit"); }
FString FRetakeModKitState::Python() const
{
#if PLATFORM_WINDOWS
    return Kit() / TEXT(".venv/Scripts/python.exe");
#else
    return Kit() / TEXT(".venv/bin/python3");
#endif
}
FString FRetakeModKitState::Script() const { return Kit() / TEXT("Scripts/ModKit/retake_kit.py"); }
FString FRetakeModKitState::Quote(const FString& Value) { return TEXT("\"") + Value.Replace(TEXT("\""), TEXT("\\\"")) + TEXT("\""); }
bool FRetakeModKitState::HasTools() const { return FPaths::FileExists(Python()) && FPaths::FileExists(Script()); }

void FRetakeModKitState::SavePreferences() const
{
    auto Value = MakeShared<FJsonObject>(); Value->SetStringField(TEXT("mod"), ModId); Value->SetStringField(TEXT("game"), GameExe);
    Value->SetStringField(TEXT("kind"),ActiveKind);auto Workspaces=MakeShared<FJsonObject>();
    for(const auto& Entry:LastWorkspaceByKind)Workspaces->SetStringField(Entry.Key,Entry.Value);
    Value->SetObjectField(TEXT("workspaces"),Workspaces);
    Value->SetStringField(TEXT("server"), ServerPath); Value->SetBoolField(TEXT("useServer"),UseServer);
    Value->SetStringField(TEXT("wslDistribution"),WslDistribution); Value->SetStringField(TEXT("wslUser"),WslUser);
    Value->SetNumberField(TEXT("testPort"),TestPort);
    auto ExportSelections = MakeShared<FJsonObject>();
    for(const auto& Entry:RememberedExports) ExportSelections->SetStringField(Entry.Key,Entry.Value);
    Value->SetObjectField(TEXT("exports"),ExportSelections);
    FString JSON; FJsonSerializer::Serialize(Value, TJsonWriterFactory<>::Create(&JSON));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Preferences()), true);
    FFileHelper::SaveStringToFile(JSON, *Preferences(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void FRetakeModKitState::Refresh()
{
    if (!IsKit()) return;
    Mods.Reset();
    for (const FString& Name : RetakeModWorkspace::List()) if(RetakeModWorkspace::Kind(Name)==ActiveKind)Mods.Add(MakeShared<FString>(Name));
    if (!Mods.ContainsByPredicate([this](const TSharedPtr<FString>& Name) { return *Name == ModId; }))
        ModId = Mods.IsEmpty() ? FString() : *Mods[0];
    Maps.Reset(); MapPackages.Reset();
    if (!ModId.IsEmpty() && CanAuthorCurrentKind())
    {
        FString Error;
        if (RetakeModWorkspace::Open(ModId, Error))
        {
            TArray<FAssetData> Assets;
            IAssetRegistry::GetChecked().GetAssetsByPath(FName(*(TEXT("/") + ModId)), Assets, true);
            for (const FAssetData& Asset : Assets)
                if (Asset.AssetClassPath == UWorld::StaticClass()->GetClassPathName())
                { Maps.Add(MakeShared<FString>(Asset.AssetName.ToString())); MapPackages.Add(Asset.AssetName.ToString(), Asset.PackageName.ToString()); }
            Maps.Sort([](const TSharedPtr<FString>& A, const TSharedPtr<FString>& B) { return *A < *B; });
        }
        else Notice = Error;
    }
    if (!Maps.ContainsByPredicate([this](const TSharedPtr<FString>& Name) { return *Name == MapName; }))
        MapName = Maps.IsEmpty() ? FString() : *Maps[0];
    RefreshExports(); RefreshMinimapState();
    ++ChangeSerial; Changed.Broadcast();
}

void FRetakeModKitState::Select(const FString& Mod)
{
    if (Busy()) return;
    if(!Mod.IsEmpty())ActiveKind=RetakeModWorkspace::Kind(Mod);
    ModId = Mod;if(!ModId.IsEmpty())LastWorkspaceByKind.Add(ActiveKind,ModId);
    TestLaunched = false; Ready = Valid = OperationFailed = false; Stale = true; Errors = Warnings = 0; Issues.Reset(); ValidationStamp.Reset();
    LastExport.Reset(); LastRecovery.Reset(); Notice.Reset(); MapModes.Reset(); PreparationChecks.Reset(); TestModes.Reset(); TestMode.Reset();
    PreparationCurrent = PreparationNeeded = PreparationBlocked = false;
    InvalidateTestCheck(); Refresh(); SavePreferences();
}

void FRetakeModKitState::SelectKind(const FString& Kind)
{
    if(Busy()||Kind==ActiveKind)return;
    if(!RetakeModWorkspace::Kinds().ContainsByPredicate([&Kind](const RetakeModWorkspace::FKind& Entry){return Kind==Entry.Id;}))return;
    if(!ModId.IsEmpty())LastWorkspaceByKind.Add(ActiveKind,ModId);
    ActiveKind=Kind;ModId=LastWorkspaceByKind.FindRef(Kind);
    Ready=Valid=OperationFailed=TestLaunched=false;Stale=true;Errors=Warnings=0;Issues.Reset();ValidationStamp.Reset();
    LastExport.Reset();LastRecovery.Reset();Notice.Reset();MapModes.Reset();PreparationChecks.Reset();TestModes.Reset();TestMode.Reset();
    PreparationCurrent=PreparationNeeded=PreparationBlocked=false;InvalidateTestCheck();Refresh();SavePreferences();
}
bool FRetakeModKitState::CanAuthorCurrentKind() const {FString Reason;return RetakeModWorkspace::CanCreateKind(ActiveKind,Reason);}
FString FRetakeModKitState::KindDescription() const
{
    FString Reason;RetakeModWorkspace::CanCreateKind(ActiveKind,Reason);return Reason;
}
void FRetakeModKitState::OpenContent()
{
    if(!Busy()&&!ModId.IsEmpty()&&CanAuthorCurrentKind())RetakeModWorkspace::Reveal(ModId);
}
void FRetakeModKitState::OpenWeaponCatalogue()
{
    if(Busy()||ActiveKind!=TEXT("weapons")||ModId.IsEmpty()||!CanAuthorCurrentKind())return;
    TArray<FAssetData> Assets;IAssetRegistry::GetChecked().GetAssetsByPath(FName(*(TEXT("/")+ModId)),Assets,true);
    Assets.RemoveAll([](const FAssetData& Asset){return Asset.AssetClassPath.ToString()!=TEXT("/Script/RetakeMapSDK.RetakeWeaponPack");});
    if(Assets.Num()!=1){Notice=TEXT("Keep exactly one weapon catalogue in this pack. Inspect its Data folder.");return;}
    if(UObject* Asset=Assets[0].GetAsset())GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset);
}
void FRetakeModKitState::CreateWeaponAsset(const FString& Role)
{
    if(Busy()||ActiveKind!=TEXT("weapons")||ModId.IsEmpty()||!CanAuthorCurrentKind()||GEditor->PlayWorld)return;
    FString ClassName,Prefix,Folder;
    if(Role==TEXT("specification")){ClassName=TEXT("RetakeWeaponSpecification");Prefix=TEXT("DA_Weapon");Folder=TEXT("Weapons");}
    else if(Role==TEXT("ammo")){ClassName=TEXT("RetakeAmmoDefinition");Prefix=TEXT("DA_Ammo");Folder=TEXT("Ammo");}
    else if(Role==TEXT("animation")){ClassName=TEXT("RetakeWeaponAnimationProfile");Prefix=TEXT("DA_WeaponAnimation");Folder=TEXT("Profiles");}
    else if(Role==TEXT("tracer")){ClassName=TEXT("RetakeTracerProfile");Prefix=TEXT("DA_Tracer");Folder=TEXT("Profiles");}
    else return;
    UClass* Class=FindObject<UClass>(nullptr,*(TEXT("/Script/RetakeMapSDK.")+ClassName));
    if(!Class||!Class->IsChildOf(UDataAsset::StaticClass())){Notice=TEXT("This authoring asset type is not available in the installed SDK.");return;}
    UDataAssetFactory* Factory=NewObject<UDataAssetFactory>();Factory->DataAssetClass=Class;
    FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().CreateAssetWithDialog(Prefix,TEXT("/")+ModId+TEXT("/")+Folder,UDataAsset::StaticClass(),Factory);
    Stale=true;Ready=false;++ChangeSerial;Changed.Broadcast();
}

bool FRetakeModKitState::HasUnsaved() const
{
    TArray<UPackage*> Packages; UEditorLoadingAndSavingUtils::GetDirtyMapPackages(Packages);
    TArray<UPackage*> Content; UEditorLoadingAndSavingUtils::GetDirtyContentPackages(Content); Packages.Append(Content);
    const FString Root = TEXT("/") + ModId + TEXT("/");
    return Packages.ContainsByPredicate([&](const UPackage* P) { return P->GetName().StartsWith(Root); });
}

FString FRetakeModKitState::SourceStamp() const
{
    if (ModId.IsEmpty()) return FString();
    const FString Root = Kit() / TEXT("Mods") / ModId;
    TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(Files, *(Root / TEXT("Content")), TEXT("*"), true, false);
    IFileManager::Get().FindFilesRecursive(Files, *(Root / TEXT("SourceArt")), TEXT("*"), true, false);
    Files.Add(Root / (ModId + TEXT(".uplugin"))); Files.Add(Root / TEXT("SourceFiles.json"));
    Files.Add(Kit() / TEXT("SDK.lock.json")); Files.Add(Kit() / TEXT("ModKit/qualification.json")); Files.Sort();
    FMD5 Hash;
    const FTCHARToUTF8 ModeBytes(*Modes);
    Hash.Update(reinterpret_cast<const uint8*>(ModeBytes.Get()), ModeBytes.Length());
    for (const FString& File : Files)
    {
        const FString Row = File + FString::Printf(TEXT("|%lld|%lld"), IFileManager::Get().FileSize(*File), IFileManager::Get().GetTimeStamp(*File).GetTicks());
        FTCHARToUTF8 Bytes(*Row); Hash.Update(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
    }
    uint8 Digest[16]; Hash.Final(Digest); return BytesToHex(Digest, 16);
}

void FRetakeModKitState::Save()
{
    if (Busy() || !IsKit() || ModId.IsEmpty()) return;
    TArray<UPackage*> Packages, Content;
    UEditorLoadingAndSavingUtils::GetDirtyMapPackages(Packages); UEditorLoadingAndSavingUtils::GetDirtyContentPackages(Content); Packages.Append(Content);
    const FString Root = TEXT("/") + ModId + TEXT("/");
    Packages.RemoveAll([&](const UPackage* P) { return !P->GetName().StartsWith(Root); });
    if (!Packages.IsEmpty() && !UEditorLoadingAndSavingUtils::SavePackages(Packages, false)) Notice = TEXT("Some content could not be saved. Check the editor messages.");
    else Notice = TEXT("Mod content saved. Check the map to refresh its readiness.");
    Stale = true; Ready = false; ++ChangeSerial; Changed.Broadcast();
}

void FRetakeModKitState::OpenMap()
{
    if (Busy() || !IsMapWorkspace() || MapName.IsEmpty()) return;
    TArray<UPackage*> Dirty; UEditorLoadingAndSavingUtils::GetDirtyMapPackages(Dirty);
    if (!Dirty.IsEmpty()) { Notice = TEXT("Save your open level before switching maps."); return; }
    if (const FString* Package = MapPackages.Find(MapName)) UEditorLoadingAndSavingUtils::LoadMap(*Package);
    RefreshMinimapState(); ++ChangeSerial; Changed.Broadcast();
}

void FRetakeModKitState::OpenExample()
{
    if (Busy() || !IsKit() || !IsMapWorkspace()) return;
    const FString Example(TEXT("RetakeMod_Example"));
    if (RetakeModWorkspace::List().Contains(Example)) { Select(Example); OpenMap(); return; }
    if (!HasTools()) { Notice = TEXT("Set up authoring tools first, then create the original-content example."); return; }
    ModId = Example; CurrentAction = TEXT("example");
    JobDirectory = Kit() / TEXT("Saved/ModKit/EditorJobs") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
    IFileManager::Get().MakeDirectory(*JobDirectory, true); LastLog = JobDirectory / TEXT("activity.log");
    Queue = {{Python(), TEXT("-X utf8 ") + Quote(Script()) + TEXT(" example --kit ") + Quote(Kit()) + TEXT(" --mod ") + Example,
        TEXT("Creating the original-content example")}};
    StartNext();
}

bool FRetakeModKitState::ReleaseModPackages()
{
    TArray<UPackage*> DirtyMaps; UEditorLoadingAndSavingUtils::GetDirtyMapPackages(DirtyMaps);
    if (HasUnsaved() || !DirtyMaps.IsEmpty()) { Notice = TEXT("Save your changes before preparing the map."); return false; }
    const FString Root = TEXT("/") + ModId + TEXT("/");
    UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    for (UObject* Asset : Editors->GetAllEditedAssets()) if (Asset && Asset->GetOutermost()->GetName().StartsWith(Root)) Editors->CloseAllEditorsForAsset(Asset);
    UWorld* World = GEditor->GetEditorWorldContext().World();
    ReturnMap.Reset();
    if (World && World->GetOutermost()->GetName().StartsWith(Root))
    { ReturnMap = World->GetOutermost()->GetName();
        if (UWorld* Empty = UEditorLoadingAndSavingUtils::NewBlankMap(false)) PreparationPlaceholder = Empty->GetOutermost()->GetName(); }
    FAssetCompilingManager::Get().FinishAllCompilation();
    TArray<UPackage*> Packages;
    for (TObjectIterator<UPackage> It; It; ++It) if (It->GetName().StartsWith(Root)) Packages.Add(*It);
    FText Error;
    if (!Packages.IsEmpty()) UPackageTools::UnloadPackages(Packages, Error, false);
    for (TObjectIterator<UObject> It; It; ++It)
        if (It->IsAsset() && !It->HasAnyFlags(RF_Transient | RF_BeginDestroyed | RF_FinishDestroyed)
            && It->GetOutermost()->GetName().StartsWith(Root))
        { Notice = TEXT("Close asset editors that still reference this mod before preparation. ") + Error.ToString(); RestoreMap(); return false; }
    return true;
}

void FRetakeModKitState::RestoreMap()
{
    if (ModId.IsEmpty()) return;
    IAssetRegistry::GetChecked().ScanPathsSynchronous({TEXT("/") + ModId + TEXT("/")}, true);
    TArray<UPackage*> Dirty; UEditorLoadingAndSavingUtils::GetDirtyMapPackages(Dirty);
    UWorld* Current = GEditor->GetEditorWorldContext().World();
    if (!ReturnMap.IsEmpty() && Dirty.IsEmpty() && Current && Current->GetOutermost()->GetName() == PreparationPlaceholder)
        UEditorLoadingAndSavingUtils::LoadMap(ReturnMap);
    ReturnMap.Reset(); PreparationPlaceholder.Reset();
}

UWorld* FRetakeModKitState::EditingMap() const
{
    const FString* Expected=MapPackages.Find(MapName);
    UWorld* World=GEditor?GEditor->GetEditorWorldContext().World():nullptr;
    return Expected && World && World->GetPackage()->GetName()==*Expected ? World : nullptr;
}
bool FRetakeModKitState::CanAuthorMinimap() const { return IsKit() && !Busy() && GEditor && !GEditor->PlayWorld && EditingMap(); }
bool FRetakeModKitState::HasMinimapCapture() const { return MinimapCapture.IsValid() && EditingMap(); }
bool FRetakeModKitState::HasMinimapPreview() const { return MinimapDefinition.IsValid() && MinimapDefinition->Floorplan; }
bool FRetakeModKitState::CanBakeMinimap() const { return CanAuthorMinimap() && HasTools() && HasMinimapCapture() && !HasUnsaved() && (!UseCustomMinimap || !CustomMinimapTexture.IsEmpty()); }
bool FRetakeModKitState::MinimapIsCurrent() const
{
    if(Stale||!HasMinimapPreview()||UseCustomMinimap!=RetakeMinimapBaker::IsCustom(*MinimapDefinition))return false;
    if(UseCustomMinimap&&CustomMinimapTexture!=MinimapDefinition->Floorplan->GetPathName())return false;
    return PreparationChecks.ContainsByPredicate([this](const FRetakeCreatorCheck& Check)
    {return Check.Title==MapName+TEXT(" / Minimap (optional)")&&Check.State==1;});
}
void FRetakeModKitState::RefreshMinimapState()
{
    MinimapCapture.Reset(); MinimapDefinition.Reset();
    UWorld* World=EditingMap();
    if(!World){MinimapMessage=TEXT("Open the selected map to set up its minimap.");return;}
    const auto Captures=RetakeMinimapBaker::FindCaptures(*World);
    if(Captures.Num()==1)MinimapCapture=const_cast<ARetakeMinimapCapture*>(Captures[0]);
    MinimapMessage=Captures.Num()==0?TEXT("Optional: add a capture to generate an image or align custom artwork."):
        Captures.Num()>1?TEXT("More than one capture exists. Keep one before creating the minimap."):TEXT("Review the capture bounds and height in the level before generating or applying artwork.");
    const FString Package=TEXT("/")+ModId+TEXT("/Minimap/DA_Minimap_")+MapName;
    if(FPackageName::DoesPackageExist(Package))MinimapDefinition=LoadObject<URetakeMinimapDefinition>(nullptr,*(Package+TEXT(".")+FPackageName::GetShortName(Package)));
    const FString Choice=ModId+TEXT(".")+MapName;
    if(MinimapChoiceForMap!=Choice)
    {
        MinimapChoiceForMap=Choice;UseCustomMinimap=MinimapDefinition.IsValid()&&RetakeMinimapBaker::IsCustom(*MinimapDefinition);
        CustomMinimapTexture=UseCustomMinimap&&MinimapDefinition->Floorplan?MinimapDefinition->Floorplan->GetPathName():FString();
    }
}
void FRetakeModKitState::SelectMinimapCapture()
{
    if(!CanAuthorMinimap())return;RefreshMinimapState();
    if(!MinimapCapture.IsValid())return;
    GEditor->SelectNone(false,true);GEditor->SelectActor(MinimapCapture.Get(),true,true);GEditor->NoteSelectionChange();
    Notice=TEXT("Capture selected. Edit floor bands and exclusion tags in Details; the box shows its coverage.");
}
void FRetakeModKitState::SetupMinimap()
{
    if(!CanAuthorMinimap()){Notice=TEXT("Open the selected map first.");return;}
    RefreshMinimapState();UWorld* World=EditingMap();
    if(RetakeMinimapBaker::FindCaptures(*World).Num()>1){Notice=MinimapMessage;return;}
    if(!MinimapCapture.IsValid())
    {
        const FScopedTransaction Transaction(Label(TEXT("Create minimap capture")));
        MinimapCapture=Cast<ARetakeMinimapCapture>(GEditor->AddActor(World->PersistentLevel,ARetakeMinimapCapture::StaticClass(),FTransform::Identity,true,RF_Transactional));
        if(!MinimapCapture.IsValid()){Notice=TEXT("Could not create the capture.");return;}
        MinimapCapture->SetActorLabel(TEXT("Minimap Capture"));MinimapCapture->MarkPackageDirty();Stale=true;Ready=false;
        FitMinimap(false);
    }
    SelectMinimapCapture();++ChangeSerial;Changed.Broadcast();
}
void FRetakeModKitState::FitMinimap(bool SelectionOnly)
{
    if(!CanAuthorMinimap()||!MinimapCapture.IsValid()){Notice=TEXT("Set up a minimap capture in the open map first.");return;}
    UWorld* World=EditingMap();ARetakeMinimapCapture* Capture=MinimapCapture.Get();
    const FRotator Rotation(0,Capture->GetActorRotation().Yaw,0);FBox Bounds(ForceInit);
    for(AActor* Actor:World->PersistentLevel->Actors)
    {
        if(!IsValid(Actor)||Actor->IsEditorOnly()||Actor->IsHidden()||Actor->ActorHasTag(Capture->ExcludeTag)
            ||(SelectionOnly&&!GEditor->GetSelectedActors()->IsSelected(Actor)))continue;
        for(UStaticMeshComponent* Component:TInlineComponentArray<UStaticMeshComponent*>(Actor))
        {
            if(!Component->GetStaticMesh()||Component->IsEditorOnly()||!Component->IsVisible()||Component->bHiddenInGame||Component->ComponentHasTag(Capture->ExcludeTag))continue;
            if(!SelectionOnly&&!Component->IsQueryCollisionEnabled())continue;
            if(const auto* Instances=Cast<UInstancedStaticMeshComponent>(Component);Instances&&Instances->GetInstanceCount()==0)continue;
            const FBox Box=Component->CalcBounds(Component->GetComponentTransform()).GetBox();
            if(!Box.IsValid)continue;
            for(int32 Corner=0;Corner<8;++Corner)Bounds+=Rotation.UnrotateVector(FVector(Corner&1?Box.Max.X:Box.Min.X,Corner&2?Box.Max.Y:Box.Min.Y,Corner&4?Box.Max.Z:Box.Min.Z));
        }
    }
    if(!Bounds.IsValid){Notice=TEXT("No suitable static meshes found. Add geometry or select visible meshes, then fit again.");return;}
    const FScopedTransaction Transaction(Label(TEXT("Fit minimap coverage")));Capture->Modify();
    const FVector Center=Rotation.RotateVector(Bounds.GetCenter());
    Capture->SetActorLocation(FVector(Center.X,Center.Y,Bounds.Min.Z));Capture->SetActorRotation(Rotation);Capture->SetActorScale3D(FVector::OneVector);
    Capture->CaptureSize=FMath::Max(1000.,FMath::Max(Bounds.GetSize().X,Bounds.GetSize().Y)*1.1);
    Capture->CaptureHeight=FMath::Max(1000.,Bounds.GetSize().Z+100.);
    if(!Capture->FloorBandHeights.IsEmpty())Capture->CaptureHeight=FMath::Max(Capture->CaptureHeight,Capture->FloorBandHeights.Last()+100.);
    Capture->RerunConstructionScripts();Capture->MarkPackageDirty();Stale=true;Ready=false;
    Notice=TEXT("Coverage fitted. Review the height cutoff to exclude roofs, then save and create the image.");++ChangeSerial;Changed.Broadcast();
}
TOptional<double> FRetakeModKitState::MinimapValue(const FString& Field) const
{
    const ARetakeMinimapCapture* Capture=MinimapCapture.Get();if(!Capture||!EditingMap())return {};
    if(Field==TEXT("size"))return Capture->CaptureSize/100.;
    if(Field==TEXT("height"))return Capture->CaptureHeight/100.;
    if(Field==TEXT("datum"))return Capture->GetActorLocation().Z/100.;
    return Capture->GetActorRotation().Yaw;
}
void FRetakeModKitState::SetMinimapValue(const FString& Field,double Value)
{
    if(!CanAuthorMinimap()||!MinimapCapture.IsValid()||!FMath::IsFinite(Value))return;
    if((Field==TEXT("size")&&Value<10.)||(Field==TEXT("height")&&Value<1.)){Notice=TEXT("Coverage must be at least 10 m and height cutoff at least 1 m.");return;}
    ARetakeMinimapCapture* Capture=MinimapCapture.Get();const FScopedTransaction Transaction(Label(TEXT("Edit minimap alignment")));Capture->Modify();
    if(Field==TEXT("size"))Capture->CaptureSize=Value*100.;
    else if(Field==TEXT("height")){Capture->CaptureHeight=Value*100.;Capture->FloorBandHeights.RemoveAll([Capture](double H){return H>=Capture->CaptureHeight;});}
    else if(Field==TEXT("datum")){FVector Location=Capture->GetActorLocation();Location.Z=Value*100.;Capture->SetActorLocation(Location);}
    else Capture->SetActorRotation(FRotator(0,FRotator::NormalizeAxis(Value),0));
    Capture->RerunConstructionScripts();Capture->MarkPackageDirty();Stale=true;Ready=false;++ChangeSerial;Changed.Broadcast();
}
void FRetakeModKitState::ChooseMinimapTexture(bool Import)
{
    if(!CanAuthorMinimap())return;
    TArray<UObject*> Objects;
    if(Import)Objects=FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().ImportAssetsWithDialog(TEXT("/")+ModId+TEXT("/Minimap/Custom"));
    else
    {
        TArray<FAssetData> Selected;FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().GetSelectedAssets(Selected);
        for(const FAssetData& Asset:Selected)Objects.Add(Asset.GetAsset());
    }
    TArray<UTexture2D*> Textures;
    for(UObject* Object:Objects)if(UTexture2D* Texture=Cast<UTexture2D>(Object))Textures.Add(Texture);
    if(Textures.Num()!=1||!Textures[0]->GetPackage()->GetName().StartsWith(TEXT("/")+ModId+TEXT("/")))
    {Notice=TEXT("Choose one texture inside this mod's Content Browser.");return;}
    UTexture2D* Texture=Textures[0];
    if(Texture->Source.GetSizeX()!=1024||Texture->Source.GetSizeY()!=1024){Notice=TEXT("Custom minimaps need a 1024 x 1024 image. Resize the source image and reimport it.");return;}
    CustomMinimapTexture=Texture->GetPathName();UseCustomMinimap=true;
    Notice=TEXT("Custom image selected. Review its world coverage, save, then Apply custom image.");++ChangeSerial;Changed.Broadcast();
}
void FRetakeModKitState::OpenMinimapPreview()
{
    if(Busy())return;RefreshMinimapState();
    if(HasMinimapPreview())GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(MinimapDefinition->Floorplan.Get());
    else Notice=TEXT("Generate or apply a minimap image first.");
}

void FRetakeModKitState::SetupTools()
{
    if (Busy() || !IsKit()) return;
#if PLATFORM_WINDOWS
    const FString Bootstrap = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/ThirdParty/Python3/Win64/python.exe"));
#else
    const FString Bootstrap = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/ThirdParty/Python3/Linux/bin/python3"));
#endif
    if (!FPaths::FileExists(Bootstrap)) { Notice = TEXT("The engine's Python tools are missing. Enable Python Editor Script Plugin and restart the editor."); return; }
    CurrentAction = TEXT("setup");
    JobDirectory = Kit() / TEXT("Saved/ModKit/EditorJobs") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
    IFileManager::Get().MakeDirectory(*JobDirectory, true); LastLog = JobDirectory / TEXT("activity.log");
    Queue = {{Bootstrap, TEXT("-m venv ") + Quote(Kit() / TEXT(".venv")), TEXT("Setting up authoring tools")},
        {Python(), TEXT("-m pip install --force-reinstall --only-binary=:all: -r ") + Quote(Kit() / TEXT("Scripts/ModKit/requirements.txt")), TEXT("Installing authoring dependencies")},
        {Python(), TEXT("-c ") + Quote(TEXT("import blake3; import numpy")), TEXT("Verifying authoring dependencies")}};
    StartNext();
}

void FRetakeModKitState::SelectMap(const FString& Map)
{
    if (Busy()) return;
    MapName = Map; TestLaunched = false; RefreshTestModes(); RefreshMinimapState(); ++ChangeSerial; Changed.Broadcast();
}

FString FRetakeModKitState::DetectedModes() const
{
    const FString* Found = MapModes.Find(MapName);
    return !Stale && Found ? *Found : FString();
}

void FRetakeModKitState::RefreshTestModes()
{
    TestModes.Reset();
    const auto Manifest = ReadJSON(LastExport / TEXT("manifest.json"));
    const TArray<TSharedPtr<FJsonValue>>* ExportMaps = nullptr;
    if (Manifest && Manifest->TryGetArrayField(TEXT("maps"), ExportMaps))
        for (const auto& Value : *ExportMaps)
        {
            const auto Map = Value->AsObject();
            if (FPaths::GetCleanFilename(TextField(Map, TEXT("packageName"))) != MapName) continue;
            const TArray<TSharedPtr<FJsonValue>>* Supported = nullptr;
            if (Map->TryGetArrayField(TEXT("supportedModes"), Supported))
                for (const auto& Mode : *Supported) TestModes.Add(MakeShared<FString>(Mode->AsString()));
        }
    if (!TestModes.ContainsByPredicate([this](const TSharedPtr<FString>& Mode) { return *Mode == TestMode; }))
        TestMode = TestModes.IsEmpty() ? FString() : *TestModes[0];
}

bool FRetakeModKitState::CanPrepare() const
{
    return IsKit() && IsMapWorkspace() && HasTools() && !Busy() && !Stale && !HasUnsaved() && PreparationNeeded && !PreparationBlocked;
}

bool FRetakeModKitState::CanExport() const { return IsKit() && IsMapWorkspace() && HasTools() && !Busy() && Ready && !Stale && !HasUnsaved(); }
bool FRetakeModKitState::IsTestingWorkflow() const
{
    return CurrentAction==TEXT("test-info")||CurrentAction==TEXT("test")||CurrentAction==TEXT("start-test-server")
        ||CurrentAction==TEXT("server-status")||CurrentAction==TEXT("stop-test-server");
}
bool FRetakeModKitState::HasExport() const { return !LastExport.IsEmpty() && FPaths::FileExists(LastExport / TEXT("manifest.json")); }
FString FRetakeModKitState::TestSelectionStamp() const
{
    return LastExport + TEXT("|") + GameExe + TEXT("|") + ServerPath + (UseServer?TEXT("|server|"):TEXT("|client|"))
        + WslDistribution + TEXT("|") + WslUser + TEXT("|") + FString::FromInt(TestPort);
}
bool FRetakeModKitState::TestCheckCurrent() const { return !TestCheckStamp.IsEmpty() && TestCheckStamp == TestSelectionStamp(); }
bool FRetakeModKitState::CanTest() const { return IsMapWorkspace() && !Busy() && HasExport() && TestReady && TestCheckCurrent() && !TestMode.IsEmpty(); }
bool FRetakeModKitState::CanStartServer() const { return IsMapWorkspace() && !Busy() && HasExport() && UseServer && ServerAllowed && TestCheckCurrent() && !TestMode.IsEmpty(); }
void FRetakeModKitState::InvalidateTestCheck()
{
    TestReady = ClientAllowed = ServerAllowed = TestLaunched = false; TestCheckStamp.Reset();
    ClientTestDetail = TEXT("Selected client compatibility has not been checked against this export.");
    ServerTestDetail = UseServer?TEXT("Selected server compatibility has not been checked against this export."):TEXT("Local server is optional.");
}
void FRetakeModKitState::RefreshExports()
{
    if(Busy())return;
    Exports.Reset(); ExportLabels.Reset();
    if(!IsMapWorkspace()){LastExport.Reset();TestModes.Reset();return;}
    TArray<FString> Candidates;
    for(const FString& Root : {Kit()/TEXT("Exports")/ModId, Kit()/TEXT("Saved/ModKit/QualificationCandidates")/ModId})
    {
        TArray<FString> Directories; IFileManager::Get().FindFiles(Directories,*(Root/TEXT("*")),false,true);
        for(const FString& Directory:Directories) if(!Directory.StartsWith(TEXT("."))) Candidates.Add(Root/Directory);
    }
    if(const FString* Previous=RememberedExports.Find(ModId))Candidates.AddUnique(*Previous);
    if(!LastExport.IsEmpty())Candidates.AddUnique(LastExport);
    Candidates.Sort([](const FString& A,const FString& B){return IFileManager::Get().GetTimeStamp(*(A/TEXT("manifest.json"))) > IFileManager::Get().GetTimeStamp(*(B/TEXT("manifest.json")));});
    for(const FString& Path:Candidates)
    {
        const auto Manifest=ReadJSON(Path/TEXT("manifest.json")); double Format=0;
        if(!Manifest || TextField(Manifest,TEXT("modId"))!=ModId || TextField(Manifest,TEXT("format"))!=TEXT("RetakeMap")
            || !Manifest->TryGetNumberField(TEXT("schemaVersion"),Format) || Format!=3)continue;
        Exports.Add(MakeShared<FString>(Path));
        ExportLabels.Add(Path,TextField(Manifest,TEXT("revision"))+(Flag(Manifest,TEXT("qualificationCandidate"))?TEXT(" | Local test bundle"):TEXT(" | Export")));
    }
    FString Selected=LastExport;
    if(Selected.IsEmpty())if(const FString* Previous=RememberedExports.Find(ModId))Selected=*Previous;
    if(!ExportLabels.Contains(Selected))Selected=Exports.IsEmpty()?FString():*Exports[0];
    if(Selected!=LastExport){LastExport=Selected;ExportStamp.Reset();InvalidateTestCheck();}
    RefreshTestModes();
}
void FRetakeModKitState::SelectExport(const FString& Path)
{
    if(Busy())return;
    const FString Absolute=FPaths::ConvertRelativePathToFull(Path);
    const auto Manifest=ReadJSON(Absolute/TEXT("manifest.json"));
    if(!Manifest || TextField(Manifest,TEXT("modId"))!=ModId){Notice=TEXT("Choose an exported bundle for this mod containing manifest.json.");return;}
    if(LastExport!=Absolute){LastExport=Absolute;ExportStamp.Reset();InvalidateTestCheck();}
    RememberedExports.Add(ModId,LastExport);RefreshExports();SavePreferences();++ChangeSerial;Changed.Broadcast();
}
void FRetakeModKitState::ChooseExport()
{
    FString Folder;
    if(IDesktopPlatform* Desktop=FDesktopPlatformModule::Get())
        if(Desktop->OpenDirectoryDialog(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),TEXT("Choose an exported mod bundle"),LastExport,Folder))SelectExport(Folder);
}

void FRetakeModKitState::Run(const FString& Action)
{
    if (Busy() || !IsKit() || (ModId.IsEmpty() && Action != TEXT("stop-test-server") && Action != TEXT("server-status"))) return;
    if(!IsMapWorkspace()&&Action!=TEXT("draft-status")&&Action!=TEXT("server-status")&&Action!=TEXT("stop-test-server"))
    {Notice=TEXT("This mod type has a separate authoring workflow. Its playable export and test pipeline is not enabled yet.");return;}
    if(Action==TEXT("draft-status")&&(ActiveKind!=TEXT("weapons")||!CanAuthorCurrentKind())){Notice=KindDescription();return;}
    if (!HasTools()) { Notice = TEXT("Set up the authoring tools first. This takes one click."); return; }
    if (GEditor->PlayWorld) { Notice = TEXT("Stop Play in Editor before running authoring tools."); return; }
    const bool SourceAction = Action == TEXT("status") || Action == TEXT("prepare") || Action == TEXT("export") || Action == TEXT("minimap") || Action == TEXT("draft-status");
    if (SourceAction && HasUnsaved()) { Notice = TEXT("Save mod content first so the checks and export use your latest work."); return; }
    if (SourceAction && IsMapWorkspace() && Modes.IsEmpty()) { Notice = TEXT("Select at least one game mode, or use automatic mode detection."); return; }
    if (Action == TEXT("export") && !CanExport()) { Notice = TEXT("Check and prepare the map before exporting."); return; }
    if (Action == TEXT("prepare") && !CanPrepare())
    { Notice = Stale ? TEXT("Check the saved map first to identify required preparation.") : PreparationCurrent ? TEXT("Preparation is up to date. Continue to Export.") : TEXT("Fix the preparation setup shown below, then check again."); return; }
    if(Action==TEXT("minimap")&&!CanBakeMinimap()){Notice=TEXT("Open the map, configure its capture and save before creating the minimap.");return;}
    if ((Action == TEXT("prepare") || Action == TEXT("minimap")) && !ReleaseModPackages()) return;
    CurrentAction = Action; Notice.Reset(); Activity.Reset(); PendingOutput.Reset();
    JobStartStamp = SourceStamp(); JobTestStamp=TestSelectionStamp();
    JobDirectory = Kit() / TEXT("Saved/ModKit/EditorJobs") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
    IFileManager::Get().MakeDirectory(*JobDirectory, true);
    LastLog = JobDirectory / TEXT("activity.log"); ReportPath = JobDirectory / TEXT("readiness.json");
    FString Arguments = TEXT("-X utf8 ") + Quote(Script()) + TEXT(" ") + Action + TEXT(" --kit ") + Quote(Kit())
        + TEXT(" --mod ") + ModId + TEXT(" --modes ") + Quote(Modes);
    if (Action == TEXT("status") || Action == TEXT("prepare") || Action == TEXT("draft-status")) Arguments += TEXT(" --report ") + Quote(ReportPath);
    if(Action==TEXT("minimap"))
    {
        Arguments+=TEXT(" --map ")+Quote(MapName);
        Arguments+=UseCustomMinimap?TEXT(" --custom-minimap-texture ")+Quote(CustomMinimapTexture):TEXT(" --replace-custom-minimap");
    }
    if (Action == TEXT("export"))
    {
        bool bInvalidRevision = Revision.IsEmpty() || Revision.Len() > 32;
        for (TCHAR C : Revision) bInvalidRevision |= C > 127 || !FChar::IsAlnum(C);
        if (bInvalidRevision)
        { Notice = TEXT("Use 1 to 32 letters or numbers for the revision."); return; }
        Arguments += TEXT(" --revision ") + Revision;
        if (Candidate) Arguments += TEXT(" --candidate");
    }
    if (Action == TEXT("test") || Action == TEXT("start-test-server") || Action == TEXT("test-info"))
    {
        if (!HasExport()) { Notice = TEXT("Select an existing exported revision, or create one in step 5."); return; }
        if (Action == TEXT("test") && !CanTest()) { Notice = TEXT("Check the selected installations and resolve the reported compatibility issues first."); return; }
        if (Action == TEXT("start-test-server") && !CanStartServer()) { Notice = TEXT("Check the selected server installation before starting it."); return; }
        if (Action == TEXT("test") && !FPaths::FileExists(GameExe)) { ChooseGame(); return; }
        if (!GameExe.IsEmpty()) Arguments += TEXT(" --game ") + Quote(GameExe);
        Arguments += TEXT(" --export-dir ") + Quote(LastExport)
            + TEXT(" --mods-dir ") + Quote(Kit() / TEXT("Saved/ModKit/TestMods")) + TEXT(" --map ") + Quote(MapName);
        if (!TestMode.IsEmpty()) Arguments += TEXT(" --mode ") + Quote(TestMode);
        if(UseServer)
        {
            if(Action != TEXT("test-info") && !FPaths::FileExists(ServerPath)) { Notice=TEXT("Choose the existing Linux server installation first."); return; }
            Arguments += TEXT(" --server ")+Quote(ServerPath)+TEXT(" --port ")+FString::FromInt(TestPort);
            if(!WslDistribution.IsEmpty()) Arguments += TEXT(" --wsl-distribution ")+Quote(WslDistribution);
            if(!WslUser.IsEmpty()) Arguments += TEXT(" --wsl-user ")+Quote(WslUser);
        }
        if(Action == TEXT("test-info")) Arguments += TEXT(" --report ") + Quote(ReportPath);
        SavePreferences();
    }
    if(Action == TEXT("server-status")) Arguments += TEXT(" --report ") + Quote(ReportPath);
    Queue = {{Python(), Arguments, Action == TEXT("status") ? TEXT("Checking your map") : Action == TEXT("prepare") ? TEXT("Preparing your map")
        : Action == TEXT("draft-status") ? TEXT("Checking weapon authoring data") : Action == TEXT("export") ? TEXT("Exporting your mod") : Action == TEXT("test-info") ? TEXT("Checking selected installations")
        : Action == TEXT("server-status") ? TEXT("Checking test-server status") : Action == TEXT("start-test-server") ? TEXT("Starting the local server") : TEXT("Opening Retake")}};
    if(Action==TEXT("minimap"))Queue.Add({Python(),TEXT("-X utf8 ")+Quote(Script())+TEXT(" status --kit ")+Quote(Kit())+TEXT(" --mod ")+ModId+TEXT(" --modes ")+Quote(Modes)+TEXT(" --report ")+Quote(ReportPath),TEXT("Checking the updated minimap")});
    StartNext();
}

void FRetakeModKitState::StartNext()
{
    if (Queue.IsEmpty()) { Finished(0); return; }
    const FCommand Command = Queue[0]; Queue.RemoveAt(0); Phase = Command.Label; OperationFailed = false;
    FPlatformProcess::CreatePipe(ReadPipe, WritePipe);
    Process = FPlatformProcess::CreateProc(*Command.Program, *Command.Arguments, false, true, true, nullptr, 0, *Kit(), WritePipe, nullptr, WritePipe);
    if (!Process.IsValid()) { Queue.Reset(); Finished(-1); }
    ++ChangeSerial; Changed.Broadcast();
}

void FRetakeModKitState::RecordOutput(const FString& Text)
{
    if (Text.IsEmpty()) return;
    FFileHelper::SaveStringToFile(Text, *LastLog, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
    Activity += Text; if (Activity.Len() > 14000) Activity.RightChopInline(Activity.Len() - 14000);
    PendingOutput += Text;
    FString Line, Remaining;
    while (PendingOutput.Split(TEXT("\n"), &Line, &Remaining))
    {
        PendingOutput = Remaining;
        if (Line.StartsWith(TEXT("event=CreatorTask ")))
        {
            TSharedPtr<FJsonObject> Event;
            if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Line.Mid(18)), Event) && Event)
            {
                const FString Task = TextField(Event, TEXT("task"));
                if (Task.Contains(TEXT("Acoustics"))) Phase = TEXT("Baking acoustics");
                else if (Task.Contains(TEXT("Minimap"))) Phase = TEXT("Preparing the minimap");
                else if (Task.Contains(TEXT("Visibility"))) Phase = TextField(Event, TEXT("phase")) == TEXT("Prepare") ? TEXT("Preparing geometry") : TEXT("Preparing visibility");
                else if (Task == TEXT("bake_visibility.py")) Phase = TEXT("Baking visibility");
                else if (Task == TEXT("BuildCookRun")) Phase = Flag(Event, TEXT("server")) ? TEXT("Cooking Linux server content") : TEXT("Cooking Windows content");
                else if (Task.Contains(TEXT("Validate"))) Phase = CurrentAction==TEXT("draft-status")?TEXT("Checking weapon authoring data"):TEXT("Checking map content");
                else if (Task.Contains(TEXT("Manifest"))) Phase = TEXT("Verifying exported content");
            }
        }
        else if (Line.StartsWith(TEXT("event=EngineEvidence"))) Phase = TEXT("Verifying engine compatibility inputs");
        else if (Line.StartsWith(TEXT("{")))
        {
            TSharedPtr<FJsonObject> Result;
            if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Line), Result) && Result && Result->HasField(TEXT("export")))
            { LastExport = TextField(Result, TEXT("export")); LastRecovery = TextField(Result, TEXT("recovery")); }
        }
        else if (Line.StartsWith(TEXT("event=ModKitFailed reason="))) Notice = Line.Mid(26);
    }
}

bool FRetakeModKitState::Tick(float)
{
    if (Process.IsValid())
    {
        RecordOutput(FPlatformProcess::ReadPipe(ReadPipe));
        int32 Code;
        if (FPlatformProcess::GetProcReturnCode(Process, &Code))
        {
            RecordOutput(FPlatformProcess::ReadPipe(ReadPipe));
            FPlatformProcess::CloseProc(Process); Process.Reset();
            FPlatformProcess::ClosePipe(ReadPipe, WritePipe); ReadPipe = WritePipe = nullptr;
            if (Code == 0 && !Queue.IsEmpty()) StartNext(); else { Queue.Reset(); Finished(Code); }
        }
    }
    else if (FPlatformTime::Seconds() >= NextRefresh && !ModId.IsEmpty())
    {
        NextRefresh = FPlatformTime::Seconds() + 3;
        RefreshMinimapState();
        if (!Stale && (HasUnsaved() || SourceStamp() != ValidationStamp))
        { Stale = true; Ready = false; ++ChangeSerial; Changed.Broadcast(); }
    }
    return true;
}

void FRetakeModKitState::Cancel()
{
    if (!Busy()) return;
    FPlatformProcess::TerminateProc(Process, true); FPlatformProcess::WaitForProc(Process); FPlatformProcess::CloseProc(Process); Process.Reset();
    FPlatformProcess::ClosePipe(ReadPipe, WritePipe); ReadPipe = WritePipe = nullptr; Queue.Reset();
    Notice = TEXT("Operation stopped. Existing exports are unchanged; check the workspace before continuing.");
    Finished(-2);
}

void FRetakeModKitState::Finished(int32 Code)
{
    OperationFailed = Code != 0 && Code != -2;
    if (CurrentAction == TEXT("prepare") || CurrentAction == TEXT("minimap")) {RestoreMap();RefreshMinimapState();}
    if (FPaths::FileExists(ReportPath) && (CurrentAction == TEXT("status") || CurrentAction == TEXT("prepare") || CurrentAction == TEXT("minimap"))) ReadReport();
    if(FPaths::FileExists(ReportPath)&&CurrentAction==TEXT("draft-status"))ReadDraftReport();
    if (Code != 0)
    {
        if(CurrentAction==TEXT("status")||CurrentAction==TEXT("prepare")||CurrentAction==TEXT("export")||CurrentAction==TEXT("minimap")||CurrentAction==TEXT("draft-status")){Ready=false;Stale=true;}
        if(CurrentAction==TEXT("test-info"))InvalidateTestCheck();
        if(Notice.IsEmpty())Notice=TEXT("The operation needs attention. Open the activity log for details.");
    }
    else if (CurrentAction == TEXT("export"))
    {
        Notice = TEXT("Export complete. Windows and Linux files and a separate source recovery archive are ready.");
        Revision = TEXT("r") + FDateTime::UtcNow().ToString(TEXT("%Y%m%d%H%M%S"));
        Stale = HasUnsaved() || SourceStamp() != JobStartStamp;
        InvalidateTestCheck(); ExportStamp = JobStartStamp; RememberedExports.Add(ModId,LastExport); RefreshExports(); SavePreferences();
    }
    else if (CurrentAction == TEXT("test-info") || CurrentAction == TEXT("server-status"))
    {
        const auto Result=ReadJSON(ReportPath);
        if(Result)
        {
            if(CurrentAction == TEXT("test-info"))
            {
                TestReady=Flag(Result,TEXT("ready"));TestCheckStamp=JobTestStamp;
                const TSharedPtr<FJsonObject>* Entry=nullptr;
                if(Result->TryGetObjectField(TEXT("client"),Entry)){ClientTestDetail=TextField(*Entry,TEXT("detail"));ClientAllowed=Flag(*Entry,TEXT("allowed"));}
                if(Result->TryGetObjectField(TEXT("server"),Entry)){ServerTestDetail=TextField(*Entry,TEXT("detail"));ServerAllowed=Flag(*Entry,TEXT("allowed"));}
                else ServerAllowed=false;
                Notice=TestReady?TEXT("Selected installed binaries match this export's compatibility records."):TEXT("The selected installations cannot launch this export yet. Review the Test card.");
            }
            const TSharedPtr<FJsonObject>* Server=nullptr;
            if(Result->TryGetObjectField(TEXT("serverStatus"),Server)){ServerStatus=TextField(*Server,TEXT("state"));ServerStatusDetail=TextField(*Server,TEXT("detail"));}
        }
    }
    else if (CurrentAction == TEXT("discover"))
    {
        const auto Found = ReadJSON(ReportPath);
        const TArray<TSharedPtr<FJsonValue>>* Items=nullptr;
        if(Found && Found->TryGetArrayField(TEXT("clients"),Items) && !Items->IsEmpty() && !FPaths::FileExists(GameExe))
            GameExe=TextField((*Items)[0]->AsObject(),TEXT("path"));
        if(Found && Found->TryGetArrayField(TEXT("servers"),Items) && !Items->IsEmpty() && !FPaths::FileExists(ServerPath))
            ServerPath=TextField((*Items)[0]->AsObject(),TEXT("path"));
        InvalidateTestCheck(); SavePreferences(); Notice=TEXT("Installation scan finished. Select an export, then check the installed binaries before testing.");
    }
    else if(CurrentAction==TEXT("draft-status"))Notice=TEXT("Authoring data checked. This does not certify weapon export or runtime readiness.");
    else if (CurrentAction == TEXT("minimap")) Notice=TEXT("Minimap created and checked. Open preview to review the image and alignment.");
    else if (CurrentAction == TEXT("example")) { Refresh(); OpenMap(); Notice = TEXT("Example created. Explore its elements, then create your own mod."); }
    else if (CurrentAction == TEXT("setup")) Notice = TEXT("Authoring tools are ready. Check your map to continue.");
    else if (CurrentAction == TEXT("stop-test-server")) { ServerStatus=TEXT("stopped");ServerStatusDetail=TEXT("The Kit-managed test server has stopped.");Notice=ServerStatusDetail; }
    else if (CurrentAction == TEXT("start-test-server")) {ServerStatus=TEXT("running");ServerStatusDetail=TEXT("The selected map is running on the local test server.");Notice=ServerStatusDetail;}
    else if (CurrentAction == TEXT("test")) { TestLaunched = true; if(UseServer){ServerStatus=TEXT("running");ServerStatusDetail=TEXT("The Kit-managed server was started for this launch.");} Notice = TEXT("Retake launched with the exported mod. Close the test client before starting another."); }
    Phase.Reset(); ++ChangeSerial; Changed.Broadcast();
}

void FRetakeModKitState::ReadDraftReport()
{
    const auto Report=ReadJSON(ReportPath);
    if(!Report||!Flag(Report,TEXT("complete"))||TextField(Report,TEXT("kind"))!=TEXT("weapons")||TextField(Report,TEXT("modId"))!=ModId)return;
    Issues.Reset();Errors=Warnings=0;Ready=false;Valid=Flag(Report,TEXT("valid"));
    const TArray<TSharedPtr<FJsonValue>>* Rows=nullptr;
    if(Report->TryGetArrayField(TEXT("issues"),Rows))for(const auto& Row:*Rows)
    {
        const auto Item=Row->AsObject();const bool Error=Flag(Item,TEXT("error"));Error?++Errors:++Warnings;
        Issues.Add((Error?TEXT("Fix: "):TEXT("Note: "))+TextField(Item,TEXT("actor"))+TEXT(" - ")+TextField(Item,TEXT("detail")));
    }
    ValidationStamp=SourceStamp();Stale=HasUnsaved()||ValidationStamp!=JobStartStamp;
}

void FRetakeModKitState::ReadReport()
{
    const TSharedPtr<FJsonObject> Report = ReadJSON(ReportPath);
    if (!Report || !Flag(Report, TEXT("complete")) || TextField(Report, TEXT("modId")) != ModId) return;
    Issues.Reset(); Errors = Warnings = 0; Valid = Flag(Report, TEXT("valid")); Ready = Flag(Report, TEXT("ready")); Candidate = Flag(Report, TEXT("candidateOnly"));
    PreparationCurrent = Flag(Report, TEXT("preparationComplete")); PreparationNeeded = Flag(Report, TEXT("preparationNeeded"));
    PreparationBlocked = Flag(Report, TEXT("preparationBlocked")); PreparationChecks.Reset(); MapModes.Reset();
    for (FRetakeCreatorCheck& Check : Checks) { Check.State = 1; Check.Detail = TEXT("Passed"); }
    const auto AddIssues = [&](const TArray<TSharedPtr<FJsonValue>>* Values)
    {
        if (!Values) return;
        for (const auto& Value : *Values)
        {
            const auto Issue = Value->AsObject(); const bool Error = Flag(Issue, TEXT("error"));
            Error ? ++Errors : ++Warnings;
            const FString Code = TextField(Issue, TEXT("code")), Actor = TextField(Issue, TEXT("actor"));
            Issues.Add((Error ? TEXT("Fix: ") : TEXT("Note: ")) + (Actor.IsEmpty() ? FString() : Actor + TEXT(" - ")) + TextField(Issue, TEXT("detail")));
            int32 Index = Code == TEXT("Dependencies") ? 5 : Code == TEXT("Audio") || Code.Contains(TEXT("Acoustic")) ? 3
                : Code.Contains(TEXT("Visibility")) ? 4 : Code == TEXT("Starts") || Code == TEXT("ObjectiveId") || Code == TEXT("Modes") ? 1 : 2;
            Checks[Index].State = FMath::Max(Checks[Index].State, Error ? 3 : 2);
            Checks[Index].Detail = Error ? TEXT("Needs a fix") : TEXT("Warnings to review");
        }
    };
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (Report->TryGetArrayField(TEXT("issues"), Values)) AddIssues(Values);
    if (Report->TryGetArrayField(TEXT("maps"), Values)) for (const auto& Value : *Values)
    {
        const auto Map = Value->AsObject(); const TArray<TSharedPtr<FJsonValue>>* MapIssues = nullptr;
        if (Map->TryGetArrayField(TEXT("issues"), MapIssues)) AddIssues(MapIssues);
        const TArray<TSharedPtr<FJsonValue>>* ModesInMap = nullptr;
        if (Map->TryGetArrayField(TEXT("modes"), ModesInMap))
        { TArray<FString> Names; for (const auto& Mode : *ModesInMap) Names.Add(Mode->AsString()); MapModes.Add(TextField(Map,TEXT("name")), FString::Join(Names,TEXT(","))); }
        const TArray<TSharedPtr<FJsonValue>>* Preparation = nullptr;
        if (Map->TryGetArrayField(TEXT("preparation"), Preparation)) for (const auto& Step : *Preparation)
        {
            const auto Item = Step->AsObject(); const FString Id = TextField(Item,TEXT("id")), StepState = TextField(Item,TEXT("state"));
            const FString Title = Id == TEXT("acoustics") ? TEXT("Acoustics") : Id == TEXT("minimap") ? TEXT("Minimap (optional)") : TEXT("Geometry & visibility");
            PreparationChecks.Add({TextField(Map,TEXT("name")) + TEXT(" / ") + Title, TextField(Item,TEXT("detail")),
                StepState == TEXT("current") ? 1 : StepState == TEXT("needed") ? 2 : StepState == TEXT("blocked") ? 3 : 4});
        }
    }
    if (!Flag(Report, TEXT("preparationComplete"))) Checks[4] = {TEXT("Visibility bake"), TEXT("Prepare the map"), 3};
    Checks[6] = {TEXT("Game compatibility"), Candidate ? TEXT("Local test exports") : TEXT("Configured; verified at export"), Candidate ? 2 : 1};
    if (!Flag(Report, TEXT("compatibilityConfigured")) || !Flag(Report, TEXT("sdkCurrent")))
        Checks[6] = {TEXT("Game compatibility"), TEXT("Kit update or qualification needed"), 3};
    ValidationStamp = SourceStamp(); Stale = HasUnsaved() || (CurrentAction == TEXT("status") && ValidationStamp != JobStartStamp);
    if (Stale) Ready = false;
}

FString FRetakeModKitState::StatusTitle() const
{
    if (!IsKit()) return TEXT("Open the Retake Mod Kit project");
    if (Busy()) return Phase;
    if(!IsMapWorkspace())
    {
        if(ActiveKind==TEXT("game-mode"))return TEXT("Game-mode mods: planned");
        if(!CanAuthorCurrentKind())return TEXT("Weapon authoring SDK required");
        return ModId.IsEmpty()?TEXT("Create or choose a weapon draft"):TEXT("Weapon pack authoring draft");
    }
    if (ModId.IsEmpty()) return TEXT("Your next map starts here");
    if (!HasTools()) return TEXT("Set up your authoring tools");
    if (OperationFailed) return CurrentAction == TEXT("status") ? TEXT("Map check could not complete") : TEXT("Operation could not complete");
    if(IsTestingWorkflow())
    {
        if(TestLaunched)return TEXT("Test client launched");
        if(CurrentAction==TEXT("server-status")||CurrentAction==TEXT("stop-test-server")||CurrentAction==TEXT("start-test-server"))return TEXT("Local test server status");
        return TestReady&&TestCheckCurrent()?TEXT("Selected export is ready to test"):TEXT("Review the selected test installations");
    }
    if (Stale) return TEXT("Check your map's readiness");
    if (Ready) return ExportIsCurrent() ? TEXT("Next: check installations and test") : TEXT("Next: export your mod");
    if (PreparationBlocked) return TEXT("Next: fix preparation setup");
    if (PreparationNeeded) return TEXT("Next: prepare your map");
    return Errors ? FString::Printf(TEXT("%d things need attention"), Errors) : TEXT("Review game compatibility");
}
FString FRetakeModKitState::StatusDetail() const
{
    if (!IsKit()) return TEXT("This workspace creates content in the standalone RetakeModKit project.");
    if (Busy()) return TEXT("Progress and details appear below. You can stop this operation safely.");
    if (!Notice.IsEmpty()) return Notice;
    if(!IsMapWorkspace())return KindDescription();
    if (Stale) return TEXT("Save your changes, then check the map. Preparation and export use the saved content.");
    if (Ready) return Candidate ? TEXT("This development kit produces local test bundles. Public compatibility is not yet qualified.")
        : TEXT("Source checks passed. Export will cook and verify Windows and Linux content.");
    return TEXT("Use the checklist and issues below to see the next step.");
}

void FRetakeModKitState::DiscoverInstallations()
{
    if(Busy() || !IsKit()) return;
    FString Interpreter=Python();
    if(!FPaths::FileExists(Interpreter)) Interpreter=FPaths::ConvertRelativePathToFull(FPaths::EngineDir()/TEXT("Binaries/ThirdParty/Python3/Win64/python.exe"));
    if(!FPaths::FileExists(Interpreter)) { Notice=TEXT("Choose your existing game installations with Browse."); return; }
    CurrentAction=TEXT("discover");
    JobDirectory=Kit()/TEXT("Saved/ModKit/EditorJobs")/FGuid::NewGuid().ToString(EGuidFormats::Digits);
    IFileManager::Get().MakeDirectory(*JobDirectory,true); LastLog=JobDirectory/TEXT("activity.log"); ReportPath=JobDirectory/TEXT("installations.json");
    Queue={{Interpreter,TEXT("-X utf8 ")+Quote(Script())+TEXT(" discover --kit ")+Quote(Kit())+TEXT(" --report ")+Quote(ReportPath),TEXT("Finding installed Retake")}};
    StartNext();
}

void FRetakeModKitState::ChooseServer()
{
    FString Folder;
    if(IDesktopPlatform* Desktop=FDesktopPlatformModule::Get())
        if(Desktop->OpenDirectoryDialog(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),TEXT("Choose the installed Linux server folder"),FPaths::GetPath(ServerPath),Folder))
        {
            const FString Launcher=Folder/TEXT("RetakeServer.sh");
            if(!FPaths::FileExists(Launcher)) { Notice=TEXT("Select the server folder containing RetakeServer.sh. No server installation is needed here."); return; }
            ServerPath=FPaths::ConvertRelativePathToFull(Launcher); InvalidateTestCheck(); SavePreferences();
        }
}

void FRetakeModKitState::ChooseGame()
{
    TArray<FString> Files;
    if (IDesktopPlatform* Desktop = FDesktopPlatformModule::Get())
        if (Desktop->OpenFileDialog(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr), TEXT("Choose installed Retake"),
            FPaths::GetPath(GameExe), TEXT(""), TEXT("Retake executable (*.exe)|*.exe"), 0, Files) && !Files.IsEmpty())
        { GameExe = FPaths::ConvertRelativePathToFull(Files[0]); InvalidateTestCheck(); SavePreferences(); }
}
void FRetakeModKitState::OpenOutput() const { if (!LastExport.IsEmpty()) FPlatformProcess::ExploreFolder(*LastExport); }
void FRetakeModKitState::OpenLog() const { if (!LastLog.IsEmpty()) FPlatformProcess::LaunchFileInDefaultExternalApplication(*LastLog); }

const TArray<FRetakeCreatorElement>& FRetakeModKitState::Elements()
{
    static const TArray<FRetakeCreatorElement> Values = {
        {TEXT("Map settings"),TEXT("Map setup"),TEXT("/Script/RetakeMapSDK.RetakeMapSettings")},
        {TEXT("Shared player start"),TEXT("Spawns"),TEXT("/Script/RetakeMapSDK.RetakeModePlayerStart"),1},
        {TEXT("Team 1 start"),TEXT("Spawns"),TEXT("/Script/RetakeMapSDK.RetakeModePlayerStart"),2},
        {TEXT("Team 2 start"),TEXT("Spawns"),TEXT("/Script/RetakeMapSDK.RetakeModePlayerStart"),3},
        {TEXT("Attacker start"),TEXT("Spawns"),TEXT("/Script/RetakeMapSDK.RetakeModePlayerStart"),4},
        {TEXT("Defender start"),TEXT("Spawns"),TEXT("/Script/RetakeMapSDK.RetakeModePlayerStart"),5},
        {TEXT("Objective site"),TEXT("Objectives"),TEXT("/Script/RetakeMapSDK.RetakeModeObjectiveMarker"),10},
        {TEXT("Control point"),TEXT("Objectives"),TEXT("/Script/RetakeMapSDK.RetakeModeObjectiveMarker"),11},
        {TEXT("Team 1 flag base"),TEXT("Objectives"),TEXT("/Script/RetakeMapSDK.RetakeModeObjectiveMarker"),12},
        {TEXT("Team 2 flag base"),TEXT("Objectives"),TEXT("/Script/RetakeMapSDK.RetakeModeObjectiveMarker"),13},
        {TEXT("Team 1 capture zone"),TEXT("Objectives"),TEXT("/Script/RetakeMapSDK.RetakeModeObjectiveMarker"),14},
        {TEXT("Team 2 capture zone"),TEXT("Objectives"),TEXT("/Script/RetakeMapSDK.RetakeModeObjectiveMarker"),15},
        {TEXT("Detonation target"),TEXT("Objectives"),TEXT("/Script/RetakeMapSDK.RetakeDetonationTarget")},
        {TEXT("Freeze Tag relay"),TEXT("Objectives"),TEXT("/Script/RetakeMapSDK.RetakeFreezeTagRelay")},
        {TEXT("Door"),TEXT("Interaction"),TEXT("/Script/RetakeMapSDK.RetakeDoor")},
        {TEXT("Ladder"),TEXT("Interaction"),TEXT("/Script/RetakeMapSDK.LadderActor")},
        {TEXT("Destructible"),TEXT("Interaction"),TEXT("/Script/RetakeDestruction.RetakeDestructiblePlacement")},
        {TEXT("Minefield"),TEXT("Interaction"),TEXT("/Script/RetakeMapSDK.RetakeMinefield")},
        {TEXT("Ambient sound"),TEXT("Environment"),TEXT("/Script/Engine.AmbientSound")},
        {TEXT("Acoustic map"),TEXT("Environment"),TEXT("/Script/RetakeMapSDK.RetakeAcousticMap")},
        {TEXT("Acoustic region"),TEXT("Environment"),TEXT("/Script/RetakeMapSDK.RetakeAcousticRegion")},
        {TEXT("Acoustic connection"),TEXT("Environment"),TEXT("/Script/RetakeMapSDK.RetakeAcousticConnection")},
        {TEXT("Minimap capture"),TEXT("Presentation"),TEXT("/Script/RetakeMapSDK.RetakeMinimapCapture")},
        {TEXT("Spectator view"),TEXT("Presentation"),TEXT("/Script/RetakeMapSDK.RetakeSpectatorOverview")},
        {TEXT("Ambience profile"),TEXT("Content assets"),TEXT("/Script/RetakeMapSDK.RetakeAmbienceProfile"),-1},
        {TEXT("Destruction definition"),TEXT("Content assets"),TEXT("/Script/RetakeDestruction.RetakeDestructionDefinition"),-1}};
    return Values;
}

void FRetakeModKitState::Place(int32 Index)
{
    if (!IsKit() || Busy() || !Elements().IsValidIndex(Index) || ModId.IsEmpty() || GEditor->PlayWorld) return;
    const auto& Entry = Elements()[Index]; UClass* Class = LoadClass<UObject>(nullptr, Entry.Class);
    if (!Class) { Notice = TEXT("This element is not available in the current kit."); return; }
    if (Entry.Preset == -1)
    {
        if (!Class->IsChildOf(UDataAsset::StaticClass())) { Notice = TEXT("This asset type is unavailable."); return; }
        UDataAssetFactory* Factory = NewObject<UDataAssetFactory>(); Factory->DataAssetClass = Class;
        FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().CreateAssetWithDialog(
            TEXT("DA_") + FString(Class->GetName()), TEXT("/") + ModId + TEXT("/Data"), UDataAsset::StaticClass(), Factory);
        Stale = true; Ready = false; return;
    }
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World || !World->GetOutermost()->GetName().StartsWith(TEXT("/") + ModId + TEXT("/")))
    { Notice = TEXT("Open a map from this mod before placing an element."); return; }
    FVector Location = FVector::ZeroVector; FRotator Rotation = FRotator::ZeroRotator;
    if (GCurrentLevelEditingViewportClient)
    {
        const FVector Start = GCurrentLevelEditingViewportClient->GetViewLocation();
        const FVector Direction = GCurrentLevelEditingViewportClient->GetViewRotation().Vector();
        FHitResult Hit; Location = Start + Direction * 400;
        if (World->LineTraceSingleByChannel(Hit, Start, Start + Direction * 100000, ECC_Visibility)) Location = Hit.ImpactPoint;
        Rotation.Yaw = GCurrentLevelEditingViewportClient->GetViewRotation().Yaw;
    }
    const FScopedTransaction Transaction(Label(TEXT("Place ") + FString(Entry.Name)));
    AActor* Actor = GEditor->AddActor(World->GetCurrentLevel(), Class, FTransform(Rotation, Location), true, RF_Transactional);
    if (!Actor) { Notice = TEXT("The element could not be placed."); return; }
    if (auto* Start = Cast<ARetakeModePlayerStart>(Actor))
    {
        Start->StartRole = Entry.Preset <= 1 ? ERetakeModeStartRole::Shared : Entry.Preset <= 3 ? ERetakeModeStartRole::Team
            : Entry.Preset == 4 ? ERetakeModeStartRole::Attacker : ERetakeModeStartRole::Defender;
        Start->TeamId = Entry.Preset == 2 ? 0 : Entry.Preset == 3 ? 1 : INDEX_NONE;
        Start->AddActorWorldOffset(FVector(0,0,Start->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()+5));
    }
    if (auto* Settings = Cast<ARetakeMapSettings>(Actor)) Settings->DisplayName = ModId.RightChop(10);
    if (auto* Objective = Cast<ARetakeModeObjectiveMarker>(Actor))
    { Objective->Kind = Entry.Preset == 11 ? ERetakeModeMarkerKind::ControlPoint : Entry.Preset == 12 || Entry.Preset == 13 ? ERetakeModeMarkerKind::TeamFlagHome
        : Entry.Preset == 14 || Entry.Preset == 15 ? ERetakeModeMarkerKind::CaptureZone : ERetakeModeMarkerKind::BombSite;
        Objective->TeamIndex = Entry.Preset == 12 || Entry.Preset == 14 ? 0 : Entry.Preset == 13 || Entry.Preset == 15 ? 1 : INDEX_NONE;
        Objective->ObjectiveId = Actor->GetFName(); }
    Actor->SetActorLabel(Entry.Name); Actor->MarkPackageDirty();
    GEditor->SelectNone(false,true); GEditor->SelectActor(Actor,true,true); GEditor->NoteSelectionChange();
    Notice = TEXT("Element placed. Configure it in Details, then check the map."); Stale = true; Ready = false;
}
