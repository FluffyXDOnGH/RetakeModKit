// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/NetSerialization.h"
#include "RetakeMinefield.generated.h"

class URetakeAudioCueDefinition;
class UNiagaraSystem;
class UBillboardComponent;
class UBoxComponent;
class UInstancedStaticMeshComponent;

/**
 * Minefield (RetakeMapSDK element, MKD-029): MOHAA's global/minefield.scr with visible mines. A living player who touches Field steps
 * on a mine. It clicks, and after a random delay between MinimumDelaySeconds and MaximumDelaySeconds the mine nearest them goes off
 * if their body still touches the field; that mine stays gone until the round resets. With no mine left, the blast comes from the
 * player's feet, as in MOHAA. The blast uses the authored cues/effect with this field's Damage and DamageRadiusCm (MOHAA
 * radiusdamage: classic falloff and explosion cover), centred on the mine; it hurts everyone in reach and is credited to nobody. As
 * in MOHAA, one mine is armed at a time per field, and after a blast the field rests for RearmSeconds before it arms again.
 * The game owns every rule: it observes the element through URetakeMapElementSubsystem, arms and detonates on the server and calls
 * ServerConsumeMine and ServerPresentDetonation, which reach every machine and replays; the element then reports MinesChannel and
 * DetonationChannel. Field is a box with no collision (rotation and scale allowed): containment is computed by OverlapsBox, never an
 * overlap event. The mines are one instanced mesh with no collision, laid out by Regenerate Mines on the floor inside the field.
 * Frozen per content generation: UPROPERTY names, order, types and defaults, the subobject names Field and Mines, the replicated
 * members and the multicast signature.
 */
UCLASS(meta=(DisplayName="Retake Minefield"))
class RETAKEMAPSDK_API ARetakeMinefield : public AActor
{
    GENERATED_BODY()
public:
    ARetakeMinefield();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
#if WITH_EDITOR
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif

