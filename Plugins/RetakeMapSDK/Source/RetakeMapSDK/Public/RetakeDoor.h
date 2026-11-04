// © 2026 Codnyx. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RetakeDoor.generated.h"

class APawn;
class UCurveFloat;
class USceneComponent;
class USphereComponent;
class UStaticMeshComponent;

/** SDK contract (MKD-029): cooked maps store these numbers. Append only; never reorder or renumber. */
UENUM(BlueprintType)
enum class ERetakeDoorState : uint8
{
	Closed = 0,
	Opening = 1,
	Open = 2,
	Closing = 3,
	Locked = 4,
	/** The leaf was destroyed (a destructible leaf, see HandOverLeaf). The doorway stays open until the round reset. */
	Broken = 5
};

/**
 * Which edge of the mesh sits on the hinge. Lets meshes authored with a centred pivot be used without re-export.
 * SDK contract (MKD-029): cooked maps store these numbers. Append only; never reorder or renumber.
 */
UENUM(BlueprintType)
enum class ERetakeDoorHinge : uint8
{
	/** The mesh's own origin is the hinge; the mesh is left where it is. */
	MeshOrigin = 0,
	/** The mesh is shifted so its -X face sits on the hinge. */
	NegativeX = 1,
	PositiveX = 2,
	NegativeY = 3,
	PositiveY = 4
};

/**
 * Server-authoritative hinged door (RetakeMapSDK element, MKD-029). The mesh hangs from DoorPivot, which is the hinge:
 * place the pivot on the hinge edge and leave the mesh at the pivot. The game selects the door for its interact action and
 * calls Interact on the server; the leaf swings away from whoever used it. Motion is driven locally on every machine from
 * replicated state, so clients never wait for per-frame updates. The moving leaf pushes pawns that implement
 * IRetakeMapElementPushable; it registers with URetakeMapElementSubsystem and reports StateChannel on every state change.
 * Frozen per content generation: UPROPERTY names, order, types and defaults, and the subobject names Root, DoorPivot,
 * DoorMesh and InteractionRange (visibility bakes hash DoorMesh's path). The replicated members are also the live and
 * replay wire layout.
 */
UCLASS(Blueprintable)
class RETAKEMAPSDK_API ARetakeDoor : public AActor
{
	GENERATED_BODY()
public:
	ARetakeDoor();
	virtual void PreReplicationForReplay(IRepChangedPropertyTracker& ChangedPropertyTracker) override;
	virtual void RewindForReplay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void Tick(float DeltaSeconds) override;

private:
	UPROPERTY(ReplicatedUsing=OnRep_ReplayProgress) float ReplayProgress = -1.f;
	UFUNCTION() void OnRep_ReplayProgress();
public:

	/** True while the pawn is close enough and the door accepts input. Server and client safe. */
	UFUNCTION(BlueprintPure, Category="Retake|Doors") bool IsUsableFor(const APawn* Pawn) const;
	/** Server only. Toggles the door for the pawn; a request during a swing reverses it. */
	UFUNCTION(BlueprintCallable, Category="Retake|Doors") bool Interact(APawn* Pawn);
	/** Server only. Opens along OpenAngle's sign. */
	UFUNCTION(BlueprintCallable, Category="Retake|Doors") bool Open();
	/** Server only. Opens away from a world location when bSwingAwayFromUser is set, otherwise like Open. */
	UFUNCTION(BlueprintCallable, Category="Retake|Doors") bool OpenAwayFrom(const FVector& Location);
	UFUNCTION(BlueprintCallable, Category="Retake|Doors") bool Close();
	/** Server only. Locks a closed door or unlocks a locked one; an open door is left alone. */
	UFUNCTION(BlueprintCallable, Category="Retake|Doors") void SetLocked(bool bLocked);
	/** Server: back to the authored initial state (closed, or locked with bStartLocked) at once, ready for use. The game calls
	 *  it whenever the map resets for a match or round; clients swing the leaf to the replicated state. */
	void ResetForRound();
	UFUNCTION(BlueprintPure, Category="Retake|Doors") ERetakeDoorState GetDoorState() const { return DoorState; }
	UFUNCTION(BlueprintPure, Category="Retake|Doors") bool IsOpen() const { return DoorState == ERetakeDoorState::Open || DoorState == ERetakeDoorState::Opening; }
	UFUNCTION(BlueprintPure, Category="Retake|Doors") bool IsLocked() const { return DoorState == ERetakeDoorState::Locked; }
	UFUNCTION(BlueprintPure, Category="Retake|Doors") bool IsBroken() const { return DoorState == ERetakeDoorState::Broken; }
	/**
	 * Another actor now draws the leaf and takes its hits (a destructible leaf attached to DoorPivot). DoorMesh stays as the leaf's
	 * pawn body, for blocking and the swing push, but is no longer drawn, shot, or used as sight cover. Local: every machine calls it
	 * when the leaf attaches.
	 */
	void HandOverLeaf();
	bool IsLeafHandedOver() const { return bLeafHandedOver; }
	/** Server only. The handed-over leaf is gone: the door becomes Broken (no use, no prompt, no pawn body) until ResetForRound. */
	void BreakLeaf();
	/** DoorPivot's world transform while the door is closed, whatever the current swing. */
	FTransform GetClosedPivotTransform() const;
	/** False while locked, cooling down after the last request, or mid-swing on a door that cannot be interrupted. */
	UFUNCTION(BlueprintPure, Category="Retake|Doors") bool CanAcceptRequest() const;
	/** Hinge location in world space. */
	UFUNCTION(BlueprintPure, Category="Retake|Doors") FVector GetHingeLocation() const;
	/** Middle of the leaf in world space; interaction distance is measured from here. */
	UFUNCTION(BlueprintPure, Category="Retake|Doors") FVector GetInteractionLocation() const;
	/**
	 * True while the leaf is closed, at rest, solid to pawns and axis-aligned: the visibility system then treats the leaf
	 * and its frame as one solid wall. OutLeaf is the closed leaf's world box, OutAxis its thin horizontal axis (0 X, 1 Y).
	 */
	bool GetClosedSeal(FBox& OutLeaf, int32& OutAxis) const;
	/** Action text for the interaction prompt in the current state. */
	UFUNCTION(BlueprintPure, Category="Retake|Doors") FText GetPromptText() const;

