// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeMapValidator.h"
#include "RetakeAcousticMap.h"
#include "RetakeAmbienceProfile.h"
#include "RetakeAmbienceZoneComponent.h"
#include "RetakeDetonationTarget.h"
#include "RetakeFreezeTagRelay.h"
#include "RetakeMapSDKManifest.h"
#include "RetakeMapSDKVersion.h"
#include "RetakeMapSettings.h"
#include "RetakeLoadoutPolicy.h"
#include "RetakeVisibilityData.h"
#include "RetakeMinefield.h"
#include "RetakeMinimapBaker.h"
#include "RetakeMinimapCapture.h"
#include "RetakeMinimapDefinition.h"
#include "RetakeModeObjectiveMarker.h"
#include "RetakeModePlayerStart.h"
#include "RetakeSpectatorOverview.h"
#include "RetakeTraversalObstacleComponent.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/AudioComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "ActorEditorUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphNode_Comment.h"
#include "Engine/CollisionProfile.h"
#include "EngineDefines.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptActor.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/KillZVolume.h"
#include "GameFramework/PainCausingVolume.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/DataValidation.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "Sound/AmbientSound.h"
#include "Sound/SoundBase.h"
#include "TextureCompiler.h"

namespace
{
    /** An actor whose nearest native class comes from an element package (RetakeMapSDK::ElementScriptPackages); the editor module never links the element plugins. */
    bool IsElement(const AActor* Actor)
    {
        // The minimap capture is an SDK authoring helper: editor only by design, never part of the map.
        if (Actor->IsA<ARetakeMinimapCapture>()) return false;
        const UClass* Native = Actor->GetClass();
        while (!Native->HasAnyClassFlags(CLASS_Native)) Native = Native->GetSuperClass();
        const FString Package = Native->GetPackage()->GetName();
        for (const TCHAR* ElementPackage : RetakeMapSDK::ElementScriptPackages)
            if (Package == ElementPackage) return true;
        return false;
    }

    template<class T>
    TArray<const T*> Placed(const UWorld& World)
    {
        TArray<const T*> Out;
        if (World.PersistentLevel)
            for (const AActor* Actor : World.PersistentLevel->Actors)
                if (const T* Typed = Cast<T>(Actor)) Out.Add(Typed);
        return Out;
    }

    void Add(TArray<FRetakeMapIssue>& Out, bool bError, const TCHAR* Code, const AActor* Actor, const FString& Detail)
    {
        FRetakeMapIssue& Issue = Out.AddDefaulted_GetRef();
        Issue.bError = bError; Issue.Code = Code; Issue.Actor = Actor ? Actor->GetActorNameOrLabel() : FString(); Issue.Detail = Detail;
    }

    /** Object's own checks (IsDataValid): an element actor, or a component element such as a traversal obstacle on Actor. */
    void AddDataValidation(TArray<FRetakeMapIssue>& Out, const AActor* Actor, const UObject& Object, bool bWarnings)
    {
        FDataValidationContext Context;
        Object.IsDataValid(Context);
        for (const FDataValidationContext::FIssue& Issue : Context.GetIssues())
            if (Issue.Severity == EMessageSeverity::Error || (bWarnings && Issue.Severity == EMessageSeverity::Warning))
                Add(Out, Issue.Severity == EMessageSeverity::Error, TEXT("Element"), Actor,
                    Issue.TokenizedMessage ? Issue.TokenizedMessage->ToText().ToString() : Issue.Message.ToString());
    }

    /** Unique, non-empty IDs among Markers. */
    void CheckIds(const TArray<const ARetakeModeObjectiveMarker*>& Markers, TArray<FRetakeMapIssue>& Out)
    {
        TSet<FName> Ids;
        for (const ARetakeModeObjectiveMarker* Marker : Markers)
        {
            if (Marker->ObjectiveId.IsNone() || Ids.Contains(Marker->ObjectiveId))
                Add(Out, true, TEXT("ObjectiveId"), Marker, TEXT("needs a unique, non-empty Objective Id."));
            Ids.Add(Marker->ObjectiveId);
        }
    }
}

