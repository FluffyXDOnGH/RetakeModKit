// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeAcousticWorldSubsystem.h"
#include "RetakeAcousticMap.h"
#include "RetakeDoor.h"
#include "RetakeMapElementSubsystem.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
    // Resolve runs on the game thread only (its cache and the scene query), so plain counters are enough.
    URetakeAcousticWorldSubsystem::FQueryTotals GQueryTotals;
}

URetakeAcousticWorldSubsystem::FQueryTotals URetakeAcousticWorldSubsystem::GetQueryTotals() { return GQueryTotals; }

bool URetakeAcousticWorldSubsystem::DoesSupportWorldType(EWorldType::Type Type) const
{ return Type == EWorldType::Game || Type == EWorldType::PIE; }

void URetakeAcousticWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    auto* Registry = Collection.InitializeDependency<URetakeMapElementSubsystem>();
    AddedHandle = Registry->OnElementAdded.AddWeakLambda(this, [this](AActor& Actor)
        { if (Actor.IsA<ARetakeAcousticConnection>() || Actor.IsA<ARetakeDoor>()) RefreshTopology(); });
    RemovedHandle = Registry->OnElementRemoved.AddWeakLambda(this, [this](AActor& Actor)
        { if (Actor.IsA<ARetakeAcousticConnection>() || Actor.IsA<ARetakeDoor>()) { Cache.Reset(); ++TopologyRevision; OnTopologyChanged.Broadcast(); } });
    StateHandle = Registry->OnElementStateChanged.AddWeakLambda(this, [this](AActor&, FName Channel)
        { if (Channel == ARetakeAcousticConnection::StateChannel || Channel == ARetakeDoor::StateChannel) RefreshTopology(); });
}

void URetakeAcousticWorldSubsystem::Deinitialize()
{
    if (auto* Registry = GetWorld()->GetSubsystem<URetakeMapElementSubsystem>())
    {
        Registry->OnElementAdded.Remove(AddedHandle); Registry->OnElementRemoved.Remove(RemovedHandle);
        Registry->OnElementStateChanged.Remove(StateHandle);
    }
    bReady = false; Cache.Reset(); ConnectionActors.Reset(); OpenStates.Reset(); Data = nullptr;
    OnTopologyChanged.Clear();
    Super::Deinitialize();
}

bool URetakeAcousticWorldSubsystem::Prepare(FString& Error)
{
    if (bReady) return true;
    ARetakeAcousticMap* Authoring = nullptr;
    for (TActorIterator<ARetakeAcousticMap> It(GetWorld()); It; ++It)
    {
        if (Authoring) { Error = TEXT("Gameplay map contains multiple Retake Acoustic Map actors."); return false; }
        Authoring = *It;
    }
    if (!Authoring) { Error = TEXT("Gameplay map requires a Retake Acoustic Map and a current bake."); return false; }
    if (!Authoring->ValidateBake(Error)) return false;
    Data = Authoring->Data;
    TMap<FName, ARetakeAcousticConnection*> ById;
    for (TActorIterator<ARetakeAcousticConnection> It(GetWorld()); It; ++It) ById.Add(It->ConnectionId, *It);
    ConnectionActors.Reset(); OpenStates.Reset();
    for (const auto& Edge : Data->Connections)
    {
        auto* Actor = ById.FindRef(Edge.Id);
        if (!Actor) { Error = TEXT("Baked acoustic connection is absent from the world."); Data = nullptr; return false; }
        ConnectionActors.Add(Actor); OpenStates.Add(Actor->HasActorBegunPlay() ? Actor->IsOpen() : Edge.bInitiallyOpen);
    }
    bReady = true; ++TopologyRevision; Cache.Reset();
    RefreshTopology();
    return true;
}

void URetakeAcousticWorldSubsystem::RefreshTopology()
{
    if (!bReady) return;
    bool Changed = false;
    for (int32 Index = 0; Index < ConnectionActors.Num(); ++Index)
    {
        const auto* Actor = ConnectionActors[Index].Get();
        const bool Open = Actor && Actor->HasActorBegunPlay() ? Actor->IsOpen() : Data->Connections[Index].bInitiallyOpen;
        if (OpenStates[Index] != uint8(Open)) { OpenStates[Index] = Open; Changed = true; }
    }
    if (Changed) { ++TopologyRevision; Cache.Reset(); OnTopologyChanged.Broadcast(); }
}

