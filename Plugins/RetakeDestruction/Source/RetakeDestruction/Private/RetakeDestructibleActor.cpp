// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeDestructibleActor.h"
#include "RetakeDestructibleTraversalComponent.h"
#include "RetakeDestructionDefinition.h"
#include "RetakeDestructionSubsystem.h"
#include "RetakeDestructiblePlacement.h"
#include "RetakeDoor.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/Facades/CollectionAnchoringFacade.h"
#include "GeometryCollectionProxyData.h"
#include "Components/StaticMeshComponent.h"
#include "Components/AudioComponent.h"
#include "Field/FieldSystem.h"
#include "Field/FieldSystemNodes.h"
#include "PhysicsProxy/GeometryCollectionPhysicsProxy.h"
#include "PhysicsProxy/FieldSystemProxyHelper.h"
#include "RetakeVisibilityCoverage.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"
#include "ProfilingDebugging/CountersTrace.h"

TRACE_DECLARE_INT_COUNTER(RetakeDestructionFragments,TEXT("Retake/Destruction/Released fragments"));
DEFINE_LOG_CATEGORY(LogRetakeDestruction);
static TAutoConsoleVariable<int32> CVarRetakeDestructionDebug(TEXT("Retake.Destruction.Debug"),0,
    TEXT("1: server logs destructible hits, breaks, releases and occlusion rejections (LogRetakeDestruction)."));