void RetakeMapValidator::ValidateWorld(const UWorld& World, TArray<FRetakeMapIssue>& Out)
{
    if (World.GetWorldPartition()) Add(Out, true, TEXT("Level"), nullptr, TEXT("World Partition maps are not supported; use a single persistent level."));
    if (!World.GetStreamingLevels().IsEmpty()) Add(Out, true, TEXT("Level"), nullptr, TEXT("Streaming sublevels are not supported; keep every actor in the persistent level."));
    if (World.PersistentLevel && World.PersistentLevel->IsUsingExternalActors())
        Add(Out, true, TEXT("Level"), nullptr, TEXT("One File Per Actor is not supported; save the map as a single package."));
    if (!World.PersistentLevel) return;
    // Retake chooses the game mode; a map override would replace it whenever a server opens the map without ?game=.
    if (const AWorldSettings* Settings = World.GetWorldSettings(false, false); Settings && Settings->DefaultGameMode)
        Add(Out, true, TEXT("GameMode"), nullptr, FString::Printf(TEXT("World Settings > GameMode Override is %s; leave it empty, Retake chooses the game mode."),
            *Settings->DefaultGameMode.Get()->GetName()));
    // Maps carry no gameplay scripts (MKD-009): an empty Level Blueprint and native actor classes only.
    if (const ULevelScriptBlueprint* LevelScript = World.PersistentLevel->GetLevelScriptBlueprint(true))
    {
        TArray<UEdGraph*> Graphs;
        LevelScript->GetAllGraphs(Graphs);
        const bool bScripted = Graphs.ContainsByPredicate([](const UEdGraph* Graph)
        {
            return Graph && Graph->Nodes.ContainsByPredicate([](const UEdGraphNode* Node)
                { return Node && !Node->IsAutomaticallyPlacedGhostNode() && !Node->IsA<UEdGraphNode_Comment>(); });
        });
        if (bScripted) Add(Out, true, TEXT("Script"), nullptr, TEXT("the Level Blueprint has nodes; maps carry no gameplay scripts, so leave it empty."));
    }
    for (const AActor* Actor : World.PersistentLevel->Actors)
    {
        if (Actor && !Actor->GetClass()->HasAnyClassFlags(CLASS_Native) && !Actor->IsA<ALevelScriptActor>())
            Add(Out, true, TEXT("Script"), Actor, FString::Printf(TEXT("is a %s Blueprint; maps place native classes only (no gameplay scripts)."), *Actor->GetClass()->GetName()));
        if (!Actor) continue;
        // Traversal ledges are a component element on any actor, such as a placed static mesh.
        for (const URetakeTraversalObstacleComponent* Obstacle : TInlineComponentArray<URetakeTraversalObstacleComponent*>(Actor))
            AddDataValidation(Out, Actor, *Obstacle, true);
        if (!IsElement(Actor)) continue;
        if (Actor->IsEditorOnly()) Add(Out, true, TEXT("EditorOnly"), Actor, TEXT("map elements must exist in the game, not only in the editor."));
        // Errors only: an element actor's checks also run the engine's component checks.
        AddDataValidation(Out, Actor, *Actor, false);
    }
    if (Placed<ARetakeMapSettings>(World).Num() > 1) Add(Out, true, TEXT("MapSettings"), nullptr, TEXT("place at most one Retake Map Settings actor."));
    if (const int32 Overviews = Placed<ARetakeSpectatorOverview>(World).Num(); Overviews > 1)
        Add(Out, true, TEXT("Overview"), nullptr, TEXT("place at most one Retake Spectator Overview."));
    else if (!Overviews)
        Add(Out, false, TEXT("Overview"), nullptr, TEXT("no Retake Spectator Overview: spectators watch from above the map origin. Place one."));
    ValidateEnvironment(World, Out);
}

