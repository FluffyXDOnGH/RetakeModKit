// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Engine/EngineTypes.h"
#include "Engine/HitResult.h"
#include "RetakeMapElementSubsystem.h"
#include "RetakeDestructionSubsystem.generated.h"

class ARetakeDestructiblePlacement;
class ARetakeDestructibleActor;
class URetakeDestructionDefinition;
class UNiagaraComponent;
class UNiagaraSystem;
class USoundBase;
class USoundAttenuation;
class USoundConcurrency;

/** Native attack-local work, never populated from an RPC. All queries precede any mutation. */
struct FRetakeDestructionHit
{
    TWeakObjectPtr<ARetakeDestructibleActor> Actor;
    FVector Point=FVector::ZeroVector,Direction=FVector::UpVector;
    float Damage=0,Radius=0;
    uint32 Generation=0;
    int32 ItemIndex=INDEX_NONE;
    FVector SurfaceNormal=FVector::ZeroVector;
    TWeakObjectPtr<URetakeDestructionDefinition> FieldDefinition;
    bool bDirectImpact=true;
    /** Penetration attenuation of the source's fixed per-hit strain. Weapon damage does not scale authored strain. */
    float StrainScale=1;
    bool bDebrisImpact=false;
    /** >0: guaranteed break of everything within this radius (a grenade passing through). */
    float BreakRadius=0;
    /** Admitted whole-collection break, independent of authored strain thresholds. */
    bool bShatterCollection=false;
};

/**
 * The destructibles of one world. The server spawns one runtime actor per placement the map element registry reports and
 * applies the hits the game has admitted (Commit); clients present the definitions' cosmetics under shared budgets. The game
 * calls in and reacts through OnDebrisSurfaceImpact and OnStructureChanged; nothing here calls the game.
 */
UCLASS()
class RETAKEDESTRUCTION_API URetakeDestructionSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    DECLARE_MULTICAST_DELEGATE_ThreeParams(FDebrisSurfaceImpact,const URetakeDestructionDefinition& /*Definition*/,const FVector& /*Point*/,const FVector& /*Normal*/);
    virtual bool DoesSupportWorldType(EWorldType::Type Type) const override;
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    void RegisterActor(ARetakeDestructibleActor* Actor);
    void UnregisterActor(ARetakeDestructibleActor* Actor);
    /**
     * Server: every placement starts the next generation as authored. A runtime actor that changed is replaced by a new one (new
     * replication identity); an untouched one (ARetakeDestructibleActor::CanKeepForRound) is kept and joins the generation.
     */
    void ResetForRound();
    bool HasDestructibles() const { return !Actors.IsEmpty(); }
    void GatherPoint(const FHitResult& Hit,const FVector& Direction,float Damage,TArray<FRetakeDestructionHit>& Out,float StrainScale=1) const;
    /** A grenade broke through at Hit: guarantees the opening, whatever the definition's thresholds. */
    void GatherBreak(const FHitResult& Hit,const FVector& Direction,TArray<FRetakeDestructionHit>& Out) const;
    /** Native admitted shatter: break the entire struck collection, preserving a separately authored frame. */
    void GatherCollectionBreak(const FHitResult& Hit,const FVector& Direction,TArray<FRetakeDestructionHit>& Out) const;
    /** Loose authored shards along a shot segment receive the source per-hit field. They never block the shot. */
    void GatherDebris(const FVector& Start,const FVector& End,const FVector& Direction,float Damage,const AActor* Shooter,TArray<FRetakeDestructionHit>& Out) const;
    /**
     * CoverChannel: a blocking trace on it from Origin to a destructible's centre shields that destructible.
     * FragileGrenadeDamage above zero limits the blast to destructibles a grenade of that contact damage breaks through
     * (URetakeDestructionDefinition::IsBrokenByGrenade), such as a smoke or flash grenade's activation.
     */
    void GatherExplosion(const FVector& Origin,float Damage,float InnerRadius,float OuterRadius,ECollisionChannel CoverChannel,const AActor* Causer,
        TArray<FRetakeDestructionHit>& Out,float FragileGrenadeDamage=0,uint8 QueryIgnoreMask=0) const;
    /**
     * Server: applies gathered hits of an attack the caller has admitted. DamageLimit caps each hit (the attack's direct or splash
     * limit) and scales splash strain. Hits on another round's actors, non-finite hits and a non-positive limit are skipped.
     */
    void Commit(TConstArrayView<FRetakeDestructionHit> Hits,float DamageLimit,bool bSplash=false);
    bool Occludes(const FBox& Eye,const FBox& Subject) const;
    /** Diagnostics: one line per destructible whose bounds meet the beam: its occlusion verdict/reason and piece state. */
    void DescribeOcclusion(const FBox& Eye,const FBox& Subject,TArray<FString>& Out) const;
    void NotifyStructureChanged();
    void Present(URetakeDestructionDefinition* Definition,const FVector& Point,uint8 PresentationFlags,const FVector& Normal=FVector::ZeroVector);
    /** The definition of the destructible Hit struck; null for anything else. */
    static const URetakeDestructionDefinition* FindDefinition(const FHitResult& Hit);
    /** Client-only persistent level effects (Chaos break/trailing data interface systems). */
    void EnsureWorldEffects(const URetakeDestructionDefinition* Definition);
    /** Client effect budget shared by every destructible cosmetic: 8 new effects per frame, 32 alive. Spawn only after it grants one,
     *  then TrackEffect. A tracked effect still alive after MaxEffectSeconds is stopped, so no effect holds a slot forever. */
    bool AcquireEffectBudget();
    void TrackEffect(UNiagaraComponent* Effect);
    bool AcquireSoundBudget();
    USoundAttenuation* GetSoundAttenuation(const USoundBase* Sound);
    USoundConcurrency* GetCollisionSoundConcurrency();
    /** Client: a shot struck a loose shard of a definition that does not silence surface impacts. */
    FDebrisSurfaceImpact OnDebrisSurfaceImpact;
    /** Concealing or blocking structure changed: a destructible appeared, broke, released pieces, cleared or reset. */
    FSimpleMulticastDelegate OnStructureChanged;
private:
    UPROPERTY(Transient) TObjectPtr<USoundAttenuation> DefaultSoundAttenuation;
    UPROPERTY(Transient) TObjectPtr<USoundConcurrency> CollisionSoundConcurrency;
    UPROPERTY(Transient) TMap<TObjectPtr<UNiagaraSystem>,TObjectPtr<UNiagaraComponent>> WorldEffects;
    TMap<TWeakObjectPtr<ARetakeDestructiblePlacement>,TWeakObjectPtr<ARetakeDestructibleActor>> Placements;
    TSet<TWeakObjectPtr<ARetakeDestructibleActor>> Actors;
    FRetakeMapElementObservation Observation;
    uint32 Generation=1;
    uint64 BudgetFrame=MAX_uint64;
    int32 EffectsThisFrame=0,SoundsThisFrame=0;
    /** How long one effect, a mod's included, may hold a slot. */
    static constexpr double MaxEffectSeconds=30.;
    struct FTrackedEffect { TWeakObjectPtr<UNiagaraComponent> Effect; double StopAt=0.; };
    TArray<FTrackedEffect> ActiveEffects;
    void AddPlacement(ARetakeDestructiblePlacement& Placement);
    void RemovePlacement(ARetakeDestructiblePlacement& Placement);
    void Spawn(ARetakeDestructiblePlacement* Placement);
    void RefreshBudget();
};