bool RetakeDestructionDebugEnabled() { return CVarRetakeDestructionDebug.GetValueOnAnyThread()!=0; }
namespace RetakeDestructionStrain
{
    /** Authored response: every shot applies this fixed strain, scaled only by penetration attenuation (reference parity). */
    constexpr float AuthoredShot=2000000.f;
    /** Breaks every cluster in range whatever the thresholds: authored explosions and grenades breaking through. */
    constexpr float Shatter=1.e12f;
}
namespace
{
constexpr float CollisionAudioFadeSeconds=.15f;
double SoundInterval(const FVector2D& Hz)
{
    // Authored graphs also contain reversed endpoints. FRandRange supports them;
    // Blueprint's safe divide returns zero when the sampled frequency is zero.
    const double Frequency=FMath::FRandRange(Hz.X,Hz.Y);
    return Frequency>0 ? 1./Frequency:0;
}

/**
 * A26-01 F6: stand a destructible that is not a door leaf exactly where the server spawned it, at its placement's transform. The
 * spawn header (0.1 cm) and replicated movement (whole centimetres, 8-bit rotation) round that pose, and a lean or capsule sweep
 * against a crate 0.5 cm away from the server's reconciled every frame. Only the kinematic, anchored root follows a later move;
 * released pieces keep simulating where they are.
 */
void HoldPlacementTransform(ARetakeDestructibleActor& Actor)
{
    const ARetakeDestructiblePlacement* Placement=Actor.GetPlacement();
    if (!IsValid(Placement) || Placement->Door) return;
    const FTransform& Exact=Placement->GetActorTransform();
    if (!Actor.GetActorTransform().Equals(Exact,0.)) Actor.SetActorTransform(Exact,false,nullptr,ETeleportType::TeleportPhysics);
}

/** The engine's component-local minimal enumeration repeats an internal parent for
 * each disabled child. Delta-velocity fields then push that parent repeatedly.
 * Match the solver-wide active-parent/direct-child set, restricted to this proxy. */
struct FRetakeUniqueFieldParticles
{
    FGeometryCollectionPhysicsProxy* Proxy;
    void GetRelevantParticleHandles(TArray<Chaos::FGeometryParticleHandle*>& Handles,const Chaos::FPBDRigidsSolver* Solver,EFieldResolutionType Resolution)
    {
        check(Resolution==EFieldResolutionType::Field_Resolution_Minimal);
        Handles.Reset();
        TSet<Chaos::FGeometryParticleHandle*> Seen;
        Seen.Reserve(Proxy->GetNumTransforms());Handles.Reserve(Proxy->GetNumTransforms());
        const auto& Children=Solver->GetEvolution()->GetRigidClustering().GetChildrenMap();
        auto Add=[&](Chaos::FGeometryParticleHandle* Handle)
        {
            bool AlreadySeen=false;Seen.Add(Handle,&AlreadySeen);
            if (!AlreadySeen) Handles.Add(Handle);
            return !AlreadySeen;
        };
        for (int32 I=0;I<Proxy->GetNumTransforms();++I)
        {
            auto* Handle=Proxy->GetParticle_Internal(I);
            if (!Handle) continue;
            if (Handle->Disabled())
            {
                auto* Parent=Handle->Parent();
                if (!Parent || Parent->Disabled() || !Parent->InternalCluster() || Parent->PhysicsProxy()!=Proxy) continue;
                Handle=Parent;
            }
            if (!Add(Handle)) continue;
            if (const auto* DirectChildren=Children.Find(Handle))
                for (auto* Child:*DirectChildren) Add(Child);
        }
    }
    void GetFilteredParticleHandles(TArray<Chaos::FGeometryParticleHandle*>& Handles,const Chaos::FPBDRigidsSolver* Solver,EFieldFilterType Filter,EFieldObjectType Object)
    { Proxy->GetFilteredParticleHandles(Handles,Solver,Filter,Object); }
};
}
URetakeGeometryCollectionComponent::URetakeGeometryCollectionComponent()
{
    SetIsReplicatedByDefault(true);SetEnableReplication(true);bEnableAbandonAfterLevel=false;
    SetReplicationMaxPositionAndVelocityCorrectionLevel(0);
    SetOneWayInteractionLevel(1); // Fragments cannot push two-way gameplay bodies.
}
void URetakeGeometryCollectionComponent::InitializeCollection(URetakeDestructionDefinition* D)
{
    const auto PendingState=RepStateData;
    const auto PendingDynamic=RepDynamicData;
    // SetRestCollection otherwise creates a proxy before applying asset defaults,
    // forcing a second costly physics-state creation to honor overrides.
    RestCollection=D->Collection;
    ApplyAssetDefaults();
    if (!D->DamageThresholds.IsEmpty()) DamageThreshold=D->DamageThresholds;
    if (D->PhysicalMaterial) BodyInstance.SetPhysMaterialOverride(D->PhysicalMaterial);
    bEnableDamageFromCollision=D->bUseAuthoredResponse && GetNetMode()!=NM_Client;
    SetOneWayInteractionLevel(D->bUseAuthoredResponse ? -1:1);
    // Contact flags must exist before physics creation: per-particle debris profiles take
    // ContactNotify only from bNotifyCollisions, never from the body instance flag.
    bNotifyCollisions=true;BodyInstance.bNotifyRigidBodyCollision=true;
    // Level-wide Chaos data effects read trailing data, as the reference collections generated it.
    bNotifyTrailing=bNotifyGlobalTrailings=!D->WorldChaosEffects.IsEmpty();
    if (D->bUseAuthoredResponse)
    {
        // The reference ran without iteration overrides: collections push their own 8/1/1
        // per-particle settings, which take precedence over the solver-wide values.
        BodyInstance.bStartAwake=D->bStartAwake;
        CanCharacterStepUpOn=ECB_No;
    }
    SetRestCollection(D->Collection,false);
    if (GetNetMode()==NM_Client)
    {
        RepStateData=PendingState;RepDynamicData=PendingDynamic;
        OnRep_RepStateData();OnRep_RepDynamicData();
    }
}
void URetakeGeometryCollectionComponent::OnCreatePhysicsState()
{
    Super::OnCreatePhysicsState();
    if (auto* Actor=Cast<ARetakeDestructibleActor>(GetOwner()); Actor && HasValidPhysicsState())
    {
        // OnRep/BeginPlay can precede physics creation. Do not consume a pending snapshot once and lose it, or destroy
        // a newly created proxy from inside this callback when the snapshot says the collection is already cleared.
        Actor->GetWorldTimerManager().ClearTimer(Actor->PhysicsStateRefreshTimer);
        Actor->PhysicsStateRefreshTimer=Actor->GetWorldTimerManager().SetTimerForNextTick(Actor,&ARetakeDestructibleActor::RefreshPhysicsState);
    }
}
void URetakeGeometryCollectionComponent::DispatchUniqueField(const FFieldSystemCommand& Command)
{
    check(IsInGameThread());
    auto* Proxy=GetPhysicsProxy();
    auto* Solver=Proxy ? Proxy->GetSolver<Chaos::FPBDRigidsSolver>():nullptr;
    if (!Solver || !Command.RootNode) return;
    check(Command.PhysicsType==EFieldPhysicsType::Field_ExternalClusterStrain || Command.PhysicsType==EFieldPhysicsType::Field_LinearVelocity || Command.PhysicsType==EFieldPhysicsType::Field_AngularVelociy);
    FFieldSystemCommand Local=Command;
    Local.InitFieldNodes(Solver->GetSolverTime(),GetFName());
    // Follow the native proxy external-command lifetime/ordering contract. No UObject
    // or game-thread collection is accessed from the solver command.
    Solver->EnqueueCommandImmediate([Proxy,Solver,Local=MoveTemp(Local)]()
    {
        if (Solver->IsShuttingDown()) return;
        TRACE_CPUPROFILER_EVENT_SCOPE(RetakeDestruction_UniqueField);
        FRetakeUniqueFieldParticles Particles{Proxy};FFieldExecutionDatas Data;
        auto Resolution=EFieldResolutionType::Field_Resolution_Max;
        auto Filter=EFieldFilterType::Field_Filter_Max;
        auto Object=EFieldObjectType::Field_Object_Max;
        auto Position=EFieldPositionType::Field_Position_Max;
        if (!Chaos::BuildFieldSamplePoints(&Particles,Solver,Local,Data,Resolution,Filter,Object,Position)) return;
        FFieldContext Context(Data,Local.MetaData,Solver->GetSolverTime()-Local.TimeCreation);
        auto& Handles=Data.ParticleHandles[(uint8)EFieldCommandHandlesType::InsideHandles];
        // These are the engine's external-strain and additive-velocity operations.
        // Avoid constructing the unrelated, non-exported position-constraint container.
        if (Local.PhysicsType==EFieldPhysicsType::Field_ExternalClusterStrain)
        {
            auto& Results=Data.ScalarResults[(uint8)EFieldCommandResultType::FinalResult];
            ResetResultsArray<float>(Data.SamplePositions.Num(),Results,0);
            TFieldArrayView<float> View(Results,0,Results.Num());
            static_cast<const FFieldNode<float>*>(Local.RootNode.Get())->Evaluate(Context,View);
            for (const FFieldContextIndex& Index:Context.GetEvaluatedSamples())
                if (Results[Index.Result]>0) if (auto* Particle=Handles[Index.Sample]->CastToClustered())
                    Solver->GetEvolution()->GetRigidClustering().SetExternalStrain(Particle,FMath::Max(Particle->GetExternalStrain(),Results[Index.Result]));
        }
        else
        {
            auto& Results=Data.VectorResults[(uint8)EFieldCommandResultType::FinalResult];
            ResetResultsArray<FVector>(Data.SamplePositions.Num(),Results,FVector::ZeroVector);
            TFieldArrayView<FVector> View(Results,0,Results.Num());
            static_cast<const FFieldNode<FVector>*>(Local.RootNode.Get())->Evaluate(Context,View);
            for (const FFieldContextIndex& Index:Context.GetEvaluatedSamples())
            {
                const FVector Delta=Results[Index.Result];
                auto* Particle=Handles[Index.Sample]->CastToRigidParticle();
                if (!Particle || !Particle->IsDynamic() || Delta.IsZero()) continue;
                if (Local.PhysicsType==EFieldPhysicsType::Field_LinearVelocity) Particle->SetV(Particle->GetV()+Delta);
                else Particle->SetW(Particle->GetW()+Delta);
                if (Particle->IsSleeping()) Solver->GetEvolution()->WakeParticle(Particle);
            }
        }
    });
}
void URetakeGeometryCollectionComponent::ApplyLocalizedDamage(const FVector& Point,float Radius,float Strain,const FVector& Direction,float DirectionalSpeed,float RadialSpeed,float AngularSpeed,bool bPointImpact,const FVector2D& NoiseRange,const FVector& SurfaceNormal,bool bAuthoredResponse)
{
    const auto StrainFalloff=(!bPointImpact && bAuthoredResponse) ? EFieldFalloffType::Field_Falloff_Squared:EFieldFalloffType::Field_Falloff_Linear;
    if (Strain>0) DispatchUniqueField(FFieldSystemCommand(EFieldPhysicsType::Field_ExternalClusterStrain,
        new FCullingField<float>(new FRadialFalloff(1,0,1,0,Radius,Point,EFieldFalloffType::Field_FallOff_None),
            new FRadialFalloff(Strain,0,1,0,Radius,Point,StrainFalloff),EFieldCullingOperationType::Field_Culling_Outside)));
    // Match the authored operator tree, including separate radial/directional commands,
    // noise, the 1.25 radial radius, and the delayed angular command after fracture.
    const auto VelocityFalloff=bPointImpact ? EFieldFalloffType::Field_FallOff_None:
        (bAuthoredResponse ? EFieldFalloffType::Field_Falloff_Squared:EFieldFalloffType::Field_Falloff_Linear);
    auto DispatchVelocity=[this,Point,NoiseRange,VelocityFalloff,bAuthoredResponse](EFieldPhysicsType Type,FFieldNode<FVector>* Vector,float Range)
    {
        auto* Noisy=new FSumVector(1,new FNoiseField(NoiseRange.X,NoiseRange.Y,FTransform(Point)),Vector);
        auto* Falloff=new FRadialFalloff(1,bAuthoredResponse ? .5f:0.f,1,0,Range,Point,VelocityFalloff);
        DispatchUniqueField(FFieldSystemCommand(Type,new FCullingField<FVector>(
            new FRadialFalloff(1,0,1,0,Range,Point,EFieldFalloffType::Field_FallOff_None),
            new FSumVector(1,Falloff,Noisy),EFieldCullingOperationType::Field_Culling_Outside)));
    };
    if (RadialSpeed>0) DispatchVelocity(EFieldPhysicsType::Field_LinearVelocity,new FRadialVector(RadialSpeed,Point),Radius*1.25f);
    const FVector Normal=SurfaceNormal.GetSafeNormal();
    const double Incidence=FVector::DotProduct(Normal,Direction);
    const FVector Reflected=Direction-2*Incidence*Normal;
    const FVector Deflected=Normal.IsNearlyZero() ? Direction:
        Direction*FVector(1,1,0)+Reflected*(1-FMath::Abs(Incidence));
    if (DirectionalSpeed>0) DispatchVelocity(EFieldPhysicsType::Field_LinearVelocity,new FUniformVector(DirectionalSpeed,Deflected),Radius);
    const FVector TorqueAxis=bPointImpact ? -FVector::CrossProduct(Normal,FVector::CrossProduct(Normal,Direction)).GetSafeNormal(.0001):FMath::VRand();
    if (AngularSpeed>0 && !TorqueAxis.IsNearlyZero())
    {
        FTimerHandle AngularTimer;
        GetWorld()->GetTimerManager().SetTimer(AngularTimer,FTimerDelegate::CreateWeakLambda(this,
            [this,DispatchVelocity,TorqueAxis,AngularSpeed,Radius]()
            {
                if (!HasValidPhysicsState() || !GetOwner() || GetOwner()->IsActorBeingDestroyed()) return;
                DispatchVelocity(EFieldPhysicsType::Field_AngularVelociy,
                    new FSumVector(AngularSpeed,nullptr,new FUniformVector(1,TorqueAxis),new FRandomVector(1)),Radius);
            }),.01f,false);
    }
}
void URetakeGeometryCollectionComponent::RemovePieces(const TArray<int32>& TransformIndices)
{
    check(IsInGameThread());
    auto* Proxy=GetPhysicsProxy();
    auto* Solver=Proxy ? Proxy->GetSolver<Chaos::FPBDRigidsSolver>():nullptr;
    if (!Solver || TransformIndices.IsEmpty()) return;
    // Mirrors the native late-join path: release the leaf and any unbroken ancestor top-down, then disable it. Chaos only
    // releases a child from a top-level cluster. A piece gone on the server had its ancestors broken there too.
    Solver->EnqueueCommandImmediate([Proxy,Solver,TransformIndices]()
    {
        if (Solver->IsShuttingDown() || !Proxy->IsInitializedOnPhysicsThread()) return;
        auto& Evolution=*Solver->GetEvolution();
        for (const int32 Index:TransformIndices)
        {
            auto* Handle=Proxy->GetParticle_Internal(Index);
            if (!Handle || Handle->ClusterIds().NumChildren!=0) continue;
            if (Handle->Parent()) Evolution.GetRigidClustering().ForceReleaseChildParticleAndParents(Handle,false);
            if (Handle->Disabled()) continue;
            Evolution.DisableParticle(Handle);
            Solver->GetParticles().MarkTransientDirtyParticle(Handle);
        }
    });
}
ARetakeDestructibleActor::ARetakeDestructibleActor()
{
    bReplicates=true; SetReplicateMovement(true); // Native GC applies corrections only to the configured root level.
    PrimaryActorTick.bCanEverTick=false;
    SetNetUpdateFrequency(20);SetMinNetUpdateFrequency(2);
    Tags.Add(TEXT("Retake.Visibility.Ignore"));
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
    Traversal=CreateDefaultSubobject<URetakeDestructibleTraversalComponent>(TEXT("Traversal"));
    Geometry=CreateDefaultSubobject<URetakeGeometryCollectionComponent>(TEXT("Geometry"));
    Geometry->SetupAttachment(RootComponent);
    Geometry->bEnableDamageFromCollision=false;
    Geometry->bAllowRemovalOnSleep=true;Geometry->bAllowRemovalOnBreak=true;
    Geometry->SetCollisionProfileName(TEXT("RetakeDestructionStructure"));
    Geometry->SetCanEverAffectNavigation(false);
    Geometry->SetNotifyBreaks(true);
    Frame=CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Frame"));
    Frame->SetupAttachment(RootComponent);Frame->SetCollisionProfileName(TEXT("RetakeDestructionStructure"));
    Frame->SetCanEverAffectNavigation(false);
}
void ARetakeDestructibleActor::Initialize(URetakeDestructionDefinition* D,uint32 InGeneration)
{
    check(GetNetMode()!=NM_Client && HasAuthority());Definition=D;Generation=InGeneration;
    if (Placement && Placement->Door) SetReplicateMovement(false); // Every machine hangs a door leaf on its own swinging pivot.
    Configure();
}
void ARetakeDestructibleActor::Configure()
{
    if (bConfigured || !Definition || !Definition->Collection) return;
    bConfigured=true;
    // A26-01 F6: a client creates the collection's physics where the server did, not at the rounded replicated pose.
    if (GetNetMode()==NM_Client) HoldPlacementTransform(*this);
    if (!bCleared)
    {
        Geometry->InitializeCollection(Definition);
        if (!Definition->bUseAuthoredResponse) Geometry->SetAnchoredByIndex(Geometry->GetRootIndex(),true); // Stable intact props; authored child anchors survive fracture.
    }
    for (int32 I=0;I<Definition->Materials.Num();++I) if (Definition->Materials[I]) Geometry->SetMaterial(I,Definition->Materials[I]);
    if (GetNetMode()!=NM_DedicatedServer) if (auto* System=GetWorld()->GetSubsystem<URetakeDestructionSubsystem>()) System->EnsureWorldEffects(Definition);
    Frame->SetStaticMesh(Definition->FrameMesh);
    if (Definition->bUseAuthoredResponse) Frame->SetCollisionProfileName(TEXT("BlockAllDynamic"));
    Frame->SetCollisionEnabled(Definition->FrameMesh ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
    SetNetCullDistanceSquared(FMath::Square(Definition->RelevanceDistanceCm));
    Geometry->OnChaosBreakEvent.AddUniqueDynamic(this,&ThisClass::HandleBreak);
    Geometry->OnChaosPhysicsCollision.AddUniqueDynamic(this,&ThisClass::HandleCollision);
    Geometry->OnFullyDecayedEvent.AddUniqueDynamic(this,&ThisClass::HandleFullyDecayed);
    Geometry->OnRootMovedEvent.AddUniqueDynamic(this,&ThisClass::HandleRootMoved);
    // Historical snapshots do not open a debris-audio window.
    bTrackingDebris=Definition->bUseAuthoredResponse;
    for (const FBox& Box:Definition->AnchorBoxes) Geometry->SetAnchoredByTransformedBox(Box,GetActorTransform(),true);
    if (GetNetMode()!=NM_Client) {ReleasedPieces.Reset(Definition->PreparedTransformCount);GonePieces.Reset(Definition->PreparedTransformCount);}
    CaptureRestPose();
    ApplyReleasedCollision();
    RemoveGonePiecesOnJoin();
    ApplyFractureState();
    ApplyRootYield();
}
void ARetakeDestructibleActor::BeginPlay()
{
    Super::BeginPlay();Configure();AttachToDoor();ApplyCollectionShatterState();
    CaptureRestPose();RemoveGonePiecesOnJoin(); // Configure can run before the collection has component-space transforms.
    if (auto* System=GetWorld()->GetSubsystem<URetakeDestructionSubsystem>()) System->RegisterActor(this);
    if (GetNetMode()!=NM_Client)
    {
        if (Definition && Definition->bUseAuthoredResponse)
        {LastDamageTime=GetWorld()->GetTimeSeconds();GetWorldTimerManager().SetTimer(SettleTimer,this,&ThisClass::RefreshReleased,.05f,true);}
        else SetNetDormancy(DORM_DormantAll);
    }
}
void ARetakeDestructibleActor::EndPlay(const EEndPlayReason::Type Reason)
{
    GetWorldTimerManager().ClearAllTimersForObject(this);
    GetWorldTimerManager().ClearAllTimersForObject(Geometry);
    StopCollisionAudio();
    StopDebrisTracking();
    if (auto* System=GetWorld()->GetSubsystem<URetakeDestructionSubsystem>()) System->UnregisterActor(this);
    Super::EndPlay(Reason);
}
void ARetakeDestructibleActor::PostNetReceiveLocationAndRotation()
{
    // Initial and later replicated movement alike, intact or broken: the server's root stays at the placement, so the replicated
    // pose is only its rounded copy. Native collection replication still corrects the root level (it needs movement replication).
    if (IsValid(Placement) && !Placement->Door) {HoldPlacementTransform(*this);return;}
    Super::PostNetReceiveLocationAndRotation(); // Placement not resolved yet: OnRep_Placement places it.
}
void ARetakeDestructibleActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME_CONDITION(ARetakeDestructibleActor,Definition,COND_InitialOnly);
    DOREPLIFETIME_CONDITION(ARetakeDestructibleActor,Generation,COND_InitialOnly);
    DOREPLIFETIME_CONDITION(ARetakeDestructibleActor,Placement,COND_InitialOnly);
    DOREPLIFETIME(ARetakeDestructibleActor,ReleasedPieces);
    DOREPLIFETIME(ARetakeDestructibleActor,GonePieces);
    DOREPLIFETIME(ARetakeDestructibleActor,bFractured);
    DOREPLIFETIME(ARetakeDestructibleActor,bCleared);
    DOREPLIFETIME(ARetakeDestructibleActor,bRootYieldsToPawns);
    DOREPLIFETIME(ARetakeDestructibleActor,bDoorCollapsed);
    DOREPLIFETIME(ARetakeDestructibleActor,bCollectionShattered);
}
void ARetakeDestructibleActor::OnRep_Definition() {Configure();AttachToDoor();ApplyCollectionShatterState();}
void ARetakeDestructibleActor::OnRep_Placement() {HoldPlacementTransform(*this);RefreshTraversal();AttachToDoor();ApplyCollectionShatterState();}
void ARetakeDestructibleActor::OnRep_Released() {ApplyReleasedCollision();}
void ARetakeDestructibleActor::OnRep_FractureState() {ApplyFractureState();}
void ARetakeDestructibleActor::OnRep_RootYield() {ApplyRootYield();}
void ARetakeDestructibleActor::OnRep_DoorCollapsed() {if (bDoorCollapsed && bDoorLeaf) Geometry->RemoveAllAnchors();}
void ARetakeDestructibleActor::OnRep_CollectionShattered() {ApplyCollectionShatterState();}
void ARetakeDestructibleActor::ApplyCollectionShatterState()
{
    if (!bCollectionShattered || !bConfigured || !Geometry || !Geometry->HasValidPhysicsState()) return;
    Geometry->RemoveAllAnchors();
    if (GetNetMode()!=NM_Client && !GetWorld()->IsPlayingReplay()) Geometry->CrumbleActiveClusters();
}
void ARetakeDestructibleActor::RefreshPhysicsState()
{
    if (!HasActorBegunPlay() || IsActorBeingDestroyed() || !bConfigured) return;
    if (bCleared) {AttachToDoor();ApplyFractureState();return;}
    if (!Geometry->HasValidPhysicsState()) return;
    // These flags describe work on a specific proxy. A recreated proxy needs the same persistent state projected again.
    bRootYieldApplied=false;bJoinStateApplied=false;bDoorLeaf=false;
    AppliedCollisionMask.Reset(Definition->PreparedTransformCount);
    CaptureRestPose();ApplyReleasedCollision();RemoveGonePiecesOnJoin();ApplyFractureState();ApplyRootYield();
    AttachToDoor();ApplyCollectionShatterState();
}
void ARetakeDestructibleActor::AttachToDoor()
{
    ARetakeDoor* Door=Placement ? Placement->Door.Get():nullptr;
    if (!bConfigured || !Door || !Door->DoorPivot || !HasActorBegunPlay()) return;
    // A cleared snapshot intentionally has no physics to attach. It still owns the door's visual leaf: otherwise a
    // reconnect/late join leaves the map's original static mesh visible after its collection has disappeared.
    Door->HandOverLeaf();
    if (HasAuthority() && (bCleared || bDoorCollapsed)) Door->BreakLeaf();
    if (bDoorLeaf || bCleared) return;
    // Physics creation schedules RefreshPhysicsState; unresolved placement references have their own OnRep.
    if (!Geometry->HasValidPhysicsState()) return;
    bDoorLeaf=true;
    // The leaf's offset from the hinge comes from the door's closed pose, so a leaf spawned while the door stands open (a round
    // reset ordered before the door's own) still hangs right.
    const FTransform Relative=Placement->GetActorTransform().GetRelativeTransform(Door->GetClosedPivotTransform());
    AttachToComponent(Door->DoorPivot,FAttachmentTransformRules::SnapToTargetNotIncludingScale);
    SetActorRelativeTransform(Relative);
    // The door keeps its own leaf as the pawn body (blocking and the swing push); this collection answers shots, blasts and sight.
    Geometry->SetCollisionResponseToChannel(ECC_Pawn,ECR_Ignore);
    // The hinge-edge strip never breaks and holds the leaf up. Long is the leaf's width axis; its hinge end is the one nearer the hinge.
    const FBox Local=Geometry->CalcBounds(FTransform::Identity).GetBox();
    const FVector Hinge=GetActorTransform().InverseTransformPosition(Door->GetHingeLocation());
    const int32 Long=Local.GetSize().X>=Local.GetSize().Y ? 0:1;
    const double Edge=FMath::Max(1.f,Placement->DoorEdgeCm);
    FBox Strip=Local.ExpandBy(1.);
    if (FMath::Abs(Hinge[Long]-Local.Max[Long])<FMath::Abs(Hinge[Long]-Local.Min[Long])) Strip.Min[Long]=Local.Max[Long]-Edge;
    else Strip.Max[Long]=Local.Min[Long]+Edge;
    Geometry->SetAnchoredByTransformedBox(Strip,GetActorTransform(),true);
    if (bDoorCollapsed) Geometry->RemoveAllAnchors();
}
void ARetakeDestructibleActor::TryCollapseDoorLeaf()
{
    ARetakeDoor* Door=bDoorLeaf && Placement ? Placement->Door.Get():nullptr;
    const FGeometryCollection* Rest=Definition && Definition->Collection ? Definition->Collection->GetGeometryCollection().Get():nullptr;
    if (!Door || !Rest || bDoorCollapsed || bCleared || GetNetMode()==NM_Client) return;
    int32 Pieces=0,Broken=0;
    for (int32 I=0;I<FMath::Min(Rest->TransformToGeometryIndex.Num(),Definition->PreparedTransformCount);++I)
        if (Rest->TransformToGeometryIndex[I]!=INDEX_NONE) {++Pieces;Broken+=IsReleased(I);}
    if (!Pieces || Broken<Placement->DoorCollapseFraction*Pieces) return;
    // The rest of the leaf falls away, hinge strip included, and the debris cleanup clears it: only the door's frame remains.
    FlushNetDormancy();bDoorCollapsed=true;ForceNetUpdate();
    Geometry->RemoveAllAnchors();
    const FBox Bounds=Geometry->Bounds.GetBox();
    Geometry->ApplyLocalizedDamage(Bounds.GetCenter(),Bounds.GetExtent().Size()+10.f,RetakeDestructionStrain::Shatter,FVector::DownVector,
        0,Definition->RadialImpulseSpeed,0,false,Definition->ImpulseNoiseRange,FVector::ZeroVector,Definition->bUseAuthoredResponse);
    GetWorldTimerManager().SetTimer(SettleTimer,this,&ThisClass::RefreshReleased,.05f,true);
    if (RetakeDestructionDebugEnabled())
        UE_LOGF(LogRetakeDestruction,Log,"event=DoorLeafCollapsed actor=%ls door=%ls broken=%d pieces=%d",*GetName(),*Door->GetName(),Broken,Pieces);
    Door->BreakLeaf();
}
void ARetakeDestructibleActor::ApplyRootYield()
{
    // Reference IgnoreCharChaos switch on bone 0 only: pawns stop colliding with the intact
    // root; weapons, explosions and visibility still query it. Released pieces keep their own profile.
    if (!bConfigured || !bRootYieldsToPawns || bRootYieldApplied || bCleared || IsReleased(Geometry->GetRootIndex())) return;
    bRootYieldApplied=true;
    Geometry->SetPerParticleCollisionProfileName(TArray<int32>{Geometry->GetRootIndex()},FName(TEXT("RetakeDestructionYield")));
    RefreshTraversal();
}
bool ARetakeDestructibleActor::CanTraverseIntact() const
{
    if (!bConfigured || !Traversal->bTraversalEnabled || !IsValid(Placement) || Placement->IsActorBeingDestroyed()
        || !Placement->Traversal || !Placement->Traversal->bTraversalEnabled || Placement->Door || bFractured || bCleared
        || bRootYieldsToPawns || !ReleasedPieces.IsEmpty() || !Geometry->HasValidPhysicsState() || Geometry->IsRootBroken()
        || Geometry->GetRootIndex()==INDEX_NONE || !Geometry->IsQueryCollisionEnabled()
        || Geometry->GetCollisionResponseToChannel(ECC_Pawn)!=ECR_Block) return false;
    // Chaos can move an intact root without moving the actor's scene root. The authored splines must still coincide
    // with the actual collection, including rotation/scale; stopping elsewhere does not make it an authored ledge.
    return GetActorTransform().Equals(Placement->GetActorTransform(),.01f)
        && Geometry->GetComponentTransform().Equals(Placement->GetActorTransform(),.01f)
        && Geometry->GetRootCurrentTransform().Equals(Geometry->GetRootInitialTransform(),.01f);
}
bool ARetakeDestructibleActor::CanKeepForRound() const
{
    // Clients cannot rebuild broken structure (native replication only breaks), so whatever may differ from a new actor on any
    // machine is replaced. A hit counts even when nothing broke: a kinematic field or impulse can change pieces.
    if (GetNetMode()==NM_Client || !HasAuthority() || bHitSinceSpawn || !bConfigured || !Definition || !Definition->Collection
        || !IsValid(Placement) || Placement->IsActorBeingDestroyed() || Placement->Definition!=Definition
        || bFractured || bCleared || bRootYieldsToPawns || bDoorCollapsed || bCollectionShattered || !ReleasedPieces.IsEmpty() || !GonePieces.IsEmpty()
        || !Geometry->HasValidPhysicsState() || Geometry->IsRootBroken() || !HasAnchoredRoot()) return false;
    // The pose a new actor takes: the placement's (a door leaf's when its door is closed), with the root where the collection
    // authored it. Same tolerance as the intact-traversal and occlusion checks.
    return GetActorTransform().Equals(Placement->GetActorTransform(),.01f)
        && Geometry->GetComponentTransform().Equals(Placement->GetActorTransform(),.01f)
        && Geometry->GetRootCurrentTransform().Equals(Geometry->GetRootInitialTransform(),.01f);
}
bool ARetakeDestructibleActor::HasAnchoredRoot() const
{
    // An anchored piece makes Chaos hold the whole intact cluster kinematic (UpdateKinematicProperties), on every machine, so
    // nothing can push it. Bounded cleanup anchors the root itself (Configure) and a door leaf its hinge strip (AttachToDoor);
    // an authored collection relies on its own anchors. An unanchored root is dynamic: client-side debris may have pushed a
    // client's copy, and only a new actor puts it back.
    if (!Definition->bUseAuthoredResponse || bDoorLeaf) return true;
    const FGeometryCollection* Rest=Definition->Collection->GetGeometryCollection().Get();
    if (!Rest) return false;
    const Chaos::Facades::FCollectionAnchoringFacade Anchors(*Rest);
    if (!Anchors.HasAnchoredAttribute()) return false;
    for (int32 I=0;I<Rest->Transform.Num();++I) if (Anchors.IsAnchored(I)) return true;
    return false;
}
void ARetakeDestructibleActor::ContinueInGeneration(uint32 InGeneration)
{
    check(GetNetMode()!=NM_Client && HasAuthority());
    // Only the server's Commit reads the generation. The initial-only replicated copy keeps the spawn value; the actor stays dormant.
    Generation=InGeneration;
}
void ARetakeDestructibleActor::RefreshTraversal()
{
    Traversal->InitializeFromPlacement();
    if (!bConfigured || !Definition || !Definition->Collection) return;
    const int32 Root=Geometry->GetRootIndex();
    if (Root==INDEX_NONE) return;
    const bool bEnable=Traversal->bTraversalEnabled && !bFractured && !bCleared && !bRootYieldsToPawns && ReleasedPieces.IsEmpty();
    if (bEnable==bTraversalCollisionApplied) return;
    // Override only the intact root's trace profile. Component-wide filter updates can overwrite released particles'
    // collision profiles; their cosmetic/nonblocking contract must survive disabling traversal and late joins.
    if (bEnable) Geometry->SetPerParticleCollisionProfileName(TArray<int32>{Root},TEXT("RetakeDestructionTraversable"));
    else if (!IsReleased(Root) && !bRootYieldsToPawns)
        Geometry->SetPerParticleCollisionProfileName(TArray<int32>{Root},TEXT("RetakeDestructionStructure"));
    bTraversalCollisionApplied=bEnable;
}
void ARetakeDestructibleActor::HandleRootMoved()
{
    if (!bConfigured || bFractured || bCleared || Geometry->IsRootBroken() || !Geometry->HasValidPhysicsState()) return;
    if (GetNetMode()!=NM_Client)
    {
        FlushNetDormancy();SetNetDormancy(DORM_Awake);
        if (!GetWorldTimerManager().IsTimerActive(SettleTimer)) GetWorldTimerManager().SetTimer(SettleTimer,this,&ThisClass::RefreshReleased,.05f,true);
        if (auto* System=GetWorld()->GetSubsystem<URetakeDestructionSubsystem>()) System->NotifyStructureChanged();
    }
}
bool ARetakeDestructibleActor::IsReleased(int32 Index) const {return ReleasedPieces.Contains(Index);}
bool ARetakeDestructibleActor::ResolveGrenadePass(const FHitResult& Hit,const FVector& Velocity,float ContactDamage,float& OutSpeedScale)
{
    OutSpeedScale=1;
    const auto* A=Cast<ARetakeDestructibleActor>(Hit.GetActor());
    // Only the intact collection yields: an authored frame always bounces the grenade, and a released piece is debris.
    if (!A || !A->Definition || A->bCleared || !Hit.bBlockingHit || Hit.bStartPenetrating
        || Hit.GetComponent()!=A->Geometry || A->IsReleased(Hit.Item)) return false;
    return A->Definition->AllowsGrenadePass(ContactDamage,float(-FVector::DotProduct(Velocity,Hit.Normal.GetSafeNormal())),OutSpeedScale);
}
bool ARetakeDestructibleActor::TraceLooseDebris(const FVector& Start,const FVector& End,int32& OutItem,FVector& OutPoint,FVector& OutNormal,double& OutDistance)
{
    if (!bConfigured || !HasLooseDebris() || !Definition->Collection || !Geometry->HasValidPhysicsState()
        || !FMath::LineBoxIntersection(Geometry->Bounds.GetBox(),Start,End,End-Start)) return false;
    TRACE_CPUPROFILER_EVENT_SCOPE(RetakeDestruction_TraceDebris);
    const auto& Rest=*Definition->Collection->GetGeometryCollection();
    auto* Data=Geometry->GetDynamicCollection();
    if (!Data) return false;
    FGeometryCollectionDynamicStateFacade State(*Data);
    if (!State.IsValid()) return false;
    const TArray<FTransform3f>& Pieces=Geometry->GetComponentSpaceTransforms3f();
    const FTransform& Component=Geometry->GetComponentTransform();
    const FVector LocalStart=Component.InverseTransformPosition(Start),LocalEnd=Component.InverseTransformPosition(End);
    float Best=2;
    for (int32 I=0;I<FMath::Min3(Pieces.Num(),Rest.TransformToGeometryIndex.Num(),Data->GetNumTransforms());++I)
    {
        const int32 GeometryIndex=Rest.TransformToGeometryIndex[I];
        // Leaf pieces flying alone or inside a detached cluster; removed pieces are inactive and parentless.
        if (GeometryIndex==INDEX_NONE || !Rest.BoundingBox.IsValidIndex(GeometryIndex) || !IsReleased(I)
            || !((State.IsActive(I) && State.IsDynamicOrSleeping(I)) || State.HasDynamicInternalClusterParent(I))) continue;
        const FTransform Piece(Pieces[I]);
        const FVector A=Piece.InverseTransformPosition(LocalStart),B=Piece.InverseTransformPosition(LocalEnd);
        FVector Point,Normal;float Time;
        if (FMath::LineExtentBoxIntersection(FBox(Rest.BoundingBox[GeometryIndex]),A,B,FVector::ZeroVector,Point,Normal,Time) && Time<Best)
        {
            Best=Time;OutItem=I;
            OutPoint=Component.TransformPosition(Piece.TransformPosition(Point));
            OutNormal=Component.TransformVectorNoScale(Piece.TransformVectorNoScale(Normal)).GetSafeNormal();
        }
    }
    if (Best>1) return false;
    OutDistance=FVector::Distance(Start,OutPoint);
    return true;
}
FString ARetakeDestructibleActor::DescribeState() const
{
    int32 Active=0,Moving=0,ActiveMoving=0,Sleeping=0,BrokenOff=0,InternalParent=0,Released=0,Gone=0;
    if (const auto* Data=Geometry ? Geometry->GetDynamicCollection():nullptr)
    {
        FGeometryCollectionDynamicStateFacade State(*const_cast<FGeometryDynamicCollection*>(Data));
        if (State.IsValid()) for (int32 I=0;I<Data->GetNumTransforms();++I)
        {
            const bool bMoving=State.IsDynamicOrSleeping(I) && !State.IsSleeping(I);
            Active+=State.IsActive(I); Moving+=bMoving; ActiveMoving+=State.IsActive(I) && bMoving; Sleeping+=State.IsSleeping(I);
            BrokenOff+=State.HasBrokenOff(I); InternalParent+=State.HasDynamicInternalClusterParent(I); Released+=IsReleased(I); Gone+=IsGone(I);
        }
    }
    return FString::Printf(TEXT("configured=%d fractured=%d cleared=%d root_broken=%d root_moved=%d physics_state=%d simulating=%d transforms=%d active=%d moving=%d active_moving=%d sleeping=%d broken_off=%d internal_parent=%d released=%d gone=%d dormancy=%d"),
        bConfigured,bFractured,bCleared,Geometry && Geometry->IsRootBroken(),
        Geometry && !Geometry->GetRootCurrentTransform().Equals(Geometry->GetRootInitialTransform(),.01),
        Geometry && Geometry->HasValidPhysicsState(),Geometry && Geometry->IsSimulatingPhysics(),
        Geometry && Geometry->GetDynamicCollection() ? Geometry->GetDynamicCollection()->GetNumTransforms():0,
        Active,Moving,ActiveMoving,Sleeping,BrokenOff,InternalParent,Released,Gone,(int32)NetDormancy);
}