bool RetakeMapValidator::ValidateMode(const UWorld& World, const RetakeMapSDK::FModeRequirement& Mode, TArray<FRetakeMapIssue>& Out)
{
    const int32 ErrorsBefore = Out.FilterByPredicate([](const FRetakeMapIssue& I) { return I.bError; }).Num();
    const FString Prefix = FString(Mode.ModeId) + TEXT(": ");
    const auto Need = [&](bool bError, const TCHAR* Code, const FString& Detail) { Add(Out, bError, Code, nullptr, Prefix + Detail); };

    // Starts, counted the way the game's start rules accept them.
    const TArray<const ARetakeModePlayerStart*> Starts = Placed<ARetakeModePlayerStart>(World);
    const auto Count = [&Starts](TFunctionRef<bool(const ARetakeModePlayerStart&)> Accept)
    { int32 N = 0; for (const ARetakeModePlayerStart* Start : Starts) N += Accept(*Start) ? 1 : 0; return N; };
    const auto Sides = [&](const TCHAR* Side, const int32 (&Counts)[2])
    {
        for (int32 I = 0; I < 2; ++I)
        {
            if (Counts[I] < Mode.MinStarts) Need(true, TEXT("Starts"), FString::Printf(TEXT("%s %d needs at least %d start(s); it has %d."), Side, I, Mode.MinStarts, Counts[I]));
            else if (Counts[I] < Mode.AdvisedStarts) Need(false, TEXT("Starts"), FString::Printf(TEXT("%s %d has %d start(s); %d are advised."), Side, I, Counts[I], Mode.AdvisedStarts));
        }
    };
    switch (Mode.Starts)
    {
    case RetakeMapSDK::EModeStartRule::AnyRole:
        if (Starts.Num() < Mode.MinStarts) Need(true, TEXT("Starts"), FString::Printf(TEXT("needs at least %d player starts; the map has %d."), Mode.MinStarts, Starts.Num()));
        else if (Starts.Num() < Mode.AdvisedStarts) Need(false, TEXT("Starts"), FString::Printf(TEXT("%d player starts; %d are advised."), Starts.Num(), Mode.AdvisedStarts));
        break;
    case RetakeMapSDK::EModeStartRule::SharedOrTeam:
    {
        const int32 Counts[2] = {
            Count([](const ARetakeModePlayerStart& S) { return S.StartRole == ERetakeModeStartRole::Shared || (S.StartRole == ERetakeModeStartRole::Team && S.TeamId == 0); }),
            Count([](const ARetakeModePlayerStart& S) { return S.StartRole == ERetakeModeStartRole::Shared || (S.StartRole == ERetakeModeStartRole::Team && S.TeamId == 1); }) };
        Sides(TEXT("team"), Counts);
        break;
    }
    case RetakeMapSDK::EModeStartRule::TeamOnly:
    {
        const int32 Counts[2] = {
            Count([](const ARetakeModePlayerStart& S) { return S.StartRole == ERetakeModeStartRole::Team && S.TeamId == 0; }),
            Count([](const ARetakeModePlayerStart& S) { return S.StartRole == ERetakeModeStartRole::Team && S.TeamId == 1; }) };
        Sides(TEXT("spawn end"), Counts);
        break;
    }
    case RetakeMapSDK::EModeStartRule::AttackerDefender:
    {
        const int32 Attackers = Count([](const ARetakeModePlayerStart& S) { return S.StartRole == ERetakeModeStartRole::Attacker; });
        const int32 Defenders = Count([](const ARetakeModePlayerStart& S) { return S.StartRole == ERetakeModeStartRole::Defender; });
        for (const TPair<const TCHAR*, int32>& Role : { TPair<const TCHAR*, int32>(TEXT("Attacker"), Attackers), TPair<const TCHAR*, int32>(TEXT("Defender"), Defenders) })
        {
            if (Role.Value < Mode.MinStarts) Need(true, TEXT("Starts"), FString::Printf(TEXT("needs at least %d %s start(s); it has %d."), Mode.MinStarts, Role.Key, Role.Value));
            else if (Role.Value < Mode.AdvisedStarts) Need(false, TEXT("Starts"), FString::Printf(TEXT("%d %s start(s); %d are advised."), Role.Value, Role.Key, Mode.AdvisedStarts));
        }
        if (Placed<ARetakeMapSettings>(World).IsEmpty())
            Need(false, TEXT("MapSettings"), TEXT("no Retake Map Settings: Security defends. Place one to choose the defending faction."));
        break;
    }
    }
    if (Mode.bRequireSharedStart && !Count([](const ARetakeModePlayerStart& S) { return S.StartRole == ERetakeModeStartRole::Shared; }))
        Need(true, TEXT("Starts"), TEXT("needs a Shared start: presets that thaw players at shared spawns require one."));

    // Objectives of the kinds the mode uses.
    TArray<const ARetakeModeObjectiveMarker*> Sites, Points, Flags, Zones;
    for (const ARetakeModeObjectiveMarker* Marker : Placed<ARetakeModeObjectiveMarker>(World))
    {
        switch (Marker->Kind)
        {
        case ERetakeModeMarkerKind::BombSite: Sites.Add(Marker); break;
        case ERetakeModeMarkerKind::ControlPoint: Points.Add(Marker); break;
        case ERetakeModeMarkerKind::CaptureZone: Zones.Add(Marker); break;
        default: Flags.Add(Marker); break;
        }
    }
    if (Mode.MinBombSites > 0)
    {
        if (Sites.Num() < Mode.MinBombSites) Need(true, TEXT("Objectives"), FString::Printf(TEXT("needs at least %d bomb site(s)."), Mode.MinBombSites));
        CheckIds(Sites, Out);
        for (const ARetakeModeObjectiveMarker* Site : Sites)
            if (Site->DetonationMode == ERetakeUplinkDetonationMode::TriggerTarget
                && (!IsValid(Site->DetonationTarget) || Site->DetonationTarget->GetLevel() != World.PersistentLevel))
                Add(Out, true, TEXT("Objectives"), Site, Prefix + TEXT("Trigger Target needs a detonation target in this level."));
    }
    if (Mode.MinControlPoints > 0)
    {
        if (Points.Num() < Mode.MinControlPoints) Need(true, TEXT("Objectives"), FString::Printf(TEXT("needs at least %d control point(s)."), Mode.MinControlPoints));
        CheckIds(Points, Out);
    }
    if (Mode.bCaptureTheFlag)
    {
        // One neutral flag, or one flag home per team; every team needs a capture zone or its own flag home.
        TArray<const ARetakeModeObjectiveMarker*> Ctf = Flags; Ctf.Append(Zones);
        CheckIds(Ctf, Out);
        const bool bNeutral = Flags.Num() == 1 && Flags[0]->Kind == ERetakeModeMarkerKind::NeutralFlagHome;
        TSet<int32> FlagTeams, ZoneTeams;
        bool bFlagsValid = bNeutral || Flags.Num() == 2;
        for (const ARetakeModeObjectiveMarker* Flag : Flags)
            if (!bNeutral && (Flag->Kind != ERetakeModeMarkerKind::TeamFlagHome || Flag->TeamIndex < 0 || Flag->TeamIndex > 1 || FlagTeams.Contains(Flag->TeamIndex)))
                bFlagsValid = false;
            else FlagTeams.Add(Flag->TeamIndex);
        if (!bFlagsValid) Need(true, TEXT("Objectives"), TEXT("needs exactly one neutral flag home, or one team flag home for each of teams 0 and 1."));
        for (const ARetakeModeObjectiveMarker* Zone : Zones)
        {
            if (Zone->TeamIndex < 0 || Zone->TeamIndex > 1 || ZoneTeams.Contains(Zone->TeamIndex))
                Add(Out, true, TEXT("Objectives"), Zone, Prefix + TEXT("capture zones need unique team indices 0 or 1."));
            ZoneTeams.Add(Zone->TeamIndex);
        }
        for (int32 Team = 0; Team < 2; ++Team)
            if (!ZoneTeams.Contains(Team) && (bNeutral || !FlagTeams.Contains(Team)))
                Need(true, TEXT("Objectives"), FString::Printf(TEXT("team %d needs a capture zone."), Team));
    }
    if (Mode.MinRelays > 0)
    {
        const TArray<const ARetakeFreezeTagRelay*> Relays = Placed<ARetakeFreezeTagRelay>(World);
        if (Relays.Num() < Mode.MinRelays) Need(true, TEXT("Relays"), FString::Printf(TEXT("needs at least %d Freeze Tag relay(s)."), Mode.MinRelays));
        TSet<FName> Ids;
        for (const ARetakeFreezeTagRelay* Relay : Relays)
        {
            if (Relay->RelayId.IsNone() || Ids.Contains(Relay->RelayId)) Add(Out, true, TEXT("Relays"), Relay, Prefix + TEXT("relays need unique, non-empty Relay Ids."));
            Ids.Add(Relay->RelayId);
        }
    }
    return Out.FilterByPredicate([](const FRetakeMapIssue& I) { return I.bError; }).Num() == ErrorsBefore;
}

