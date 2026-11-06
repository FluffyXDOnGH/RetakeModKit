// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/NetSerialization.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "RetakeVisibilityGeometry.h"
#include "RetakePieceMask.h"
#include "RetakeDestructibleActor.generated.h"

class URetakeDestructionDefinition;
class ARetakeDestructiblePlacement;
class URetakeDestructibleTraversalComponent;
struct FRetakeDestructionHit;

RETAKEDESTRUCTION_API DECLARE_LOG_CATEGORY_EXTERN(LogRetakeDestruction, Log, All);
/** `Retake.Destruction.Debug 1`: server logs each destructible hit, break and release, and why a wall does not conceal. */
RETAKEDESTRUCTION_API bool RetakeDestructionDebugEnabled();
class UStaticMeshComponent;
class UAudioComponent;

/** Exposes targeted native field dispatch without field actors or per-hit UObjects. */
UCLASS()
class RETAKEDESTRUCTION_API URetakeGeometryCollectionComponent : public UGeometryCollectionComponent
{
    GENERATED_BODY()
public:
    URetakeGeometryCollectionComponent();
    void InitializeCollection(URetakeDestructionDefinition* Definition);
    void ApplyLocalizedDamage(const FVector& Point,float Radius,float Strain,const FVector& Direction,float DirectionalSpeed,float RadialSpeed,float AngularSpeed,bool bPointImpact,const FVector2D& NoiseRange,const FVector& SurfaceNormal,bool bAuthoredResponse);
    /** Client: release these leaf pieces (and any unbroken ancestors, as native late-join replication does) and disable them. */
    void RemovePieces(const TArray<int32>& TransformIndices);
protected:
    virtual void OnCreatePhysicsState() override;
private:
    void DispatchUniqueField(const FFieldSystemCommand& Command);
};

/**
 * A placement's runtime collection, spawned by URetakeDestructionSubsystem. A round reset replaces a changed actor with a new one
 * (new replication identity and generation) and keeps an untouched one (CanKeepForRound), which joins the new generation.
 */