FRetakeAcousticPathResult URetakeAcousticWorldSubsystem::Resolve(const FVector& Source, const FVector& Listener,
    const FRetakeAudioPropagationProfile& Profile, const AActor* SourceActor, const AActor* ListenerActor)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(RetakeAudio_PropagationGraph);
    FRetakeAcousticPathResult Result;
    if (!bReady || Source.ContainsNaN() || Listener.ContainsNaN()) return Result;
    ++GQueryTotals.Resolves;
    const float Distance = FVector::Distance(Source, Listener);
    if (Profile.GainAtDistance(Distance) <= .001f) return Result;
    const auto Quantize = [](const FVector& V) { return FIntVector(FMath::FloorToInt(V.X / 50.), FMath::FloorToInt(V.Y / 50.), FMath::FloorToInt(V.Z / 50.)); };
    FCacheKey Key{Quantize(Source), Quantize(Listener), Data->FindRegion(Source), Data->FindRegion(Listener),
        Profile.RangeCm, Profile.FullGainRadiusCm, Profile.FalloffExponent, FObjectKey(SourceActor), FObjectKey(ListenerActor)};
    if (Key.SourceRegion == INDEX_NONE || Key.ListenerRegion == INDEX_NONE) return Result;
    const double Now = GetWorld()->GetTimeSeconds();
    if (const auto* Entry = Cache.Find(Key); Entry && Now < Entry->Expires && Now >= Entry->Expires - .1)
    { ++GQueryTotals.CacheHits; return Entry->Result; }
    FCollisionQueryParams Query(SCENE_QUERY_STAT(RetakeAcousticStructure), false, SourceActor);
    if (ListenerActor) Query.AddIgnoredActor(ListenerActor);
    ++GQueryTotals.Traces;
    // Channel 7 is RetakeAcoustics, default Ignore; only authored structural blockers opt in.
    if (!GetWorld()->LineTraceTestByChannel(Source, Listener, ECC_GameTraceChannel7, Query))
    {
        Result.Gain = Profile.GainAtDistance(Distance); Result.DistanceCm = Distance;
        // Door leaves intentionally stay out of structural collision/bake hashes. Their registered
        // state still applies to a direct ray through the leaf, including a handed-over destructible.
        // Test in mesh-local space so diagonal doors are handled without treating their world AABB as a wall.
        TArray<const ARetakeDoor*, TInlineAllocator<16>> CrossedDoors;
        for (int32 Index=0; Index<ConnectionActors.Num(); ++Index)
        {
            const auto* Connection=ConnectionActors[Index].Get();
            const auto* Door=Connection ? Connection->Door.Get() : nullptr;
            const auto* Mesh=Door ? Door->DoorMesh.Get() : nullptr;
            if (OpenStates[Index] || !Mesh || !Mesh->GetStaticMesh() || Door==SourceActor || Door==ListenerActor
                || CrossedDoors.Contains(Door)) continue;
            const FTransform Transform=Mesh->GetComponentTransform();
            const FVector Start=Transform.InverseTransformPosition(Source),End=Transform.InverseTransformPosition(Listener);
            if (!FMath::LineBoxIntersection(Mesh->GetStaticMesh()->GetBoundingBox(),Start,End,End-Start)) continue;
            CrossedDoors.Add(Door);
            const auto& Material=Data->Connections[Index].ClosedMaterial;
            Result.Gain*=Material.Transmission;
            Result.LowPassHz=FMath::Min(Result.LowPassHz,Material.LowPassHz);
        }
        if (!CrossedDoors.IsEmpty())
        {
            // A stronger route through a neighbouring opening may beat transmission through the closed leaf.
            const auto Indirect=RetakeAcousticGraph::Resolve(*Data,OpenStates,Source,Listener,Profile);
            if (Indirect.Gain>Result.Gain) Result=Indirect;
        }
    }
    else Result = RetakeAcousticGraph::Resolve(*Data, OpenStates, Source, Listener, Profile);
    // Bounded cache storage, including denied paths. No scene search or allocation runs on an audio thread.
    if (Cache.Num() >= 4096) Cache.Reset();
    Cache.Add(Key, FCachedPath{Now + .1, Result});
    return Result;
}