void ARetakeDestructibleActor::ApplyDamageAt(const FRetakeDestructionHit& Hit,float Damage,float DamageFraction)
{
    const auto* Response=Hit.FieldDefinition.IsValid() ? Hit.FieldDefinition.Get():Definition.Get();
    const bool bExplosion=Hit.Radius>0;
    // A breaking hit always opens its whole radius, whatever this collection's thresholds (a grenade flying through).
    const bool bBreak=Hit.BreakRadius>0;
    if (RetakeDestructionDebugEnabled() && HasAuthority())
        UE_LOGF(LogRetakeDestruction,Log,"event=DestructionDamage actor=%ls definition=%ls point=%ls damage=%.2f radius=%.1f point_radius=%.1f strain=%.0f fraction=%.3f direct=%d debris=%d break_radius=%.1f accepted=%d %ls",
            *GetName(),*GetNameSafe(Definition),*Hit.Point.ToCompactString(),Damage,Hit.Radius,Response ? Response->PointRadiusCm:0.f,
            Response && !Response->bUseAuthoredResponse ? Damage*Response->StrainPerDamage:0.f,DamageFraction,Hit.bDirectImpact,Hit.bDebrisImpact,Hit.BreakRadius,
            !(GetNetMode()==NM_Client || !bConfigured || bCleared || !Geometry->HasValidPhysicsState()),*DescribeState());
    if (GetNetMode()==NM_Client || !HasAuthority() || !bConfigured || bCleared || !Response || !Geometry->HasValidPhysicsState()) return;
    TRACE_CPUPROFILER_EVENT_SCOPE(RetakeDestruction_ApplyStrain);
    bHitSinceSpawn=true;
    FlushNetDormancy();SetNetDormancy(DORM_Awake);
    LastDamageTime=GetWorld()->GetTimeSeconds();
    BeginDebrisSettling();
    if (Hit.bShatterCollection && !bCollectionShattered)
    {
        bCollectionShattered=true;ApplyCollectionShatterState();ForceNetUpdate();
    }
    const float Range=bBreak ? Hit.BreakRadius:bExplosion ? Hit.Radius:Response->PointRadiusCm;
    const float Strain=bBreak ? RetakeDestructionStrain::Shatter
        :Response->bUseAuthoredResponse ? (bExplosion ? RetakeDestructionStrain::Shatter:RetakeDestructionStrain::AuthoredShot)*DamageFraction
        :Damage*Response->StrainPerDamage;
    if (!bExplosion && Hit.bDirectImpact && Definition->bUseAuthoredResponse && !Definition->bHasKinematicPieces)
        Geometry->ApplyKinematicField(150,GetActorLocation());
    Geometry->ApplyLocalizedDamage(Hit.Point,Range,Strain,Hit.Direction,
        bExplosion ? 0:Response->ImpulseSpeed,bExplosion ? Response->ExplosionImpulseSpeed:Response->RadialImpulseSpeed,
        Response->AngularImpulseSpeed,!bExplosion,Response->ImpulseNoiseRange,Hit.SurfaceNormal,Response->bUseAuthoredResponse);
    if (Hit.bDebrisImpact || LastDamageTime-LastImpactPresentation>=.05)
    {
        if (!Hit.bDebrisImpact) LastImpactPresentation=LastDamageTime;
        MulticastImpact(Hit.Point,Hit.SurfaceNormal.IsNearlyZero() ? -Hit.Direction:Hit.SurfaceNormal,
            RetakeDestructionPresentation::Impact|(Hit.bDebrisImpact ? RetakeDestructionPresentation::DebrisImpact:0));
    }
    GetWorldTimerManager().SetTimer(SettleTimer,this,&ThisClass::RefreshReleased,.05f,true);
}
void ARetakeDestructibleActor::HandleBreak(const FChaosBreakEvent& Event)
{
    if (RetakeDestructionDebugEnabled() && GetNetMode()!=NM_Client)
        UE_LOGF(LogRetakeDestruction,Log,"event=DestructionBreak actor=%ls index=%d location=%ls mass=%.2f fractured_before=%d",
            *GetName(),Event.Index,*Event.Location.ToCompactString(),Event.Mass,bFractured);
    if (GetNetMode()==NM_Client) return; // Receiving a historical snapshot never emits one-shot presentation.
    LastDamageTime=GetWorld()->GetTimeSeconds();
    FlushNetDormancy();SetNetDormancy(DORM_Awake);
    GetWorldTimerManager().SetTimer(SettleTimer,this,&ThisClass::RefreshReleased,.05f,true);
    if (!bFractured) {FlushNetDormancy();bFractured=true;ApplyFractureState();ForceNetUpdate();}
    ReleaseBroken(Event.Index);
    RefreshReleased();
    const double Now=GetWorld()->GetTimeSeconds();
    // Mirror the authored solver event filter locally, without changing Mover's world solver.
    if (Definition->bUseAuthoredResponse && (Event.Mass<1 || Event.Velocity.SizeSquared()<2500 || Event.Extents.X*Event.Extents.Y*Event.Extents.Z<1)) return;
    uint8 Flags=0;
    if (Definition->BreakSound && Now>=NextBreakSound) {Flags|=RetakeDestructionPresentation::BreakSound;NextBreakSound=Now+SoundInterval(Definition->BreakSoundFrequencyHz);}
    if (Definition->BreakEffect && Event.Velocity.SizeSquared()>=FMath::Square(Definition->MinimumEffectSpeed) && Now>=NextBreakEffect)
    {Flags|=RetakeDestructionPresentation::BreakEffect;NextBreakEffect=Now+FMath::Max(.05,FMath::FRandRange(Definition->BreakEffectCooldownSeconds.X,Definition->BreakEffectCooldownSeconds.Y));}
    if (Flags) MulticastImpact(Event.Location,FVector::UpVector,Flags);
}
void ARetakeDestructibleActor::HandleFullyDecayed()
{
    if (GetNetMode()==NM_Client || bCleared) return;
    FlushNetDormancy();bCleared=true;bFractured=true;ForceNetUpdate();
    GetWorldTimerManager().ClearTimer(SettleTimer);
    // Do not destroy a physics proxy inside its own post-physics callback.
    GetWorldTimerManager().SetTimerForNextTick(this,&ThisClass::FinishClearedState);
}
void ARetakeDestructibleActor::FinishClearedState()
{
    AttachToDoor(); // Full decay also clears the pawn body, even if it overtook the collapse-counting timer.
    ApplyFractureState();
    if (auto* System=GetWorld()->GetSubsystem<URetakeDestructionSubsystem>()) System->NotifyStructureChanged();
    ForceNetUpdate();SetNetDormancy(DORM_DormantAll);
}
bool ARetakeDestructibleActor::HasLeftRest(int32 Index) const
{
    // Breaking off the root is not leaving: kinematic pieces, anchored islands and pieces still resting in the wall stay
    // where they were and still hide, block shots and block pawns. Before this, one hit counted every piece of a
    // single-cluster wall as released while it stood (2026-09-28).
    const TArray<FTransform3f>& Current=const_cast<URetakeGeometryCollectionComponent*>(Geometry.Get())->GetComponentSpaceTransforms3f();
    if (!RestPieceTransforms.IsValidIndex(Index) || !Current.IsValidIndex(Index)) return true;
    const FTransform3f& Rest=RestPieceTransforms[Index]; const FTransform3f& Now=Current[Index];
    // Removal shrinks a piece where it lies, so scale counts as leaving too.
    return FVector3f::DistSquared(Rest.GetTranslation(),Now.GetTranslation())>1.f
        || Rest.GetRotation().AngularDistance(Now.GetRotation())>FMath::DegreesToRadians(2.f)
        || Now.GetScale3D().GetMin()<Rest.GetScale3D().GetMin()*.5f;
}
void ARetakeDestructibleActor::CaptureRestPose()
{
    if (RestPieceTransforms.IsEmpty() && Geometry->GetRestCollection()) RestPieceTransforms=Geometry->GetComponentSpaceTransforms3f();
}

