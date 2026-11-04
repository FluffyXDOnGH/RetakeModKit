// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeDestructionSubsystem.h"
#include "RetakeDestructiblePlacement.h"
#include "RetakeDestructibleActor.h"
#include "RetakeDestructionDefinition.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundConcurrency.h"
#include "ProfilingDebugging/CountersTrace.h"

TRACE_DECLARE_INT_COUNTER(RetakeDestructionActors,TEXT("Retake/Destruction/Collections"));
TRACE_DECLARE_INT_COUNTER(RetakeDestructionSuppressed,TEXT("Retake/Destruction/Suppressed cosmetics"));
bool URetakeDestructionSubsystem::DoesSupportWorldType(EWorldType::Type Type) const
{ return Type==EWorldType::Game || Type==EWorldType::PIE; }
void URetakeDestructionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    if (auto* Elements=Collection.InitializeDependency<URetakeMapElementSubsystem>())
        Observation=Elements->Observe<ARetakeDestructiblePlacement>(*this,
            [this](ARetakeDestructiblePlacement& P){AddPlacement(P);},
            [this](ARetakeDestructiblePlacement& P){RemovePlacement(P);});
}
void URetakeDestructionSubsystem::Deinitialize()
{
    if (auto* Elements=URetakeMapElementSubsystem::Get(GetWorld())) Elements->Unobserve(Observation);
    OnDebrisSurfaceImpact.Clear();OnStructureChanged.Clear();
    Placements.Reset();Actors.Reset();ActiveEffects.Reset();WorldEffects.Reset();Super::Deinitialize();
}
void URetakeDestructionSubsystem::AddPlacement(ARetakeDestructiblePlacement& P)
{
    // Non-replicated map actors can have ROLE_Authority on a client as well.
    if (GetWorld()->GetNetMode()!=NM_Client && P.HasAuthority() && !Placements.Contains(&P)) {Placements.Add(&P);Spawn(&P);}
}
void URetakeDestructionSubsystem::RemovePlacement(ARetakeDestructiblePlacement& P)
{ if (auto* Found=Placements.Find(&P)) {if (auto* A=Found->Get()) A->Destroy();Placements.Remove(&P);} }
void URetakeDestructionSubsystem::Spawn(ARetakeDestructiblePlacement* P)
{
    if (GetWorld()->GetNetMode()==NM_Client || !IsValid(P) || !P->Definition || !P->Definition->Collection) return;
    auto* A=GetWorld()->SpawnActorDeferred<ARetakeDestructibleActor>(ARetakeDestructibleActor::StaticClass(),P->GetActorTransform(),nullptr,nullptr,ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
    if (!A) return;
    A->SetPlacement(P); A->Initialize(P->Definition,Generation); A->FinishSpawning(P->GetActorTransform()); Placements.FindOrAdd(P)=A;
}
void URetakeDestructionSubsystem::RegisterActor(ARetakeDestructibleActor* A)
{Actors.Add(A);TRACE_COUNTER_SET(RetakeDestructionActors,Actors.Num());NotifyStructureChanged();}
void URetakeDestructionSubsystem::UnregisterActor(ARetakeDestructibleActor* A)
{Actors.Remove(A);TRACE_COUNTER_SET(RetakeDestructionActors,Actors.Num());NotifyStructureChanged();}
void URetakeDestructionSubsystem::ResetForRound()
{
    if (GetWorld()->GetNetMode()==NM_Client) return;
    TRACE_CPUPROFILER_EVENT_SCOPE(RetakeDestruction_Reset);
    ++Generation; if (!Generation) ++Generation;
    // A new actor rebuilds its collection's physics state and connectivity graph and replicates to every client: replacing every
    // collection cost 110-150 ms of server frames on the obj maps (alpha.30). Only changed ones need it. An untouched one is
    // already what a new actor would be on every machine, so it is kept and joins the new generation.
    int32 Kept=0,Replaced=0;
    TArray<TWeakObjectPtr<ARetakeDestructiblePlacement>> Keys;Placements.GetKeys(Keys);
    for (const auto& P:Keys)
    {
        auto* A=Placements.FindRef(P).Get();
        if (A && P.IsValid() && A->CanKeepForRound()) {A->ContinueInGeneration(Generation);++Kept;continue;}
        if (A) A->Destroy();
        if (P.IsValid()) {Spawn(P.Get());++Replaced;} else Placements.Remove(P);
    }
    UE_LOGF(LogRetakeDestruction,Log,"event=DestructionReset generation=%d kept=%d replaced=%d",(int32)Generation,Kept,Replaced);
    NotifyStructureChanged();
}
namespace
{
    /** The hit struck on A itself, using A's definition for the field. */
    FRetakeDestructionHit DirectHit(ARetakeDestructibleActor* A,const FHitResult& Hit,const FVector& Direction,float Damage)
    {
        FRetakeDestructionHit Direct{A,Hit.ImpactPoint,Direction.GetSafeNormal(),Damage,0,A->GetGeneration(),Hit.Item,Hit.Normal};
        Direct.FieldDefinition=A->GetDefinition();
        return Direct;
    }
}
void URetakeDestructionSubsystem::GatherPoint(const FHitResult& Hit,const FVector& Direction,float Damage,TArray<FRetakeDestructionHit>& Out,float StrainScale) const
{
    auto* A=Cast<ARetakeDestructibleActor>(Hit.GetActor());
    if (!A || A->IsCleared() || !A->HasAuthority() || !Hit.bBlockingHit || Damage<=0 || !FMath::IsFinite(Damage)) return;
    if (Hit.GetComponent()==A->GetCollectionComponent() && A->IsReleased(Hit.Item)) return;
    auto* Response=A->GetDefinition();
    if (!Response) return;
    if (!Response->bUseAuthoredResponse && Hit.GetComponent()!=A->GetCollectionComponent()) return;
    FRetakeDestructionHit Direct=DirectHit(A,Hit,Direction,Damage);
    Direct.StrainScale=FMath::Clamp(FMath::IsFinite(StrainScale) ? StrainScale:0.f,0.f,1.f);Out.Add(Direct);
    if (!Response->bUseAuthoredResponse) return;
    // The original field was global. Gather neighbouring collections before any fracture,
    // using the struck profile's radius/kernel for every recipient of this one impact.
    const float Reach=Response->PointRadiusCm*1.25f;
    for (const auto& Weak:Actors)
    {
        auto* Nearby=Weak.Get();
        if (!Nearby || Nearby==A || Nearby->IsCleared() || !Nearby->HasAuthority()) continue;
        if (Nearby->GetCollectionComponent()->Bounds.GetBox().ComputeSquaredDistanceToPoint(Hit.ImpactPoint)>FMath::Square(Reach)) continue;
        auto Neighbour=Direct;Neighbour.Actor=Nearby;Neighbour.Generation=Nearby->GetGeneration();Neighbour.ItemIndex=INDEX_NONE;Neighbour.bDirectImpact=false;
        Out.Add(Neighbour);
    }
}
void URetakeDestructionSubsystem::GatherBreak(const FHitResult& Hit,const FVector& Direction,TArray<FRetakeDestructionHit>& Out) const
{
    auto* A=Cast<ARetakeDestructibleActor>(Hit.GetActor());
    if (!A || !A->GetDefinition() || A->IsCleared() || !A->HasAuthority() || !Hit.bBlockingHit || Hit.GetComponent()!=A->GetCollectionComponent()) return;
    // The opening comes from BreakRadius, not damage; the nominal damage only satisfies the attack's commit limits.
    FRetakeDestructionHit Break=DirectHit(A,Hit,Direction,1);
    Break.BreakRadius=A->GetDefinition()->GrenadeBreakRadiusCm;
    Out.Add(Break);
}
void URetakeDestructionSubsystem::GatherCollectionBreak(const FHitResult& Hit,const FVector& Direction,TArray<FRetakeDestructionHit>& Out) const
{
    auto* Actor=Cast<ARetakeDestructibleActor>(Hit.GetActor());
    if (!Actor || !Actor->GetDefinition() || Actor->IsCleared() || !Actor->HasAuthority() || !Hit.bBlockingHit
        || Hit.bStartPenetrating || Hit.GetComponent()!=Actor->GetCollectionComponent() || Actor->IsReleased(Hit.Item)) return;
    const FBox Bounds=Actor->GetCollectionComponent()->Bounds.GetBox();
    const double Radius=FVector::Distance(Hit.ImpactPoint,Bounds.GetCenter())+Bounds.GetExtent().Size()+1.;
    if (!FMath::IsFinite(Radius) || Radius<=0 || Radius>1000000) return;
    FRetakeDestructionHit Break=DirectHit(Actor,Hit,Direction,1);
    Break.BreakRadius=float(Radius);Break.bShatterCollection=true;Out.Add(Break);
}

void URetakeDestructionSubsystem::GatherDebris(const FVector& Start,const FVector& End,const FVector& Direction,float Damage,const AActor* Shooter,TArray<FRetakeDestructionHit>& Out) const
{
    if (GetWorld()->GetNetMode()==NM_Client || Damage<=0 || !FMath::IsFinite(Damage) || Start.ContainsNaN() || End.ContainsNaN()) return;
    bool bAnyDebris=false;
    for (const auto& Weak:Actors) if (const auto* A=Weak.Get(); A && A->HasLooseDebris() && A->GetDefinition()->bShotsKickDebris) {bAnyDebris=true;break;}
    if (!bAnyDebris) return;
    TRACE_CPUPROFILER_EVENT_SCOPE(RetakeDestruction_GatherDebris);
    // Loose shards never enter the query scene, so they cannot block a shot or diverge gameplay
    // traces. Like the reference bullet, only the first shard on the path reacts.
    FRetakeDestructionHit Nearest;double NearestDistance=TNumericLimits<double>::Max();
    for (const auto& Weak:Actors)
    {
        auto* A=Weak.Get();
        if (!A || A==Shooter || !A->HasAuthority() || !A->GetDefinition() || !A->GetDefinition()->bUseAuthoredResponse || !A->GetDefinition()->bShotsKickDebris) continue;
        int32 Item;FVector Point,Normal;double Distance;
        if (!A->TraceLooseDebris(Start,End,Item,Point,Normal,Distance) || Distance>=NearestDistance) continue;
        NearestDistance=Distance;
        Nearest={A,Point,Direction.GetSafeNormal(),Damage,0,A->GetGeneration(),Item,Normal};
        Nearest.FieldDefinition=A->GetDefinition();Nearest.bDirectImpact=false;Nearest.bDebrisImpact=true;
    }
    if (Nearest.Actor.IsValid()) Out.Add(Nearest);
}
void URetakeDestructionSubsystem::GatherExplosion(const FVector& Origin,float Damage,float Inner,float Outer,ECollisionChannel CoverChannel,const AActor* Causer,
    TArray<FRetakeDestructionHit>& Out,float FragileGrenadeDamage,uint8 QueryIgnoreMask) const
{
    if (GetWorld()->GetNetMode()==NM_Client || !FMath::IsFinite(Outer) || Outer<=0 || Damage<=0 || Origin.ContainsNaN()
        || !FMath::IsFinite(FragileGrenadeDamage) || FragileGrenadeDamage<0) return;
    TRACE_CPUPROFILER_EVENT_SCOPE(RetakeDestruction_GatherExplosion);
    FCollisionQueryParams Query(SCENE_QUERY_STAT(RetakeDestructionExplosion),false,Causer);
    Query.IgnoreMask=QueryIgnoreMask;
    for (TActorIterator<APawn> It(GetWorld());It;++It) Query.AddIgnoredActor(*It);
    for (const auto& Weak:Actors)
    {
        auto* A=Weak.Get(); if (!A || A->IsCleared() || !A->HasAuthority()) continue;
        if (FragileGrenadeDamage>0 && !A->GetDefinition()->IsBrokenByGrenade(FragileGrenadeDamage)) continue;
        const FBox Bounds=A->GetCollectionComponent()->Bounds.GetBox();
        const FVector Point=Bounds.GetClosestPointTo(Origin);
        const float Radius=A->GetDefinition()->bUseAuthoredResponse ? 200.f:Outer;
        const float Distance=FVector::Distance(Origin,Point); if (Distance>Radius*1.25f) continue;
        FHitResult Cover;
        if (!A->GetDefinition()->bUseAuthoredResponse && GetWorld()->LineTraceSingleByChannel(Cover,Origin,Bounds.GetCenter(),CoverChannel,Query) && Cover.GetActor()!=A) continue;
        // The native strain field applies distance falloff at each fragment, once.
        // Applying another actor-bounds falloff here would attenuate destruction twice.
        Out.Add({A,Origin,(Bounds.GetCenter()-Origin).GetSafeNormal(),Damage,Radius,A->GetGeneration()});
    }
}
void URetakeDestructionSubsystem::Commit(TConstArrayView<FRetakeDestructionHit> Hits,float DamageLimit,bool bSplash)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(RetakeDestruction_Commit);
    if (!FMath::IsFinite(DamageLimit) || DamageLimit<=0) return;
    for (const auto& Hit:Hits)
    {
        auto* A=Hit.Actor.Get();
        if (!A || A->GetGeneration()!=Generation || A->GetGeneration()!=Hit.Generation || Hit.Point.ContainsNaN() || Hit.Direction.ContainsNaN() || Hit.SurfaceNormal.ContainsNaN()
            || !FMath::IsFinite(Hit.Damage) || !FMath::IsFinite(Hit.Radius) || !FMath::IsFinite(Hit.BreakRadius) || Hit.Damage<=0 || Hit.Radius<0 || Hit.BreakRadius<0)
        {
            if (RetakeDestructionDebugEnabled())
                UE_LOGF(LogRetakeDestruction,Log,"event=DestructionHitSkipped actor=%ls actor_generation=%d world_generation=%d hit_generation=%d damage=%.2f radius=%.1f",
                    *GetNameSafe(A),A ? (int32)A->GetGeneration():-1,(int32)Generation,(int32)Hit.Generation,Hit.Damage,Hit.Radius);
            continue;
        }
        // The reference applied a fixed strain to every bullet regardless of weapon. Authored
        // strain is scaled only by penetration attenuation, never by damage relative to the
        // headshot/pellet ceiling, so every weapon fractures the same way.
        const float Fraction=bSplash ? FMath::Min(1.f,Hit.Damage/DamageLimit):Hit.StrainScale;
        A->ApplyDamageAt(Hit,FMath::Min(DamageLimit,Hit.Damage),Fraction);
    }
}
bool URetakeDestructionSubsystem::Occludes(const FBox& Eye,const FBox& Subject) const
{
    TRACE_CPUPROFILER_EVENT_SCOPE(RetakeDestruction_Visibility);
    for (const auto& Weak:Actors) if (auto* A=Weak.Get(); A && A->HasAuthority() && A->Occludes(Eye,Subject)) return true;
    return false;
}
void URetakeDestructionSubsystem::DescribeOcclusion(const FBox& Eye,const FBox& Subject,TArray<FString>& Out) const
{
    for (const auto& Weak:Actors)
    {
        const auto* A=Weak.Get();
        if (!A || !A->HasAuthority() || !A->GetCollectionComponent() || !A->GetCollectionComponent()->Bounds.GetBox().Intersect(Eye+Subject)) continue;
        FString Reason; const bool Hidden=A->Occludes(Eye,Subject,&Reason);
        Out.Add(FString::Printf(TEXT("actor=%ls definition=%ls occludes=%d reason=%ls %ls"),*A->GetName(),*GetNameSafe(A->GetDefinition()),Hidden,*Reason,*A->DescribeState()));
    }
}
void URetakeDestructionSubsystem::NotifyStructureChanged()
{ OnStructureChanged.Broadcast(); }
void URetakeDestructionSubsystem::RefreshBudget()
{
    if (BudgetFrame==GFrameCounter) return;
    BudgetFrame=GFrameCounter;EffectsThisFrame=SoundsThisFrame=0;
    const double Now=GetWorld()->GetTimeSeconds();
    ActiveEffects.RemoveAll([Now](const FTrackedEffect& Tracked)
    {
        UNiagaraComponent* Effect=Tracked.Effect.Get();
        if (!Effect || !Effect->IsActive()) return true;
        if (Now<Tracked.StopAt) return false;
        Effect->DeactivateImmediate();
        return true;
    });
}
bool URetakeDestructionSubsystem::AcquireEffectBudget()
{RefreshBudget();if (EffectsThisFrame>=8 || ActiveEffects.Num()>=32) {TRACE_COUNTER_INCREMENT(RetakeDestructionSuppressed);return false;}++EffectsThisFrame;return true;}
void URetakeDestructionSubsystem::TrackEffect(UNiagaraComponent* Effect)
{ if (Effect) ActiveEffects.Add({Effect,GetWorld()->GetTimeSeconds()+MaxEffectSeconds}); }
bool URetakeDestructionSubsystem::AcquireSoundBudget()
{RefreshBudget();if (SoundsThisFrame>=8) {TRACE_COUNTER_INCREMENT(RetakeDestructionSuppressed);return false;}++SoundsThisFrame;return true;}
USoundAttenuation* URetakeDestructionSubsystem::GetSoundAttenuation(const USoundBase* Sound)
{
    if (!Sound || Sound->GetAttenuationSettingsToApply() || GetWorld()->GetNetMode()==NM_DedicatedServer) return nullptr;
    if (!DefaultSoundAttenuation)
    {
        DefaultSoundAttenuation=NewObject<USoundAttenuation>(this);
        auto& Settings=DefaultSoundAttenuation->Attenuation;
        Settings.bAttenuate=true;Settings.bSpatialize=true;
        Settings.DistanceAlgorithm=EAttenuationDistanceModel::Logarithmic;
        Settings.AttenuationShapeExtents=FVector(100,0,0);Settings.FalloffDistance=1800;
    }
    return DefaultSoundAttenuation;
}
USoundConcurrency* URetakeDestructionSubsystem::GetCollisionSoundConcurrency()
{
    if (GetWorld()->GetNetMode()==NM_DedicatedServer) return nullptr;
    if (!CollisionSoundConcurrency)
    {
        CollisionSoundConcurrency=NewObject<USoundConcurrency>(this);
        auto& Settings=CollisionSoundConcurrency->Concurrency;
        Settings.MaxCount=32;Settings.bLimitToOwner=false;
        Settings.ResolutionRule=EMaxConcurrentResolutionRule::StopQuietest;Settings.VoiceStealReleaseTime=.1f;
    }
    return CollisionSoundConcurrency;
}
void URetakeDestructionSubsystem::Present(URetakeDestructionDefinition* D,const FVector& Point,uint8 Flags,const FVector& Normal)
{
    if (!D || GetWorld()->GetNetMode()==NM_DedicatedServer) return;
    // Shots through loose shards: the reference bullet hit presentation on the shard itself.
    if ((Flags&RetakeDestructionPresentation::DebrisImpact) && !D->bSilentSurfaceImpacts) OnDebrisSurfaceImpact.Broadcast(*D,Point,Normal);
    if ((Flags&RetakeDestructionPresentation::BreakEffect) && D->BreakEffect && AcquireEffectBudget())
        TrackEffect(UNiagaraFunctionLibrary::SpawnSystemAtLocation(GetWorld(),D->BreakEffect,Point,FRotator(0,FMath::FRandRange(0.f,359.f),0),FVector::OneVector,true,true,ENCPoolMethod::AutoRelease));
    USoundBase* Sound=(Flags&RetakeDestructionPresentation::BreakSound) ? D->BreakSound.Get():((!D->bUseAuthoredResponse && (Flags&RetakeDestructionPresentation::Impact)) ? D->ImpactSound.Get():nullptr);
    // A sound without its own attenuation gets the default falloff in every mode; unattenuated it would play across the whole map.
    if (Sound && AcquireSoundBudget()) UGameplayStatics::PlaySoundAtLocation(GetWorld(),Sound,Point,D->BreakSoundVolume,
        FMath::FRandRange(D->BreakPitch.X,D->BreakPitch.Y),0,GetSoundAttenuation(Sound));
}
const URetakeDestructionDefinition* URetakeDestructionSubsystem::FindDefinition(const FHitResult& Hit)
{
    const auto* A=Cast<ARetakeDestructibleActor>(Hit.GetActor());
    return A ? A->GetDefinition():nullptr;
}
void URetakeDestructionSubsystem::EnsureWorldEffects(const URetakeDestructionDefinition* D)
{
    if (!D || GetWorld()->GetNetMode()==NM_DedicatedServer) return;
    for (UNiagaraSystem* System:D->WorldChaosEffects)
    {
        if (!System) continue;
        if (const auto* Existing=WorldEffects.Find(System); Existing && IsValid(*Existing)) continue;
        // The reference placed one Chaos data-interface system per material in the level.
        if (auto* FX=UNiagaraFunctionLibrary::SpawnSystemAtLocation(GetWorld(),System,FVector::ZeroVector,FRotator::ZeroRotator,FVector::OneVector,false,true,ENCPoolMethod::None))
            WorldEffects.Add(System,FX);
    }
}