	/** Prints one interaction debug line on screen (non-shipping) and to LogRetakeDoors while `Retake.Doors.Debug` is non-zero. */
	static void DebugMessage(const FString& Message, const FColor& Color = FColor::Yellow);

	/** URetakeMapElementSubsystem state channel reported whenever DoorState changes (server and clients). */
	static const FName StateChannel;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Doors") TObjectPtr<USceneComponent> Root;
	/** The hinge. Only this component rotates. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Doors") TObjectPtr<USceneComponent> DoorPivot;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Doors") TObjectPtr<UStaticMeshComponent> DoorMesh;
	/** Visualizer only: the sphere around the leaf centre inside which the interact key reaches this door. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Doors") TObjectPtr<USphereComponent> InteractionRange;

	/**
	 * Target size of the leaf in the mesh's own axes (X width along the leaf, Y thickness, Z height), in cm. The mesh
	 * scale is derived from this against the mesh bounds, so any mesh fits the frame. Zero on an axis keeps the
	 * mesh's native size on that axis; all zero shows the mesh exactly as authored.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Doors", meta=(ClampMin="0", Units="cm")) FVector DoorSize = FVector::ZeroVector;
	/** Edge of the mesh placed on the hinge (the actor's pivot). Use it for meshes whose origin is not on the hinge. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Doors") ERetakeDoorHinge HingeEdge = ERetakeDoorHinge::MeshOrigin;
	/** Shift the mesh so the actor origin sits at the bottom of the leaf; Snap To Floor then puts the door on the ground. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Doors") bool bOriginAtBottom = true;
	/** Swing in degrees. The sign is the default direction when the door does not swing away from its user. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Doors", meta=(ClampMin="-179", ClampMax="179", Units="deg")) float OpenAngle = 90.f;
	/** Push the leaf away from whoever opens it, so the door works from both sides. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Doors") bool bSwingAwayFromUser = true;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Doors", meta=(ClampMin="0.05", Units="s")) float OpenDuration = 0.7f;
	/** Seconds after an accepted request before the door takes the next one, so it cannot be slammed shut right after opening. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Doors", meta=(ClampMin="0", Units="s")) float ReuseDelay = 0.5f;
	/** When set, a request during a swing reverses it; when clear, requests are refused until the swing has finished. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Doors") bool bInterruptibleSwing = false;
	/**
	 * Speed, in cm/s, at which the moving leaf pushes pawns out of its arc (Tarkov / Call of Duty style). The leaf
	 * stays solid while it swings, so players cannot pass through it before it has moved out of the way; a pawn still
	 * inside the leaf when it stops (pinned against a wall) can leave it until the arc is clear. Zero disables the
	 * push and keeps the leaf solid at all times.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Doors", meta=(ClampMin="0", Units="cm/s")) float PushSpeed = 250.f;
	/**
	 * Classic blocking door: the leaf stays solid and pauses whenever a pawn is in the way, resuming when the arc is
	 * clear. Overrides PushSpeed. Players can hold such a door shut by standing in it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Doors") bool bStopWhenObstructed = false;
	/** Optional 0..1 easing over normalized time; an ease-in-out is used when unset. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Doors") TObjectPtr<UCurveFloat> OpenCurve;
	/** Measured from the middle of the leaf. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Doors", meta=(ClampMin="1", Units="cm")) float InteractionDistance = 180.f;
	/**
	 * Seconds an open door stays open before closing on its own; zero keeps it open. A pawn in the doorway holds it open: the
	 * delay counts from the last moment one stood in the closed leaf's box grown by the leaf's width on every side (the doorway,
	 * both swing arcs and their approaches).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Doors", meta=(ClampMin="0", Units="s")) float AutoCloseDelay = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Doors") bool bStartLocked = false;
	/**
	 * How far the surrounding wall reaches from the leaf on each side, in cm. While the door is closed, wall faces within
	 * this distance that meet the leaf are treated as one solid wall with it, so players behind a closed door are
	 * withheld like players behind a wall. The wall must be solid across that depth and the leaf must fill the frame.
	 * Zero disables it: the leaf still hides what it covers on its own.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Doors|Visibility", meta=(ClampMin="0", Units="cm")) float FrameDepth = 40.f;
	/** Gaps up to this size between the closed leaf and its frame (sides, top, floor) count as closed. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Doors|Visibility", meta=(ClampMin="0", ClampMax="10", Units="cm")) float FrameGap = 3.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Doors|Prompt") FText OpenPrompt = NSLOCTEXT("RetakeDoors", "OpenPrompt", "Open door");
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Doors|Prompt") FText ClosePrompt = NSLOCTEXT("RetakeDoors", "ClosePrompt", "Close door");
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Doors|Prompt") FText LockedPrompt = NSLOCTEXT("RetakeDoors", "LockedPrompt", "Locked");
	/** Draws the interaction sphere while playing; the editor viewport always shows it. `Retake.Doors.Debug 1` shows it on every door. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Doors|Debug") bool bShowInteractionRange = false;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY(ReplicatedUsing=OnRep_DoorState) ERetakeDoorState DoorState = ERetakeDoorState::Closed;
	/** Signed yaw the pivot reaches when fully open. Chosen on the server per swing. */
	UPROPERTY(Replicated) float TargetYaw = 90.f;
	UFUNCTION() void OnRep_DoorState();