UCLASS(NotBlueprintable, NotPlaceable)
class RETAKEDESTRUCTION_API ARetakeDestructibleActor : public AActor
{
    GENERATED_BODY()
public:
    ARetakeDestructibleActor();
    void Initialize(URetakeDestructionDefinition* InDefinition,uint32 InGeneration);
    /** Server: one committed hit. Damage is already capped by the attack; DamageFraction scales authored strain. */
    void ApplyDamageAt(const FRetakeDestructionHit& Hit,float Damage,float DamageFraction);
    /**
     * Whether a grenade striking Hit at Velocity breaks through instead of bouncing. Deterministic over replicated state
     * (definition and released pieces), so the server, the owner's prediction and observer presentation agree.
     */
    static bool ResolveGrenadePass(const FHitResult& Hit,const FVector& Velocity,float ContactDamage,float& OutSpeedScale);
    bool IsReleased(int32 Index) const;
    /** Server: nearest loose shard crossed by the segment, using each piece's current bounds. No query collision is involved. */
    bool TraceLooseDebris(const FVector& Start,const FVector& End,int32& OutItem,FVector& OutPoint,FVector& OutNormal,double& OutDistance);
    bool IsCleared() const { return bCleared; }
    bool IsCollectionShattered() const { return bCollectionShattered; }
    bool HasLooseDebris() const { return !bCleared && !ReleasedPieces.IsEmpty(); }
    bool Occludes(const FBox& Eye,const FBox& Subject,FString* OutReason=nullptr) const;
    /** Diagnostics: physics and release state of the pieces (counts only). */
    FString DescribeState() const;
    /**
     * Server: world-space opaque faces of the pieces still standing, within Bounds. They mark solid, pawn-blocking pieces,
     * so the movement proof treats them like wall faces. False (nothing added) whenever Occludes would not trust them.
     */
    bool GatherIntactFaces(const FBox& Bounds,TArray<FRetakeSightPolygon>& Out,int32 Limit) const;
    UGeometryCollectionComponent* GetCollectionComponent() const { return Geometry; }
    UStaticMeshComponent* GetFrameComponent() const { return Frame; }
    ARetakeDestructiblePlacement* GetPlacement() const { return Placement; }
    /** Intact, pawn-blocking collection still at the placement's authored pose. Never admits loose fragments or door leaves. */
    bool CanTraverseIntact() const;
    URetakeDestructionDefinition* GetDefinition() const { return Definition; }
    /** Server, before FinishSpawning: the map placement that owns this actor (receives the cosmetic Blueprint events). */
    void SetPlacement(ARetakeDestructiblePlacement* InPlacement) { Placement=InPlacement; }
    uint32 GetGeneration() const { return Generation; }
    /**
     * Server, at a round reset: still what a new actor would be on every machine, so the reset keeps it. No hit was applied,
     * nothing broke, released, cleared or yielded, an anchor holds the intact root (no client's copy can have been pushed) and
     * the collection stands at the placement's pose.
     */
    bool CanKeepForRound() const;
    /** Server: a kept actor joins the new round's generation, so hits gathered before the reset still miss it. */
    void ContinueInGeneration(uint32 InGeneration);
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    /**
     * Client (A26-01 F6): a destructible that is not a door leaf keeps its placement's exact transform instead of the replicated
     * pose. The server spawns it there and never moves its root in any state (Chaos moves only the collection's particles), while
     * replicated movement carries that pose rounded to whole centimetres and 8-bit rotation. Owner and server Mover sweeps must
     * hit the same crate. Door leaves do not replicate movement; they hang from their door (AttachToDoor).
     */
    virtual void PostNetReceiveLocationAndRotation() override;
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    friend class URetakeGeometryCollectionComponent;
    UPROPERTY(VisibleAnywhere, Category="Retake|Destruction|Components") TObjectPtr<URetakeGeometryCollectionComponent> Geometry;
    UPROPERTY(VisibleAnywhere, Category="Retake|Destruction|Components") TObjectPtr<UStaticMeshComponent> Frame;
    UPROPERTY(VisibleAnywhere, Category="Retake|Destruction|Components") TObjectPtr<URetakeDestructibleTraversalComponent> Traversal;
    UPROPERTY(Transient) TArray<TObjectPtr<UAudioComponent>> CollisionVoices;
    TArray<FTimerHandle> CollisionVoiceTimers;
    UPROPERTY(ReplicatedUsing=OnRep_Definition) TObjectPtr<URetakeDestructionDefinition> Definition;
    /** Level placement (stably named, so it resolves on clients). */
    UPROPERTY(ReplicatedUsing=OnRep_Placement) TObjectPtr<ARetakeDestructiblePlacement> Placement;
    UPROPERTY(Replicated) uint32 Generation=0;
    /** Compact persistent gameplay-collision mask. It also prevents client physics from restoring cover. */
    UPROPERTY(ReplicatedUsing=OnRep_Released) FRetakePieceMask ReleasedPieces;
    /**
     * Server truth for pieces that have left the wall: released and moved, turned or shrunk off their rest pose. Visibility
     * reads it. A client receiving the initial state removes listed pieces still standing in its copy: native Geometry
     * Collection replication sends only broken flags, so a joining client otherwise rebuilds pieces from inside a broken
     * cluster at rest. Later updates are left to native replication, which already simulates the breaks.
     */
    UPROPERTY(Replicated) FRetakePieceMask GonePieces;
    UPROPERTY(ReplicatedUsing=OnRep_FractureState) bool bFractured=false;
    UPROPERTY(ReplicatedUsing=OnRep_FractureState) bool bCleared=false;
    /** Server-decided reference switch: the intact root no longer blocks pawns after its first real contact. */
    UPROPERTY(ReplicatedUsing=OnRep_RootYield) bool bRootYieldsToPawns=false;
    /** A door leaf (the placement's Door) broke past its collapse share: every machine drops the leaf's edge anchors. */
    UPROPERTY(ReplicatedUsing=OnRep_DoorCollapsed) bool bDoorCollapsed=false;
    /** Persistent anchor release for an admitted whole-collection shatter. */
    UPROPERTY(ReplicatedUsing=OnRep_CollectionShattered) bool bCollectionShattered=false;
    /** Attached to the placement's door on this machine (locally on every machine, never through movement replication). */
    bool bDoorLeaf=false;
    bool bRootYieldApplied=false;
    FRetakePieceMask AppliedCollisionMask;
    bool bJoinStateApplied=false;
    FTimerHandle SettleTimer;
    FTimerHandle PhysicsStateRefreshTimer;
    FTimerHandle CollisionAudioWindowTimer;
    FTimerHandle DebrisSleepTimer;
    double CollisionAudioEndTime=0;
    double LastDebrisMotionTime=0,DebrisTrackingEndTime=0;
    double LastDamageTime=-DBL_MAX;
    double NextCollisionSound=0,NextBreakSound=0,NextBreakEffect=0;
    double LastImpactPresentation=-DBL_MAX;
    uint64 LastReleaseFrame=MAX_uint64;
    /** Each piece's component-space transform when configured. A piece counts as released only once it has left it. */
    TArray<FTransform3f> RestPieceTransforms;
    void CaptureRestPose();
    bool HasLeftRest(int32 Index) const;
    /** Visibility: released and off its rest pose. A broken piece still standing in the wall keeps concealing. */
    bool IsGone(int32 Index) const { return GonePieces.Contains(Index); }
    /** Server: release a broken particle and every piece under it (collision off at once, as the reference's IgnoreCharChaos). */
    void ReleaseBroken(int32 Index);
    bool bConfigured=false;
    /** Server: a committed hit reached the collection. Its fields can change piece states and velocities without breaking anything. */
    bool bHitSinceSpawn=false;
    /** Chaos holds an intact cluster kinematic while any of its pieces is anchored. */
    bool HasAnchoredRoot() const;
    bool bMaterialsBroken=false;
    bool bTrackingDebris=false;
    bool bTraversalCollisionApplied=false;
    void Configure();
    /** Reapply persistent state once a newly created collection proxy is ready, outside its creation callback. */
    void RefreshPhysicsState();
    void RefreshTraversal();
    void RefreshReleased();
    void ApplyReleasedCollision();
    void RemoveGonePiecesOnJoin();
    void ApplyFractureState();
    void FinishClearedState();
    void ApplyRootYield();
    /** Door leaf: hang from the door's pivot at the leaf's closed-pose offset, anchor the hinge strip, take over the door's leaf. */
    void AttachToDoor();
    /** Server: once the placement's collapse share of the leaf has broken off, the door's whole leaf falls away. */
    void TryCollapseDoorLeaf();
    void OpenCollisionAudioWindow();
    void StopCollisionAudio();
    void FadeCollisionAudio(int32 VoiceIndex);
    void BeginDebrisSettling();
    void SettleQuietDebris();
    void StopDebrisTracking();
    UFUNCTION() void OnRep_Definition();
    UFUNCTION() void OnRep_Placement();
    UFUNCTION() void OnRep_Released();
    UFUNCTION() void OnRep_FractureState();
    UFUNCTION() void OnRep_RootYield();
    UFUNCTION() void OnRep_DoorCollapsed();
    UFUNCTION() void OnRep_CollectionShattered();
    void ApplyCollectionShatterState();
    UFUNCTION() void HandleFullyDecayed();
    UFUNCTION() void HandleRootMoved();
    UFUNCTION() void HandleBreak(const FChaosBreakEvent& Event);
    UFUNCTION() void HandleCollision(const FChaosPhysicsCollisionInfo& Event);
    /** Transient presentation only; durable structure lives in native GC replication and the mask. */
    UFUNCTION(NetMulticast, Unreliable) void MulticastImpact(FVector_NetQuantize100 Point,FVector_NetQuantizeNormal Normal,uint8 PresentationFlags);
};