namespace
{
    /** Named in UPhysicsSettings::PhysicalSurfaces, which the authoring profile hash freezes between kit and game; empty when unnamed. */
    FString SurfaceName(EPhysicalSurface Surface)
    {
        const FPhysicalSurfaceName* Named = GetDefault<UPhysicsSettings>()->PhysicalSurfaces.FindByPredicate(
            [Surface](const FPhysicalSurfaceName& Entry) { return Entry.Type == Surface; });
        return Named ? Named->Name.ToString() : FString();
    }

    /**
     * What is wrong with Material as world geometry, empty when nothing: an unnamed surface or Character (bError; the game reads
     * Character as a player), or SurfaceType_Default (not bError; it sounds like Concrete but stops every bullet).
     */
    FString SurfaceProblem(const UPhysicalMaterial* Material, bool& bError)
    {
        const EPhysicalSurface Surface = UPhysicalMaterial::DetermineSurfaceType(Material);
        bError = false;
        if (Surface == SurfaceType_Default) return TEXT("SurfaceType_Default");
        const FString Name = SurfaceName(Surface);
        bError = Name.IsEmpty() || Name == TEXT("Character");
        if (!bError) return FString();
        return Name.IsEmpty() ? FString::Printf(TEXT("unnamed surface type %d"), int32(Surface)) : FString(TEXT("the Character surface"));
    }

    /** A Play When Silent voice never stops; 112 such emitters once held all 32 mixer voices (Docs/Systems/Audio/MapAmbience.md). */
    void CheckVirtualization(const USoundBase* Sound, const AActor* Actor, const FString& User, TArray<FRetakeMapIssue>& Out)
    {
        if (Sound && Sound->VirtualizationMode == EVirtualizationMode::PlayWhenSilent)
            Add(Out, true, TEXT("Audio"), Actor, FString::Printf(TEXT("%s plays %s, whose Virtualization Mode is Play When Silent; use Restart."), *User, *Sound->GetName()));
    }

    /** The checks the game runs on every profile it loads, plus virtualization. */
    void CheckProfile(const URetakeAmbienceProfile& Profile, const AActor* Actor, TArray<FRetakeMapIssue>& Out)
    {
        TArray<FString> Errors;
        Profile.ValidateProfile(Errors);
        for (const FString& Error : Errors) Add(Out, true, TEXT("Audio"), Actor, FString::Printf(TEXT("ambience profile %s: %s"), *Profile.GetName(), *Error));
        const FString User = TEXT("ambience profile ") + Profile.GetName();
        for (const FRetakeAmbienceBed& Bed : Profile.Beds) CheckVirtualization(Bed.Sound, Actor, User, Out);
        for (const FRetakeAmbienceElement& Element : Profile.Elements)
            for (const USoundBase* Sound : Element.Sounds) CheckVirtualization(Sound, Actor, User, Out);
    }

    bool HasUITextureSettings(const UTexture2D& Texture)
    {
        return Texture.CompressionSettings == TC_EditorIcon && Texture.LODGroup == TEXTUREGROUP_UI && Texture.MipGenSettings == TMGS_NoMipmaps
            && Texture.NeverStream && Texture.SRGB;
    }
    const TCHAR* const UITextureSettings = TEXT("compression UserInterface2D, texture group UI, no mipmaps, Never Stream and sRGB");