	/** Derives the mesh scale from DoorSize, shifts the mesh onto the hinge edge, and re-centres the range visualizer. */
	void ApplyDoorSize() const;
	/** Pawns whose capsule intersects the leaf at its current pose, or at the pose the pivot would have at Alpha. */
	void FindPawnsInLeaf(TArray<APawn*>& OutPawns, const float* AtProgress = nullptr) const;
	/** Pushes pawns inside the leaf along its direction of travel; YawDirection is the sign of this frame's yaw change. */
	void PushPawnsOutOfLeaf(float YawDirection) const;
	/** Leaf blocks pawns, swinging or at rest; it overlaps them only while a pawn is left inside it at rest. */
	void UpdatePawnBlocking();
	/** Server: closes the open door once AutoCloseTime has passed, pushing it back while the doorway is occupied. Re-arms itself. */
	void UpdateAutoClose();
	/** A live pawn is in the area that holds an auto-closing door open (see AutoCloseDelay). */
	bool IsDoorwayOccupied() const;
	bool StartOpening(const FVector* AwayFrom);
	void SetDoorState(ERetakeDoorState NewState);
	void ApplyProgress() const;
	bool IsMoving() const { return DoorState == ERetakeDoorState::Opening || DoorState == ERetakeDoorState::Closing; }
	/** Outward normal of the closed leaf in world space, derived from hinge and leaf geometry. */
	FVector LeafNormal() const;
	float EvaluateCurve(float Alpha) const;

	/** 0 = closed, 1 = fully open. Animated locally on every machine. */
	float Progress = 0.f;
	/** Pivot rotation authored in the editor; swings are added on top of it. */
	FRotator ClosedRotation = FRotator::ZeroRotator;
	FTimerHandle AutoCloseTimer;
	/** Server world time at which the open door closes on its own. */
	double AutoCloseTime = 0.0;
	bool bLeafBlocksPawns = true;
	bool bLeafHandedOver = false;
	/** DoorMesh's authored collision, restored when a broken door resets. */
	TEnumAsByte<ECollisionEnabled::Type> LeafCollision = ECollisionEnabled::QueryAndPhysics;
	/** Server world time at which the door accepts the next request; replicated so client prompts grey out honestly. */
	UPROPERTY(Replicated) float NextRequestTime = 0.f;
	float ServerTime() const;
};