void ARetakeDestructibleActor::ReleaseBroken(int32 Index)
{
    // The reference switches a broken piece to IgnoreCharChaos on its break event, so shards left hanging in a frame
    // never block players. Visibility still keeps a piece until it leaves its rest pose (IsGone).
    auto* Data=Geometry->GetDynamicCollection();
    if (!Data || Index<0 || Index>=Data->GetNumTransforms() || Index==Geometry->GetRootIndex()) return;
    bool Changed=false;
    for (int32 I=0;I<Data->GetNumTransforms();++I)
    {
        if (IsReleased(I)) continue;
        bool Under=false;
        for (int32 Bone=I,Depth=0;Bone!=INDEX_NONE && Depth<Data->GetNumTransforms();++Depth,Bone=Data->GetParent(Bone)) if (Bone==Index) { Under=true; break; }
        if (!Under) continue;
        if (!Changed) FlushNetDormancy();
        Changed|=ReleasedPieces.Add(I);
    }
    if (Changed)
    {
        ApplyReleasedCollision(); ForceNetUpdate();
        if (auto* System=GetWorld()->GetSubsystem<URetakeDestructionSubsystem>()) System->NotifyStructureChanged();
    }
}

void ARetakeDestructibleActor::RefreshReleased()
{
    if (GetNetMode()==NM_Client || !HasAuthority() || !Definition || bCleared) return;
    if (LastReleaseFrame==GFrameCounter) return;
    LastReleaseFrame=GFrameCounter;
    auto* Data=Geometry->GetDynamicCollection();
    if (!Data) return;
    FGeometryCollectionDynamicStateFacade State(*Data);
    if (!State.IsValid()) return;
    bool Changed=false,HasMovingBodies=false;
    auto MarkGone=[&](int32 I)
    {
        if (GonePieces.Contains(I) || !HasLeftRest(I)) return;
        if (!Changed) FlushNetDormancy(); // Wake before modifying the replicated baseline.
        Changed|=GonePieces.Add(I);
    };
    for (int32 I=0;I<Data->GetNumTransforms();++I)
    {
        HasMovingBodies|=State.IsActive(I) && State.IsDynamicOrSleeping(I) && !State.IsSleeping(I);
        if (IsReleased(I)) {MarkGone(I);continue;}
        bool Detached=false;
        for (int32 Bone=I,Depth=0;Bone!=INDEX_NONE && Depth<Data->GetNumTransforms();++Depth)
        {
            if (State.HasDynamicInternalClusterParent(Bone)
                || (Bone!=Geometry->GetRootIndex() && State.HasBrokenOff(Bone) && State.IsDynamicOrSleeping(Bone)))
            {Detached=true;break;}
            Bone=Data->GetParent(Bone);
        }
        if (Detached && !HasLeftRest(I)) continue;
        if (!Detached) continue;
        if (!Changed) FlushNetDormancy(); // Wake before modifying the replicated baseline.
        if (ReleasedPieces.Add(I)) {Changed=true;TRACE_COUNTER_INCREMENT(RetakeDestructionFragments);}
        MarkGone(I);
    }
    if (Changed)
    {
        if (RetakeDestructionDebugEnabled()) UE_LOGF(LogRetakeDestruction,Log,"event=DestructionReleased actor=%ls %ls",*GetName(),*DescribeState());
        FlushNetDormancy();ApplyReleasedCollision();ForceNetUpdate();
        if (auto* System=GetWorld()->GetSubsystem<URetakeDestructionSubsystem>()) System->NotifyStructureChanged();
    }
    TryCollapseDoorLeaf(); // Break events release pieces before this pass, so check every pass, not only when it found new ones.
    // Retain the timer only through the bounded debris lifetime, never on idle map props. Detached pieces still at rest
    // are rechecked while the timer runs; the next hit restarts it.
    if (GetWorld()->GetTimeSeconds()-LastDamageTime>(Definition->bUseAuthoredResponse ? 1:Definition->MaximumDebrisSeconds+1) && (!Definition->bUseAuthoredResponse || !HasMovingBodies))
    {GetWorldTimerManager().ClearTimer(SettleTimer);ForceNetUpdate();SetNetDormancy(DORM_DormantAll);}
}
void ARetakeDestructibleActor::ApplyReleasedCollision()
{
    if (!bConfigured) return;
    TArray<int32> Changed;
    for (int32 I=0;I<Definition->PreparedTransformCount;++I)
        if (IsReleased(I) && !AppliedCollisionMask.Contains(I)) Changed.Add(I);
    if (!Changed.IsEmpty()) Geometry->SetPerParticleCollisionProfileName(Changed,Definition->bUseAuthoredResponse ? TEXT("RetakeDestructionAuthoredDebris"):TEXT("RetakeDestructionDebris"));
    AppliedCollisionMask=ReleasedPieces;
    if (!Changed.IsEmpty()) ApplyFractureState();
}
void ARetakeDestructibleActor::RemoveGonePiecesOnJoin()
{
    // Native Geometry Collection replication rebuilds a joining client from broken flags alone. A piece that broke off
    // inside a larger chunk has no flag of its own, so the client would keep it standing at rest after the server dropped
    // or removed it. Runs once, with the initial state (joining or becoming relevant again); the replicated properties of
    // that bunch are all set before Configure runs from OnRep_Definition.
    if (GetNetMode()!=NM_Client || bJoinStateApplied || !bConfigured || RestPieceTransforms.IsEmpty() || !Geometry->HasValidPhysicsState()) return;
    bJoinStateApplied=true;
    if (bCleared) return;
    TArray<int32> Standing;
    for (int32 I=0;I<Definition->PreparedTransformCount;++I) if (GonePieces.Contains(I) && !HasLeftRest(I)) Standing.Add(I);
    if (Standing.IsEmpty()) return;
    Geometry->RemovePieces(Standing);
}
void ARetakeDestructibleActor::ApplyFractureState()
{
    if (!bConfigured) return;
    RefreshTraversal();
    Geometry->SetVisibility(!bCleared);
    if (!bMaterialsBroken && (bFractured || bCleared))
    {
        bMaterialsBroken=true;
        for (int32 I=0;I<Definition->BreakMaterialIndices.Num() && I<Definition->BreakMaterials.Num();++I)
            if (Definition->BreakMaterials[I]) Geometry->SetMaterial(Definition->BreakMaterialIndices[I],Definition->BreakMaterials[I]);
    }
    if (bCleared)
    {
        StopCollisionAudio();
        StopDebrisTracking();
        Geometry->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Geometry->DestroyPhysicsState();Geometry->SetComponentTickEnabled(false);
    }
}
void ARetakeDestructibleActor::MulticastImpact_Implementation(FVector_NetQuantize100 Point,FVector_NetQuantizeNormal Normal,uint8 PresentationFlags)
{
    if (PresentationFlags&(RetakeDestructionPresentation::Impact|RetakeDestructionPresentation::BreakSound|RetakeDestructionPresentation::BreakEffect))
    {
        if (GetNetMode()==NM_Client) BeginDebrisSettling();
        OpenCollisionAudioWindow();
    }
    if (auto* System=GetWorld()->GetSubsystem<URetakeDestructionSubsystem>()) System->Present(Definition,Point,PresentationFlags,Normal);
    if (Placement && GetNetMode()!=NM_DedicatedServer)
    {
        if (PresentationFlags&RetakeDestructionPresentation::Impact) Placement->OnDestructibleImpact(Point,Normal);
        if (PresentationFlags&(RetakeDestructionPresentation::BreakSound|RetakeDestructionPresentation::BreakEffect)) Placement->OnDestructibleBreak(Point);
    }
}
void ARetakeDestructibleActor::OpenCollisionAudioWindow()
{
    if (!bConfigured || bCleared || !Definition->CollisionSound || Definition->CollisionSoundWindowSeconds<=0
        || Definition->CollisionSoundVolume<=0 || GetNetMode()==NM_DedicatedServer) return;
    if (Definition->bUseAuthoredResponse) {CollisionAudioEndTime=DBL_MAX;return;}
    CollisionAudioEndTime=GetWorld()->GetTimeSeconds()+Definition->CollisionSoundWindowSeconds;
    GetWorldTimerManager().SetTimer(CollisionAudioWindowTimer,this,&ThisClass::StopCollisionAudio,Definition->CollisionSoundWindowSeconds,false);
}
void ARetakeDestructibleActor::StopCollisionAudio()
{
    CollisionAudioEndTime=0;
    GetWorldTimerManager().ClearTimer(CollisionAudioWindowTimer);
    for (auto& Timer:CollisionVoiceTimers) GetWorldTimerManager().ClearTimer(Timer);
    for (UAudioComponent* Voice:CollisionVoices) if (Voice) Voice->Stop();
}
void ARetakeDestructibleActor::FadeCollisionAudio(int32 VoiceIndex)
{
    if (CollisionVoices.IsValidIndex(VoiceIndex) && CollisionVoices[VoiceIndex] && CollisionVoices[VoiceIndex]->IsPlaying())
        CollisionVoices[VoiceIndex]->FadeOut(CollisionAudioFadeSeconds,0);
}
void ARetakeDestructibleActor::BeginDebrisSettling()
{
    if (!bConfigured || bCleared || !Geometry->HasValidPhysicsState()) return;
    if (Definition->bUseAuthoredResponse) {bTrackingDebris=true;return;}
    LastDebrisMotionTime=GetWorld()->GetTimeSeconds();
    DebrisTrackingEndTime=LastDebrisMotionTime+Definition->MaximumDebrisSeconds;
    bTrackingDebris=true; // Contact notifications stay registered; toggling them rebuilds every particle filter.
    GetWorldTimerManager().SetTimer(DebrisSleepTimer,this,&ThisClass::SettleQuietDebris,
        FMath::Min(Definition->DebrisQuietSeconds,Definition->MaximumDebrisSeconds),false);
}
void ARetakeDestructibleActor::StopDebrisTracking()
{
    GetWorldTimerManager().ClearTimer(DebrisSleepTimer);
    bTrackingDebris=false;
}
void ARetakeDestructibleActor::SettleQuietDebris()
{
    const double Now=GetWorld()->GetTimeSeconds();
    const double Remaining=(Definition->bUseAuthoredResponse ? LastDebrisMotionTime+4:FMath::Min(LastDebrisMotionTime+Definition->DebrisQuietSeconds,DebrisTrackingEndTime))-Now;
    if (!bCleared && Remaining>.01)
    {
        GetWorldTimerManager().SetTimer(DebrisSleepTimer,this,&ThisClass::SettleQuietDebris,Remaining,false);
        return;
    }
    // UE6's physics-object path skips disabled bodies and only sleeps Dynamic/Sleeping bodies.
    // Authored static/kinematic supports remain intact; no field changes their state.
    if (!bCleared && Geometry->HasValidPhysicsState()) Geometry->PutAllRigidBodiesToSleep();
    if (!Definition->bUseAuthoredResponse) {StopDebrisTracking();StopCollisionAudio();}
}
void ARetakeDestructibleActor::HandleCollision(const FChaosPhysicsCollisionInfo& Event)
{
    if (!Definition || bCleared || !bTrackingDebris) return;
    const double Now=GetWorld()->GetTimeSeconds();
    if (Definition->bUseAuthoredResponse && GetNetMode()!=NM_Client && Event.Velocity.SizeSquared()>.25)
    {
        FlushNetDormancy();SetNetDormancy(DORM_Awake);
        if (!GetWorldTimerManager().IsTimerActive(SettleTimer)) GetWorldTimerManager().SetTimer(SettleTimer,this,&ThisClass::RefreshReleased,.05f,true);
    }
    if (Definition->bUseAuthoredResponse && ((Event.Mass<5 && Event.OtherMass<5)
        || (Event.Velocity.SizeSquared()<2500 && Event.OtherVelocity.SizeSquared()<2500)
        || Event.AccumulatedImpulse.SizeSquared()<2500)) return;
    // An opted-in intact prop must remain a solid traversal support. After fracture, the authored yield policy resumes.
    if (Definition->bUseAuthoredResponse && Definition->bRootYieldsToPawnsAfterContact && !bRootYieldsToPawns
        && !(Traversal->bTraversalEnabled && !bFractured)
        && GetNetMode()!=NM_Client && Event.Velocity.SizeSquared()>.25)
    {FlushNetDormancy();bRootYieldsToPawns=true;ApplyRootYield();ForceNetUpdate();}
    if (Event.Velocity.SizeSquared()>FMath::Square(Definition->DebrisSleepSpeed))
    {
        LastDebrisMotionTime=Now;
        if (Definition->bUseAuthoredResponse) GetWorldTimerManager().SetTimer(DebrisSleepTimer,this,&ThisClass::SettleQuietDebris,4,false);
    }
    if (!Definition->CollisionSound || GetNetMode()==NM_DedicatedServer || (!Definition->bUseAuthoredResponse && !Geometry->IsRootBroken())) return;
    const float Remaining=CollisionAudioEndTime-Now;
    if (Remaining<=CollisionAudioFadeSeconds || Now<NextCollisionSound) return;
    if (!Event.OtherComponent || (Event.OtherComponent->GetCollisionObjectType()!=ECC_WorldStatic
        && Event.OtherComponent->GetCollisionObjectType()!=ECC_WorldDynamic)) return;
    const float ImpactSpeed=Event.Velocity.Size();
    if (!FMath::IsFinite(ImpactSpeed) || ImpactSpeed<Definition->CollisionSoundMinimumImpactSpeed
        || Event.AccumulatedImpulse.ContainsNaN() || Event.AccumulatedImpulse.IsNearlyZero()) return;
    int32 VoiceIndex=CollisionVoices.IndexOfByPredicate([](const auto& Voice){return !Voice || !Voice->IsPlaying();});
    if (VoiceIndex==INDEX_NONE)
    {
        if (CollisionVoices.Num()>=Definition->MaximumCollisionVoices) return;
        VoiceIndex=CollisionVoices.Add(nullptr);CollisionVoiceTimers.AddDefaulted();
    }
    auto* System=GetWorld()->GetSubsystem<URetakeDestructionSubsystem>();
    if (!System || !System->AcquireSoundBudget()) return;
    NextCollisionSound=Now+(Definition->bUseAuthoredResponse ? SoundInterval(Definition->CollisionSoundFrequencyHz):FMath::Max(.25,SoundInterval(Definition->CollisionSoundFrequencyHz)));
    UAudioComponent* Voice=CollisionVoices[VoiceIndex];
    if (!Voice)
    {
        Voice=NewObject<UAudioComponent>(this);CollisionVoices[VoiceIndex]=Voice;
        Voice->bAutoActivate=false;Voice->bAutoDestroy=false;Voice->bStopWhenOwnerDestroyed=true;
        Voice->SetupAttachment(GetRootComponent());Voice->RegisterComponent();
    }
    Voice->SetSound(Definition->CollisionSound);
    Voice->AttenuationSettings=System->GetSoundAttenuation(Definition->CollisionSound);
    Voice->ConcurrencySet.Add(System->GetCollisionSoundConcurrency());
    Voice->SetWorldLocation(Event.Location);
    Voice->SetVolumeMultiplier(Definition->CollisionSoundVolume);
    Voice->SetPitchMultiplier(FMath::FRandRange(Definition->CollisionPitch.X,Definition->CollisionPitch.Y));
    Voice->Play();
    if (Definition->bUseAuthoredResponse) return; // Authored one-shots finish naturally.
    const float Duration=FMath::Min(Definition->CollisionSoundMaxDurationSeconds,Remaining);
    GetWorldTimerManager().SetTimer(CollisionVoiceTimers[VoiceIndex],FTimerDelegate::CreateUObject(this,&ThisClass::FadeCollisionAudio,VoiceIndex),
        FMath::Max(.01f,Duration-CollisionAudioFadeSeconds),false);
}
bool ARetakeDestructibleActor::GatherIntactFaces(const FBox& Bounds,TArray<FRetakeSightPolygon>& Out,int32 Limit) const
{
    if (!bConfigured || bCleared || IsHidden() || !Definition->Collection || Definition->PreparedCollectionGuid!=Definition->Collection->GetStateGuid()
        || (!Geometry->IsRootBroken() && !Geometry->GetRootCurrentTransform().Equals(Geometry->GetRootInitialTransform(),.01))
        || (bMaterialsBroken && !Definition->BreakMaterialIndices.IsEmpty())) return false;
    const FTransform Transform=GetActorTransform();
    FRetakeVisibilityCandidates Nearby;Definition->FindOccludersInBounds(Bounds.TransformBy(Transform.ToInverseMatrixWithScale()),Nearby);
    TArray<int32,TInlineAllocator<4>> UsedCovers;
    for (int32 Index:Nearby)
    {
        const auto& Entry=Definition->Occluders[Index];
        if (IsGone(Entry.TransformIndex)) continue;
        if (Definition->PlaneCovers.IsValidIndex(Entry.Cover))
        {
            if (UsedCovers.Contains(Entry.Cover)) continue;
            UsedCovers.Add(Entry.Cover);
            const auto& Face=Definition->PlaneCovers[Entry.Cover];
            TArray<FRetakeSightPolygon> Pieces;
            if (!Face.Transforms.ContainsByPredicate([&](int32 T){return IsGone(T);})) Pieces.Add(Face.Polygon);
            else Face.AppendIntactStrips([this](int32 T){return IsGone(T);},Pieces);
            for (auto& Piece:Pieces)
            {
                if (Out.Num()>=Limit) return false;
                for (FVector& V:Piece.Vertices) V=Transform.TransformPosition(V);
                Piece.Normal=Transform.TransformVectorNoScale(Piece.Normal).GetSafeNormal();
                Out.Add(MoveTemp(Piece));
            }
            continue;
        }
        if (Out.Num()>=Limit) return false;
        FRetakeSightPolygon& World=Out.Add_GetRef(Entry.Polygon);
        for (FVector& V:World.Vertices) V=Transform.TransformPosition(V);
        World.Normal=Transform.TransformVectorNoScale(Entry.Polygon.Normal).GetSafeNormal();
    }
    return true;
}