    /** A presentation texture inside the mod with the UI settings, at most MaxSize per side, ideally 16:9. Returns its object path. */
    FString CheckTexture(const ARetakeMapSettings& Settings, const TSoftObjectPtr<UTexture2D>& Reference, const FString& ModRoot, int32 MaxSize,
        const TCHAR* Field, TArray<FRetakeMapIssue>& Out)
    {
        if (Reference.IsNull()) return FString();
        const FSoftObjectPath Path = Reference.ToSoftObjectPath();
        const UTexture2D* Texture = Path.GetLongPackageName().StartsWith(ModRoot) ? Reference.LoadSynchronous() : nullptr;
        if (!Texture) Add(Out, true, TEXT("Presentation"), &Settings, FString::Printf(TEXT("%s must be a texture inside %s."), Field, *ModRoot));
        else
        {
            if (!HasUITextureSettings(*Texture)) Add(Out, true, TEXT("Presentation"), &Settings, FString::Printf(TEXT("%s needs %s."), Field, UITextureSettings));
            const int32 Width = Texture->Source.GetSizeX(), Height = Texture->Source.GetSizeY();
            if (Width > MaxSize || Height > MaxSize)
                Add(Out, true, TEXT("Presentation"), &Settings, FString::Printf(TEXT("%s is %d x %d; at most %d px per side."), Field, Width, Height, MaxSize));
            else if (Height <= 0 || FMath::Abs(double(Width) / Height - 16. / 9.) > .01)
                Add(Out, false, TEXT("Presentation"), &Settings, FString::Printf(TEXT("%s is %d x %d, not 16:9; screens crop it to fill."), Field, Width, Height));
        }
        return Path.ToString();
    }

    /** The mod's minimap definition for World; validates it against its texture rules, the capture and the current geometry. */
    FString CheckMinimap(const UWorld& World, const FString& ModRoot, const ARetakeMapSettings* Settings, TArray<FRetakeMapIssue>& Out)
    {
        const TArray<const ARetakeMinimapCapture*> Captures = RetakeMinimapBaker::FindCaptures(World);
        if (Captures.Num() > 1) Add(Out, true, TEXT("Minimap"), Captures[1], TEXT("place at most one Retake Minimap Capture."));
        const FString WorldPackage = World.GetPackage()->GetName();
        TArray<FAssetData> Assets;
        IAssetRegistry::GetChecked().GetAssetsByClass(URetakeMinimapDefinition::StaticClass()->GetClassPathName(), Assets);
        TArray<const URetakeMinimapDefinition*> Definitions;
        for (const FAssetData& Asset : Assets)
            if (Asset.PackageName.ToString().StartsWith(ModRoot))
                if (const URetakeMinimapDefinition* Definition = Cast<URetakeMinimapDefinition>(Asset.GetAsset());
                    Definition && Definition->World.ToSoftObjectPath().GetLongPackageName() == WorldPackage) Definitions.Add(Definition);
        if (Definitions.IsEmpty())
        {
            if (!Captures.IsEmpty()) Add(Out, true, TEXT("Minimap"), Captures[0], TEXT("the configured minimap is not ready: generate it or apply a custom image in the Mod Kit panel."));
            return FString();
        }
        if (Definitions.Num() > 1)
        {
            Add(Out, true, TEXT("Minimap"), nullptr, FString::Printf(TEXT("%d minimap definitions in the mod name this map; keep one."), Definitions.Num()));
            return FString();
        }
        const URetakeMinimapDefinition& Definition = *Definitions[0];
        const FString Name = Definition.GetName();
        if (Definition.Floorplan) FTextureCompilingManager::Get().FinishCompilation({ Definition.Floorplan.Get() });
        FString Error;
        if (!Definition.ValidateDefinition(Error)) Add(Out, true, TEXT("Minimap"), nullptr, FString::Printf(TEXT("%s: %s"), *Name, *Error));
        else if (!Definition.Floorplan->GetPackage()->GetName().StartsWith(ModRoot) || !HasUITextureSettings(*Definition.Floorplan))
            Add(Out, true, TEXT("Minimap"), nullptr, FString::Printf(TEXT("%s: the floorplan must be a texture in the mod with %s; bake again."), *Name, UITextureSettings));
        if (Captures.IsEmpty())
            Add(Out, true, TEXT("Minimap"), nullptr, FString::Printf(TEXT("%s exists but the map has no Retake Minimap Capture to bake it from; place one and bake."), *Name));
        else if ((RetakeMinimapBaker::IsCustom(Definition) && Definition.Floorplan
                ? RetakeMinimapBaker::CustomFingerprint(World,*Captures[0],*Definition.Floorplan)
                : RetakeMinimapBaker::ComputeFingerprint(World,*Captures[0])) != Definition.GeometryFingerprint
            || (Settings && Definition.DisplayName.ToString() != Settings->DisplayName))
            Add(Out, true, TEXT("Minimap"), Captures[0], RetakeMinimapBaker::IsCustom(Definition)
                ? TEXT("custom minimap image, layout or alignment changed: review it and Apply custom image again.")
                : FString::Printf(TEXT("%s is older than the map's geometry, capture or display name: generate the minimap again."), *Name));
        return Definition.GetPathName();
    }
}

