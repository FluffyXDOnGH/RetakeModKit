// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "RetakeAcousticMapData.h"
#include "UObject/ObjectKey.h"
#include "RetakeAcousticWorldSubsystem.generated.h"

class ARetakeAcousticConnection;
class AActor;

/** Game-thread topology and propagation. Shared by authoritative delivery, voice and replay presentation. */
UCLASS()
class RETAKEMAPSDK_API URetakeAcousticWorldSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    bool Prepare(FString& Error);
    bool IsReady() const { return bReady; }
    uint32 GetTopologyRevision() const { return TopologyRevision; }
    const URetakeAcousticMapData* GetMapData() const { return Data; }
    FRetakeAcousticPathResult Resolve(const FVector& Source, const FVector& Listener,
        const FRetakeAudioPropagationProfile& Profile, const AActor* SourceActor = nullptr, const AActor* ListenerActor = nullptr);
    DECLARE_MULTICAST_DELEGATE(FTopologyChanged);
    FTopologyChanged OnTopologyChanged;
    /**
     * Process totals since start, over every world: Resolve calls on a ready subsystem, those answered from the path cache and
     * the structural line traces they ran. Game thread; consumers (the game's core performance counters) take differences.
     */
    struct FQueryTotals { uint64 Resolves = 0, CacheHits = 0, Traces = 0; };
    static FQueryTotals GetQueryTotals();
protected:
    virtual bool DoesSupportWorldType(EWorldType::Type Type) const override;
private:
    void RefreshTopology();
    UPROPERTY(Transient) TObjectPtr<URetakeAcousticMapData> Data;
    TArray<TWeakObjectPtr<ARetakeAcousticConnection>> ConnectionActors;
    TArray<uint8> OpenStates;
    FDelegateHandle AddedHandle, RemovedHandle, StateHandle;
    uint32 TopologyRevision = 0;
    bool bReady = false;
    struct FCacheKey
    {
        FIntVector Source, Listener;
        int32 SourceRegion = INDEX_NONE, ListenerRegion = INDEX_NONE;
        float Range = 0, Radius = 0, Exponent = 0;
        FObjectKey SourceActor, ListenerActor;
        bool operator==(const FCacheKey&) const = default;
        friend uint32 GetTypeHash(const FCacheKey& Key)
        {
            uint32 Hash = HashCombineFast(GetTypeHash(Key.Source), GetTypeHash(Key.Listener));
            Hash = HashCombineFast(Hash, HashCombineFast(GetTypeHash(Key.SourceRegion), GetTypeHash(Key.ListenerRegion)));
            Hash = HashCombineFast(Hash, HashCombineFast(GetTypeHash(Key.Range), HashCombineFast(GetTypeHash(Key.Radius), GetTypeHash(Key.Exponent))));
            return HashCombineFast(Hash, HashCombineFast(GetTypeHash(Key.SourceActor), GetTypeHash(Key.ListenerActor)));
        }
    };
    struct FCachedPath { double Expires = 0; FRetakeAcousticPathResult Result; };
    TMap<FCacheKey, FCachedPath> Cache;
};