bool ARetakeDestructibleActor::Occludes(const FBox& Eye,const FBox& Subject,FString* OutReason) const
{
    auto Reject=[&](const TCHAR* Reason){ if (OutReason) *OutReason=Reason; return false; };
    if (!bConfigured) return Reject(TEXT("not_configured"));
    if (bCleared) return Reject(TEXT("cleared"));
    if (IsHidden()) return Reject(TEXT("hidden"));
    if (!Definition->Collection || Definition->PreparedCollectionGuid!=Definition->Collection->GetStateGuid()) return Reject(TEXT("stale_preparation"));
    if (!Geometry->IsRootBroken() && !Geometry->GetRootCurrentTransform().Equals(Geometry->GetRootInitialTransform(),.01)) return Reject(TEXT("root_moved"));
    // A material swap may make surviving geometry transparent or deform it.
    if (bMaterialsBroken && !Definition->BreakMaterialIndices.IsEmpty()) return Reject(TEXT("break_materials"));
    if (!FMath::LineBoxIntersection(Geometry->Bounds.GetBox(),Eye.GetCenter(),Subject.GetCenter(),Subject.GetCenter()-Eye.GetCenter())) return Reject(TEXT("centre_line_misses_bounds"));
    int32 DebugCandidates=0,DebugReleased=0,DebugSeparatedPlanes=0,DebugLastCovers=0;
    ON_SCOPE_EXIT
    {
        if (OutReason && OutReason->IsEmpty())
            *OutReason=FString::Printf(TEXT("not_covered candidates=%d released_candidates=%d separated_planes=%d last_covers=%d"),DebugCandidates,DebugReleased,DebugSeparatedPlanes,DebugLastCovers);
    };
    const FBox LocalEye=Eye.TransformBy(GetActorTransform().ToInverseMatrixWithScale());
    const FBox LocalSubject=Subject.TransformBy(GetActorTransform().ToInverseMatrixWithScale());
    FVector A[8],B[8];RetakeVisibilityGeometry::Corners(LocalEye,A);RetakeVisibilityGeometry::Corners(LocalSubject,B);
    using namespace RetakeVisibilityCoverage;
    int32 Work=8192,Planes=0;
    TArray<FVector4> SeenPlanes;
    TArray<bool,TInlineAllocator<8>> CoverIntact;CoverIntact.SetNumUninitialized(Definition->PlaneCovers.Num());
    for (int32 C=0;C<Definition->PlaneCovers.Num();++C)
    {
        CoverIntact[C]=!Definition->PlaneCovers[C].Transforms.ContainsByPredicate([&](int32 T){return IsGone(T);});
        if (CoverIntact[C] && RetakeVisibilityGeometry::Occludes(Definition->PlaneCovers[C].Polygon,LocalEye,LocalSubject))
        { if (OutReason) *OutReason=TEXT("covered_by_intact_face"); return true; }
    }
    FRetakeVisibilityCandidates Candidates;Definition->FindOccluders(LocalEye.GetCenter(),LocalSubject.GetCenter(),Candidates);
    DebugCandidates=Candidates.Num();
    for (int32 Candidate:Candidates)
    {
        const auto& Entry=Definition->Occluders[Candidate];
        if (IsGone(Entry.TransformIndex)) { ++DebugReleased; continue; }
        const auto& P=Entry.Polygon;
        if (RetakeVisibilityGeometry::Occludes(P,LocalEye,LocalSubject)) { if (OutReason) *OutReason=TEXT("covered_by_one_face"); return true; }
        double Distance;
        if (!RetakeVisibilityGeometry::SegmentPolygon(LocalEye.GetCenter(),LocalSubject.GetCenter(),P,Distance)) continue;
        const double Offset=FVector::DotProduct(P.Normal,P.Vertices[0]);
        const FVector4 Plane(P.Normal,Offset);
        if (SeenPlanes.ContainsByPredicate([&](const FVector4& V){return V.Equals(Plane,1.e-2);})) continue;
        if (++Planes>8) return Reject(TEXT("plane_budget"));
        SeenPlanes.Add(Plane);
        double DA[8],DB[8];bool Separated=true;
        const bool Front=FVector::DotProduct(A[0],P.Normal)>Offset;
        if (!Front && !P.bTwoSided) continue;
        for (int32 I=0;I<8;++I)
        {
            DA[I]=FVector::DotProduct(A[I],P.Normal)-Offset;DB[I]=FVector::DotProduct(B[I],P.Normal)-Offset;
            if (Front ? DA[I]<=.1 || DB[I]>=-.1 : DA[I]>=-.1 || DB[I]<=.1) Separated=false;
        }
        if (!Separated) continue;
        ++DebugSeparatedPlanes;
        FVector X,Y;P.Normal.FindBestAxisVectors(X,Y);
        auto Project=[&](const FVector& V){return FVector2D(FVector::DotProduct(V,X),FVector::DotProduct(V,Y));};
        TArray<FVector2D> CrossSection;
        FBox CrossBounds(ForceInit);
        for (int32 I=0;I<8;++I) for (int32 J=0;J<8;++J)
        {
            const FVector V=FMath::Lerp(A[I],B[J],DA[I]/(DA[I]-DB[J]));CrossSection.Add(Project(V));CrossBounds+=V;
        }
        FPolygon Required;if (!Hull(CrossSection,Required)) continue;
        const FBox2D RequiredBounds=Bounds(Required);
        TArray<FPolygon> Covers;
        TArray<int32,TInlineAllocator<4>> UsedCovers;
        FRetakeVisibilityCandidates Nearby;Definition->FindOccludersInBounds(CrossBounds.ExpandBy(.1),Nearby);
        for (int32 OtherIndex:Nearby)
        {
            const auto& Other=Definition->Occluders[OtherIndex];
            if (IsGone(Other.TransformIndex) || !Other.Polygon.Normal.Equals(P.Normal,1.e-4)) continue;
            // A tiled face stands in for all of its piece faces: whole while intact, its intact grid strips once damaged.
            if (CoverIntact.IsValidIndex(Other.Cover))
            {
                if (UsedCovers.Contains(Other.Cover)) continue;
                UsedCovers.Add(Other.Cover);
                const auto& Face=Definition->PlaneCovers[Other.Cover];
                if (Face.Polygon.Vertices.ContainsByPredicate([&](const FVector& W){return FMath::Abs(FVector::DotProduct(W,P.Normal)-Offset)>1.e-2;})) continue;
                TArray<FRetakeSightPolygon> Pieces;
                if (CoverIntact[Other.Cover]) Pieces.Add(Face.Polygon);
                else Face.AppendIntactStrips([this](int32 T){return IsGone(T);},Pieces);
                for (const auto& Piece:Pieces)
                {
                    TArray<FVector2D> Points;for (const FVector& W:Piece.Vertices) Points.Add(Project(W));
                    FPolygon Cover;if (Hull(Points,Cover) && Bounds(Cover).Intersect(RequiredBounds)) Covers.Add(MoveTemp(Cover));
                }
                continue;
            }
            if (!Other.Polygon.Vertices.ContainsByPredicate([&](const FVector& V){return FMath::Abs(FVector::DotProduct(V,P.Normal)-Offset)>1.e-2;}))
            {
                TArray<FVector2D> Points;for (const FVector& V:Other.Polygon.Vertices) Points.Add(Project(V));
                FPolygon Cover;if (Hull(Points,Cover) && Bounds(Cover).Intersect(RequiredBounds)) Covers.Add(MoveTemp(Cover));
            }
            if (Covers.Num()>64) break;
        }
        DebugLastCovers=Covers.Num();
        if (RetakeVisibilityCoverage::Covers(Required,Covers,Work)) { if (OutReason) *OutReason=FString::Printf(TEXT("covered covers=%d"),Covers.Num()); return true; }
        if (Work<0) return Reject(TEXT("coverage_work_budget"));
    }
    return false;
}