void RetakeMapValidator::ValidateEnvironment(const UWorld& World, TArray<FRetakeMapIssue>& Out)
{
    if (!World.PersistentLevel) return;
    // Surfaces matter wherever players stand, see or shoot. "Weapon" is a profiled trace channel (DefaultEngine.ini CollisionProfile).
    ECollisionChannel Weapon = ECC_MAX;
    for (int32 Channel = 0; Channel < ECC_MAX; ++Channel)
        if (UCollisionProfile::Get()->ReturnChannelNameFromContainerIndex(Channel) == TEXT("Weapon")) Weapon = ECollisionChannel(Channel);
    TSet<const URetakeAmbienceProfile*> Profiles;
    for (const AActor* Actor : World.PersistentLevel->Actors)
    {
        if (!Actor || Actor->IsEditorOnly() || FActorEditorUtils::IsABuilderBrush(Actor)) continue;
        // The game plays every AmbientSound through its ambience class: it must loop, fall off and free its voice when inaudible.
        if (const AAmbientSound* Emitter = Cast<AAmbientSound>(Actor))
        {
            const UAudioComponent* Audio = Emitter->GetAudioComponent();
            const USoundBase* Sound = Audio ? Audio->Sound.Get() : nullptr;
            if (!Sound) Add(Out, true, TEXT("Audio"), Actor, TEXT("the Ambient Sound has no sound."));
            else
            {
                CheckVirtualization(Sound, Actor, TEXT("the Ambient Sound"), Out);
                if (!Sound->IsLooping()) Add(Out, false, TEXT("Audio"), Actor, FString::Printf(TEXT("%s does not loop, so the Ambient Sound plays once at load."), *Sound->GetName()));
                if (!Audio->bAllowSpatialization || !Audio->GetAttenuationSettingsToApply())
                    Add(Out, false, TEXT("Audio"), Actor, TEXT("the Ambient Sound has no attenuation, so it is heard at full volume everywhere."));
            }
        }
        for (const URetakeAmbienceZoneComponent* Zone : TInlineComponentArray<URetakeAmbienceZoneComponent*>(Actor))
        {
            FDataValidationContext Context;
            Zone->IsDataValid(Context);
            for (const FDataValidationContext::FIssue& Issue : Context.GetIssues())
                if (Issue.Severity == EMessageSeverity::Error)
                    Add(Out, true, TEXT("Audio"), Actor, Issue.TokenizedMessage ? Issue.TokenizedMessage->ToText().ToString() : Issue.Message.ToString());
            if (Zone->Profile && !Profiles.Contains(Zone->Profile))
            {
                Profiles.Add(Zone->Profile);
                CheckProfile(*Zone->Profile, Actor, Out);
            }
        }
        // One issue per actor and problem: a mesh repeats its material on every component.
        TSet<FString> Invalid;
        bool bDefault = false;
        for (UPrimitiveComponent* Primitive : TInlineComponentArray<UPrimitiveComponent*>(Actor))
        {
            if (Primitive->IsEditorOnly() || !Primitive->IsQueryCollisionEnabled()) continue;
            if (Primitive->GetCollisionResponseToChannel(ECC_Visibility) != ECR_Block && Primitive->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block
                && (Weapon == ECC_MAX || Primitive->GetCollisionResponseToChannel(Weapon) != ECR_Block)) continue;
            // Empty SDK geometry slots cannot produce surface hits. Invisible meshes with real collision still count.
            if (const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Primitive); Mesh && !Mesh->GetStaticMesh()) continue;
            if (const UInstancedStaticMeshComponent* Instances = Cast<UInstancedStaticMeshComponent>(Primitive); Instances && Instances->GetInstanceCount() == 0) continue;
            const FBodyInstance* Body = Primitive->GetBodyInstance();
            if (!Body) continue;
            const UBodySetup* Setup = Primitive->GetBodySetup();
            const ECollisionTraceFlag TraceFlag = Setup ? Setup->GetCollisionTraceFlag().GetValue() : CTF_UseSimpleAndComplex;
            // Resolve the material paths the physics shapes actually use. A simple-as-complex clip's render material
            // never participates in a hit; conversely, complex-as-simple geometry uses its triangle materials for both.
            // GetCollisionTraceFlag resolves project defaults. Passing the component avoids depending on live physics state.
            TArray<UPhysicalMaterial*> Materials;
            if (TraceFlag != CTF_UseSimpleAsComplex) FBodyInstance::GetComplexPhysicalMaterials(Body, Primitive, Materials);
            if (TraceFlag != CTF_UseComplexAsSimple)
                Materials.Add(FBodyInstance::GetSimplePhysicalMaterial(Body, Primitive, Primitive->GetBodySetup()));
            for (const UPhysicalMaterial* Material : Materials)
            {
                bool bError = false;
                const FString Problem = SurfaceProblem(Material, bError);
                if (bError) Invalid.Add(FString::Printf(TEXT("%s (%s)"), *GetNameSafe(Material), *Problem));
                else bDefault |= !Problem.IsEmpty();
            }
        }
        for (const FString& Problem : Invalid)
            Add(Out, true, TEXT("Surface"), Actor, FString::Printf(TEXT("collision uses %s; world geometry needs a named surface other than Character."), *Problem));
        if (bDefault)
            Add(Out, false, TEXT("Surface"), Actor, TEXT("active collision uses SurfaceType_Default; assign a mod physical material with a named surface. Surface effects use fallbacks, and bullet hits on this unclassified surface cannot penetrate."));
        // Damage comes only through the game's own rules, so the engine's pain volume would silently do nothing.
        if (Actor->IsA<APainCausingVolume>())
            Add(Out, true, TEXT("Volume"), Actor, TEXT("Pain Causing Volumes do nothing in Retake; remove it (Kill Z or a Kill Z Volume ends a fall)."));
    }
    // A start or objective at or below Kill Z kills or rejects at once.
    const AWorldSettings* Settings = World.GetWorldSettings(false, false);
    // A player who leaves the map dies at Kill Z; left at the engine default (about 10 km down) they fall for minutes first.
    if (Settings && Settings->KillZ <= -UE_OLD_HALF_WORLD_MAX1)
        Add(Out, false, TEXT("Volume"), nullptr, TEXT("World Settings Kill Z is the engine default (about 10 km down), so a player who leaves the map falls for minutes before dying. Set it about 10 m below the lowest floor."));
    const TArray<const AKillZVolume*> KillVolumes = Placed<AKillZVolume>(World);
    const auto CheckPoint = [&](const AActor* Actor, const FVector& Point, const TCHAR* What)
    {
        if (Settings && Point.Z <= Settings->KillZ)
            Add(Out, true, TEXT("Volume"), Actor, FString::Printf(TEXT("%s (%.0f cm) is at or below World Settings Kill Z (%.0f cm)."), What, Point.Z, Settings->KillZ));
        for (const AKillZVolume* Volume : KillVolumes)
            if (Volume->EncompassesPoint(Point))
                Add(Out, true, TEXT("Volume"), Actor, FString::Printf(TEXT("%s is inside Kill Z Volume %s."), What, *Volume->GetActorNameOrLabel()));
    };
    for (const ARetakeModePlayerStart* Start : Placed<ARetakeModePlayerStart>(World))
        CheckPoint(Start, Start->GetActorLocation() - FVector(0, 0, Start->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()), TEXT("the start's floor point"));
    for (const ARetakeModeObjectiveMarker* Marker : Placed<ARetakeModeObjectiveMarker>(World))
        CheckPoint(Marker, Marker->GetActorLocation(), TEXT("the objective"));
    // A minefield mines the edge of the playable space (MOHAA's canal on obj_team4): a start inside one kills at spawn, and an
    // objective zone that touches one turns the objective into a mine.
    for (const ARetakeMinefield* Minefield : Placed<ARetakeMinefield>(World))
    {
        // Element warnings are not collected from IsDataValid, so the mine layout is checked here.
        if (const FString Layout = Minefield->GetMineLayoutWarning(); !Layout.IsEmpty()) Add(Out, false, TEXT("Minefield"), Minefield, Layout);
        for (const ARetakeModePlayerStart* Start : Placed<ARetakeModePlayerStart>(World))
            if (Start->GetCapsuleComponent() && Minefield->OverlapsBox(Start->GetCapsuleComponent()->Bounds.GetBox()))
                Add(Out, true, TEXT("Minefield"), Start, FString::Printf(TEXT("the start is inside minefield %s, so a player steps on a mine as they spawn."),
                    *Minefield->GetActorNameOrLabel()));
        for (const ARetakeModeObjectiveMarker* Marker : Placed<ARetakeModeObjectiveMarker>(World))
            if (Marker->Zone && Minefield->OverlapsBox(Marker->Zone->Bounds.GetBox()))
                Add(Out, false, TEXT("Minefield"), Marker, FString::Printf(TEXT("the objective zone touches minefield %s, so playing the objective can set off a mine."),
                    *Minefield->GetActorNameOrLabel()));
    }
}