    /** The mined area: a player whose collision box touches this box steps on a mine. Size it with Box Extent or scale. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Minefields") TObjectPtr<UBoxComponent> Field;
    /** Shortest wait between the click and the blast (MOHAA: randomfloat .5 + .5). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Minefields", meta=(ClampMin="0", Units="s")) float MinimumDelaySeconds = .5f;
    /** Longest wait between the click and the blast. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Minefields", meta=(ClampMin="0", Units="s")) float MaximumDelaySeconds = 1.f;
    /** After a blast the field arms no mine for this long (MOHAA: wait 3). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Minefields", meta=(ClampMin="0", Units="s")) float RearmSeconds = 3.f;
    /** Blast damage before falloff (MOHAA: radiusdamage 4000). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Minefields", meta=(ClampMin="0")) float Damage = 4000.f;
    /** Blast radius (MOHAA: radiusdamage 256 units, 1.905 cm each). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Minefields", meta=(ClampMin="0", Units="cm")) float DamageRadiusCm = 487.68f;

    /** Spatial one-shot cue when an intruder arms a mine. A mod map owns its cues and recordings. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Minefields|Presentation") TObjectPtr<URetakeAudioCueDefinition> TriggerCue;
    /** Spatial one-shot explosion cue; independent of the enabled weapon packs and loadout. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Minefields|Presentation") TObjectPtr<URetakeAudioCueDefinition> ExplosionCue;
    /** Client-only explosion resource, prepared before admission. Never loaded at detonation. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Minefields|Presentation") TSoftObjectPtr<UNiagaraSystem> ExplosionEffect;

    /** The visible mines: one instance per MineTransforms entry, no collision (players walk over them), hidden while exploded. Set its
     *  Static Mesh; Regenerate Mines writes the instances. Not cover for anyone: tagged Retake.Visibility.Ignore, outside acoustics. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Minefields") TObjectPtr<UInstancedStaticMeshComponent> Mines;
    /** Grid spacing of the mine layout: one mine per cell of at most this size. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Minefields", meta=(ClampMin="25", Units="cm")) float MineSpacingCm = 200.f;
    /** How far a mine may sit from its cell's centre, as a fraction of the cell (0 is a regular grid). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Minefields", meta=(ClampMin="0", ClampMax="1")) float MineJitter = .5f;
    /** Seed of the jitter and of each mine's turn: the same seed and field give the same layout. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Minefields") int32 MineLayoutSeed = 1;
    /** The mines, relative to the actor's location and rotation (unscaled), each on its surface and aligned to it. Written by
     *  Regenerate Mines. */
    UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category="Retake|Minefields") TArray<FTransform> MineTransforms;
    /** The field transform, size and layout settings Regenerate Mines last ran with; validation warns when they no longer match. */
    UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category="Retake|Minefields") int32 MineLayoutHash = 0;

    /**
     * Editor: lays the mines out on a jittered grid over the field. Each mine rests on the first surface below the field's top that is
     * visible in game, opaque (water, foliage and grates are skipped, as are hidden collision and players) and walkable; a cell whose
     * surface is too steep gets no mine. Turns are seeded, so the layout is deterministic. Reports the count and the worst distance
     * from a standable point of the field to its nearest mine (GetMineLayoutReport); it should stay under DamageRadiusCm.
     */
    UFUNCTION(CallInEditor, BlueprintCallable, Category="Retake|Minefields") void RegenerateMines();
    /** The last Regenerate Mines summary on this machine (editor). */
    UFUNCTION(BlueprintPure, Category="Retake|Minefields") FString GetMineLayoutReport() const { return MineLayoutReport; }

    /** True when WorldBox (a pawn's collision bounds) touches Field. Analytic and the same on every machine. */
    bool OverlapsBox(const FBox& WorldBox) const;
    /** The separating-axis test behind OverlapsBox: an oriented box (centre, rotation, half extents) against an axis-aligned box.
     *  Touching counts as overlapping. */
    static bool OrientedBoxOverlaps(const FVector& Center, const FQuat& Rotation, const FVector& HalfExtent, const FBox& WorldBox);
    /** Why this field cannot arm (size, delays, damage), or empty. */
    FString GetConfigurationError() const;
    /** Required authored presentation and persistent-level placement; independent of weapon registration. */
    FString GetPresentationError() const;
    /** Why the mines need Regenerate Mines (none, no mesh, field moved or resized since), or empty. Mines are optional. */
    FString GetMineLayoutWarning() const;
    /** The layout's settings and the field's placement, as Regenerate Mines stamps them into MineLayoutHash. */
    int32 ComputeMineLayoutHash() const;

    /** The jittered grid: one point per cell over a box of HalfExtent (field-local XY), cells at most SpacingCm, row by row, and a
     *  seeded turn in degrees for each. Every point draws its jitter and turn, so a skipped point never shifts the others. */
    static void LayoutGrid(const FVector2D& HalfExtent, float SpacingCm, float Jitter, int32 Seed, TArray<FVector2D>& OutPoints, TArray<float>& OutYaws);
    /** The farthest any point of the box can be (horizontally) from its own cell's mine. */
    static double LayoutCoverageCm(const FVector2D& HalfExtent, float SpacingCm, float Jitter);

    int32 GetMineCount() const { return MineTransforms.Num(); }
    bool IsMineAvailable(int32 Index) const { return MineTransforms.IsValidIndex(Index) && !IsBitSet(ExplodedMines, Index); }
    int32 GetAvailableMineCount() const;
    /** A mine's world location (on its surface) and up direction (its surface normal). */
    FVector GetMineLocation(int32 Index) const;
    FVector GetMineUp(int32 Index) const;
    /** Server: the mine went off; it is hidden on every machine until ServerRestoreMines. False if it was gone already. */
    bool ServerConsumeMine(int32 Index);
    /** Server: every mine back (round and match resets). */
    void ServerRestoreMines();

    /** Server: the game detonated a mine at Location. Every machine and replays receive it. */
    void ServerPresentDetonation(const FVector& Location);
    /** The detonation this machine received last; read it while DetonationChannel is reported. */
    const FVector& GetLastDetonationLocation() const { return LastDetonationLocation; }

    /** The exploded-mine mask: bit Index of byte Index / 8. SetBit refuses an index outside Count and a bit that is set already. */
    static bool IsBitSet(const TArray<uint8>& Bits, int32 Index);
    static bool SetBit(TArray<uint8>& Bits, int32 Index, int32 Count);

    /** URetakeMapElementSubsystem state channels reported on every machine: a detonation arrived; the exploded mines changed. */
    static const FName DetonationChannel;
    static const FName MinesChannel;

private:
    UFUNCTION(NetMulticast, Reliable) void MulticastDetonation(const FVector_NetQuantize& Location);
    UPROPERTY(ReplicatedUsing=OnRep_ExplodedMines) TArray<uint8> ExplodedMines;
    UFUNCTION() void OnRep_ExplodedMines();
    /** Shows every available mine and hides the exploded ones (zero scale), touching only instances that change. */
    void PresentMines();
    FVector LastDetonationLocation = FVector::ZeroVector;
    FString MineLayoutReport;
#if WITH_EDITORONLY_DATA
    UPROPERTY() TObjectPtr<UBillboardComponent> Sprite;
#endif
};