bool RetakeMapValidator::ValidatePresentation(const UWorld& World, const FString& ModRoot, FRetakeModMap& OutMap, TArray<FRetakeMapIssue>& Out)
{
    OutMap.Visibility = TEXT("Unbaked");
    OutMap.VisibilityBake.Reset();
    const int32 ErrorsBefore = Out.FilterByPredicate([](const FRetakeMapIssue& I) { return I.bError; }).Num();
    const TArray<const ARetakeMapSettings*> AllSettings = Placed<ARetakeMapSettings>(World);
    // More than one is a MapSettings error from ValidateWorld.
    const ARetakeMapSettings* Settings = AllSettings.Num() == 1 ? AllSettings[0] : nullptr;
    if (AllSettings.IsEmpty())
        Add(Out, true, TEXT("Presentation"), nullptr, TEXT("place a Retake Map Settings actor and set its Display Name: a mod map presents itself through it."));
    if (Settings)
    {
        const auto CheckLoadout = [&](const URetakeLoadoutPolicy* Policy, const FString& Mode)
        {
            if (!Policy)
            {
                if (!Mode.IsEmpty()) Add(Out, true, TEXT("Loadouts"), Settings, TEXT("mode loadout policy is empty; remove the entry to inherit the map default"));
                return;
            }
            FString Error;
            if (!Policy->GetOutermost()->GetName().StartsWith(ModRoot) || !Policy->ValidateAuthoringWeapons(Error))
            {
                Add(Out, true, TEXT("Loadouts"), Settings, Error.IsEmpty() ? TEXT("loadout policy must be owned by this mod") : Error);
                return;
            }
            if (Mode.IsEmpty()) OutMap.DefaultLoadoutPolicy = Policy->GetPathName();
            else if (const FString* Supported = OutMap.SupportedModes.FindByPredicate([&Mode](const FString& Value) { return Value.Equals(Mode, ESearchCase::IgnoreCase); }))
                OutMap.ModeLoadoutPolicies.Add(*Supported, Policy->GetPathName());
            else Add(Out, true, TEXT("Loadouts"), Settings, TEXT("loadout policy names a mode this map does not support: ") + Mode);
        };
        CheckLoadout(Settings->DefaultLoadoutPolicy, FString());
        for (const auto& Pair : Settings->ModeLoadoutPolicies) CheckLoadout(Pair.Value, Pair.Key.ToString());
        if (Settings->VisibilityBake)
        {
            const URetakeVisibilityData* Bake = Settings->VisibilityBake;
            OutMap.Visibility = TEXT("Baked");
            OutMap.VisibilityBake = Bake->GetPathName();
            FString Geometry, Source, Error;
            TArray<FRetakeOcclusionMesh> Meshes;
            if (!Bake->GetOutermost()->GetName().StartsWith(ModRoot))
                Add(Out, true, TEXT("Visibility"), Settings, TEXT("visibility preparation must belong to this mod"));
            else if (!RetakeVisibilityScene::Capture(const_cast<UWorld*>(&World), Meshes, Geometry, Error, nullptr, true)
                || !RetakeVisibilityScene::SourceFingerprint(const_cast<UWorld*>(&World), Source, Error)
                || !Bake->Validate(World.GetOutermost()->GetName(), Geometry, Error))
                Add(Out, true, TEXT("Visibility"), Settings, Error);
            else if (Bake->SourceFingerprint != Source)
                Add(Out, true, TEXT("Visibility"), Settings, TEXT("visibility source identity is stale; bake this map again"));
        }
        OutMap.DisplayName = Settings->DisplayName;
        OutMap.Description = Settings->Description;
        if (!FRetakeModManifest::IsValidDisplayName(Settings->DisplayName))
            Add(Out, true, TEXT("Presentation"), Settings, TEXT("Display Name must be 1-64 characters without quotes, backslashes, '?' or '='."));
        if (!FRetakeModManifest::IsValidDescription(Settings->Description))
            Add(Out, true, TEXT("Presentation"), Settings, TEXT("Description must be at most 280 characters on one line, without quotes or backslashes."));
        OutMap.LoadingArt = CheckTexture(*Settings, Settings->LoadingArt, ModRoot, 2048, TEXT("Loading Art"), Out);
        OutMap.Preview = CheckTexture(*Settings, Settings->Preview, ModRoot, 1024, TEXT("Preview"), Out);
        if (!Settings->Ambience.IsNull())
        {
            const FSoftObjectPath Path = Settings->Ambience.ToSoftObjectPath();
            OutMap.Ambience = Path.ToString();
            const URetakeAmbienceProfile* Profile = Path.GetLongPackageName().StartsWith(ModRoot) ? Settings->Ambience.LoadSynchronous() : nullptr;
            if (Profile) CheckProfile(*Profile, Settings, Out);
            else Add(Out, true, TEXT("Audio"), Settings, FString::Printf(TEXT("Ambience must be an ambience profile inside %s."), *ModRoot));
        }
    }
    OutMap.Minimap = CheckMinimap(World, ModRoot, Settings, Out);
    const TArray<const ARetakeAcousticMap*> AcousticMaps = Placed<ARetakeAcousticMap>(World);
    if (AcousticMaps.Num() != 1)
        Add(Out, true, TEXT("Acoustics"), nullptr, TEXT("a gameplay map needs exactly one Retake Acoustic Map actor and a current acoustic bake."));
    else
    {
        const ARetakeAcousticMap* AcousticMap = AcousticMaps[0];
        FString Error;
        if (!AcousticMap->ValidateBake(Error)) Add(Out, true, TEXT("Acoustics"), AcousticMap, Error);
        if (AcousticMap->Data)
        {
            OutMap.Acoustics = AcousticMap->Data->GetPathName();
            if (!AcousticMap->Data->GetOutermost()->GetName().StartsWith(ModRoot))
                Add(Out, true, TEXT("Acoustics"), AcousticMap, TEXT("the acoustic bake must live inside this mod's own package root."));
        }
    }
    // Mod physical materials stand in for the game's: they must carry a named world surface.
    TArray<FAssetData> Materials;
    IAssetRegistry::GetChecked().GetAssetsByClass(UPhysicalMaterial::StaticClass()->GetClassPathName(), Materials, true);
    for (const FAssetData& Asset : Materials)
    {
        if (!Asset.PackageName.ToString().StartsWith(ModRoot)) continue;
        bool bError = false;
        const FString Problem = SurfaceProblem(Cast<UPhysicalMaterial>(Asset.GetAsset()), bError);
        if (!Problem.IsEmpty())
            Add(Out, true, TEXT("Surface"), nullptr, FString::Printf(TEXT("physical material %s uses %s; set Surface Type to a named world surface such as Concrete or Metal."),
                *Asset.AssetName.ToString(), *Problem));
    }
    return Out.FilterByPredicate([](const FRetakeMapIssue& I) { return I.bError; }).Num() == ErrorsBefore;
}
