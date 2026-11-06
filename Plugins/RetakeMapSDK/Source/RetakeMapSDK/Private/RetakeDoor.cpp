// © 2026 Codnyx. All Rights Reserved.
#include "RetakeDoor.h"
#include "RetakeMapElementPushable.h"
#include "RetakeMapElementSubsystem.h"

#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "CollisionQueryParams.h"
#include "Curves/CurveFloat.h"
#include "Engine/CollisionProfile.h"
#include "Engine/OverlapResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogRetakeDoors, Log, All);

const FName ARetakeDoor::StateChannel(TEXT("Door.State"));

static TAutoConsoleVariable<int32> CVarRetakeDoorsDebug(TEXT("Retake.Doors.Debug"), 0,
	TEXT("1: print every step of door interaction on screen and to LogRetakeDoors. 0: silent."));

namespace
{
	/** How often an open auto-closing door looks at its doorway while it counts down. */
	constexpr float DoorwayCheckInterval = 0.25f;

	const TCHAR* DoorStateName(ERetakeDoorState State)
	{
		switch (State)
		{
		case ERetakeDoorState::Closed: return TEXT("Closed");
		case ERetakeDoorState::Opening: return TEXT("Opening");
		case ERetakeDoorState::Open: return TEXT("Open");
		case ERetakeDoorState::Closing: return TEXT("Closing");
		case ERetakeDoorState::Broken: return TEXT("Broken");
		default: return TEXT("Locked");
		}
	}
}

void ARetakeDoor::DebugMessage(const FString& Message, const FColor& Color)
{
	if (CVarRetakeDoorsDebug.GetValueOnGameThread() == 0) return;
	UE_LOGF(LogRetakeDoors, Log, "%ls", *Message);
#if !UE_BUILD_SHIPPING
	if (GEngine) GEngine->AddOnScreenDebugMessage(INDEX_NONE, 5.f, Color, Message);
#endif
}

ARetakeDoor::ARetakeDoor()
{
	bReplicates = true;
	bReplayRewindable = true;
	SetReplicateMovement(false);
	// Doors sit idle most of the match: stay dormant and wake only when the state changes.
	NetDormancy = DORM_DormantAll;
	SetNetUpdateFrequency(10.f);
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
	DoorPivot = CreateDefaultSubobject<USceneComponent>(TEXT("DoorPivot"));
	DoorPivot->SetupAttachment(Root);
	DoorMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DoorMesh"));
	DoorMesh->SetupAttachment(DoorPivot);
	DoorMesh->SetCollisionProfileName(UCollisionProfile::BlockAllDynamic_ProfileName);
	DoorMesh->SetGenerateOverlapEvents(false);
	DoorMesh->SetMobility(EComponentMobility::Movable);
	InteractionRange = CreateDefaultSubobject<USphereComponent>(TEXT("InteractionRange"));
	InteractionRange->SetupAttachment(DoorPivot);
	InteractionRange->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	InteractionRange->SetGenerateOverlapEvents(false);
	InteractionRange->SetCanEverAffectNavigation(false);
	InteractionRange->SetHiddenInGame(true);
	InteractionRange->ShapeColor = FColor(80, 200, 255);
}

void ARetakeDoor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARetakeDoor, DoorState);
	DOREPLIFETIME(ARetakeDoor, TargetYaw);
	DOREPLIFETIME(ARetakeDoor, NextRequestTime);
	DOREPLIFETIME_CONDITION(ARetakeDoor, ReplayProgress, COND_ReplayOnly);
}

void ARetakeDoor::PreReplicationForReplay(IRepChangedPropertyTracker& ChangedPropertyTracker)
{
	Super::PreReplicationForReplay(ChangedPropertyTracker);
	ReplayProgress = Progress;
}

void ARetakeDoor::RewindForReplay()
{
	Super::RewindForReplay();
	if (!GetWorld() || !GetWorld()->IsPlayingReplay()) return;
	GetWorldTimerManager().ClearTimer(AutoCloseTimer);
	Progress = 0.f; ReplayProgress = -1.f; TargetYaw = OpenAngle;
	DoorState = bStartLocked ? ERetakeDoorState::Locked : ERetakeDoorState::Closed;
	ApplyProgress(); UpdatePawnBlocking(); SetActorTickEnabled(false);
}

void ARetakeDoor::OnRep_ReplayProgress()
{
	if (!GetWorld() || !GetWorld()->IsPlayingReplay() || !FMath::IsFinite(ReplayProgress) || ReplayProgress < 0.f) return;
	Progress = FMath::Clamp(ReplayProgress, 0.f, 1.f);
	if (HasActorBegunPlay()) { ApplyProgress(); UpdatePawnBlocking(); }
}

void ARetakeDoor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	ApplyDoorSize();
}

void ARetakeDoor::ApplyDoorSize() const
{
	const UStaticMesh* Mesh = DoorMesh ? DoorMesh->GetStaticMesh() : nullptr;
	if (!Mesh) return;
	const FVector Native = Mesh->GetBounds().BoxExtent * 2.0;
	FVector Scale = FVector::OneVector;
	for (int32 Axis = 0; Axis < 3; ++Axis)
		if (DoorSize[Axis] > 0.0 && Native[Axis] > UE_KINDA_SMALL_NUMBER) Scale[Axis] = DoorSize[Axis] / Native[Axis];
	DoorMesh->SetRelativeScale3D(Scale);
	// Put the chosen face of the mesh on the hinge, so centred-pivot meshes swing from their edge.
	const FBox Local = Mesh->GetBounds().GetBox();
	FVector Offset = FVector::ZeroVector;
	switch (HingeEdge)
	{
	case ERetakeDoorHinge::NegativeX: Offset.X = -Local.Min.X * Scale.X; break;
	case ERetakeDoorHinge::PositiveX: Offset.X = -Local.Max.X * Scale.X; break;
	case ERetakeDoorHinge::NegativeY: Offset.Y = -Local.Min.Y * Scale.Y; break;
	case ERetakeDoorHinge::PositiveY: Offset.Y = -Local.Max.Y * Scale.Y; break;
	default: break;
	}
	if (bOriginAtBottom) Offset.Z = -Local.Min.Z * Scale.Z;
	if (HingeEdge != ERetakeDoorHinge::MeshOrigin || bOriginAtBottom) DoorMesh->SetRelativeLocation(Offset);
	DoorMesh->UpdateBounds();
	// The range is measured from the leaf centre, so the visualizer follows it (and swings with the pivot).
	InteractionRange->SetWorldLocation(DoorMesh->Bounds.Origin);
	InteractionRange->SetSphereRadius(InteractionDistance, false);
	InteractionRange->SetHiddenInGame(!(bShowInteractionRange || CVarRetakeDoorsDebug.GetValueOnGameThread() != 0));
}

void ARetakeDoor::BeginPlay()
{
	Super::BeginPlay();
	ApplyDoorSize();
	ClosedRotation = DoorPivot->GetRelativeRotation();
	CaptureLeafDefaults();
	if (HasAuthority())
	{
		// Runtime state never comes from the asset: a placed door starts closed (or locked), whatever was serialized.
		TargetYaw = OpenAngle;
		DoorState = bStartLocked ? ERetakeDoorState::Locked : ERetakeDoorState::Closed;
	}
	// Late joiners can receive an open door before BeginPlay: snap to the rest pose, animate only if moving.
	Progress = DoorState == ERetakeDoorState::Open ? 1.f : 0.f;
	if (GetWorld()->IsPlayingReplay() && ReplayProgress >= 0.f) Progress = FMath::Clamp(ReplayProgress, 0.f, 1.f);
	ApplyProgress();
	UpdatePawnBlocking();
	SetActorTickEnabled(!IsBroken() && (IsMoving() || !bLeafBlocksPawns));
	URetakeMapElementSubsystem::Register(*this);
}

void ARetakeDoor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	URetakeMapElementSubsystem::Unregister(*this);
	Super::EndPlay(EndPlayReason);
}

FVector ARetakeDoor::GetHingeLocation() const { return DoorPivot->GetComponentLocation(); }

FVector ARetakeDoor::GetInteractionLocation() const
{
	return DoorMesh && DoorMesh->GetStaticMesh() ? DoorMesh->Bounds.Origin : GetHingeLocation();
}

FText ARetakeDoor::GetPromptText() const
{
	if (IsBroken()) return FText::GetEmpty();
	if (DoorState == ERetakeDoorState::Locked) return LockedPrompt;
	return IsOpen() ? ClosePrompt : OpenPrompt;
}

FVector ARetakeDoor::LeafNormal() const
{
	const FVector Up = DoorPivot->GetUpVector();
	const FVector Leaf = FVector::VectorPlaneProject(GetInteractionLocation() - GetHingeLocation(), Up);
	if (Leaf.IsNearlyZero(1.f)) return DoorPivot->GetRightVector();
	return FVector::CrossProduct(Up, Leaf).GetSafeNormal();
}

float ARetakeDoor::EvaluateCurve(float Alpha) const
{
	if (OpenCurve) return OpenCurve->GetFloatValue(Alpha);
	return FMath::InterpEaseInOut(0.f, 1.f, Alpha, 2.f);
}

void ARetakeDoor::ApplyProgress() const
{
	DoorPivot->SetRelativeRotation(ClosedRotation + FRotator(0.f, TargetYaw * EvaluateCurve(Progress), 0.f));
}

bool ARetakeDoor::GetClosedSeal(FBox& OutLeaf, int32& OutAxis) const
{
	// A handed-over leaf can take holes, so only its own piece-by-piece cover may hide what is behind it.
	if (bLeafHandedOver || (DoorState != ERetakeDoorState::Closed && DoorState != ERetakeDoorState::Locked) || Progress != 0.f || !bLeafBlocksPawns
		|| FrameDepth <= 0.f || !DoorMesh || !DoorMesh->GetStaticMesh() || !DoorMesh->IsVisible() || DoorMesh->bHiddenInGame || IsHidden()) return false;
	// The frame's wall faces are exact axis planes, so only an upright, axis-aligned leaf can join them.
	const FTransform Leaf = DoorMesh->GetComponentTransform();
	const FVector Forward = Leaf.GetUnitAxis(EAxis::X), Up = Leaf.GetUnitAxis(EAxis::Z);
	if (FMath::Abs(Up.Z) < 1.0 - 1.e-6 || FMath::Max(FMath::Abs(Forward.X), FMath::Abs(Forward.Y)) < 1.0 - 1.e-6) return false;
	OutLeaf = DoorMesh->GetStaticMesh()->GetBoundingBox().TransformBy(Leaf);
	const FVector Size = OutLeaf.GetSize();
	OutAxis = Size.X < Size.Y ? 0 : 1;
	OutLeaf.Min[1 - OutAxis] -= FrameGap; OutLeaf.Max[1 - OutAxis] += FrameGap; OutLeaf.Min.Z -= FrameGap; OutLeaf.Max.Z += FrameGap;
	return OutLeaf.IsValid && Size[OutAxis] > 0.0 && Size[1 - OutAxis] > 2.0 * Size[OutAxis];
}

bool ARetakeDoor::IsUsableFor(const APawn* Pawn) const
{
	if (!Pawn) return false;
	const float Distance = FVector::Dist(Pawn->GetActorLocation(), GetInteractionLocation());
	const bool bUsable = DoorState != ERetakeDoorState::Locked && !IsBroken() && Distance <= InteractionDistance;
	DebugMessage(FString::Printf(TEXT("%s: %s, %.0f cm from leaf centre (limit %.0f) -> %s"), *GetActorNameOrLabel(),
		DoorStateName(DoorState), Distance, InteractionDistance, bUsable ? TEXT("usable") : TEXT("not usable")), bUsable ? FColor::Green : FColor::Orange);
	return bUsable;
}

bool ARetakeDoor::CanAcceptRequest() const
{
	if (DoorState == ERetakeDoorState::Locked || IsBroken()) return false;
	if (!bInterruptibleSwing && IsMoving()) return false;
	return ReuseDelay <= 0.f || ServerTime() >= NextRequestTime;
}

float ARetakeDoor::ServerTime() const
{
	const AGameStateBase* GameState = GetWorld() ? GetWorld()->GetGameState() : nullptr;
	return GameState ? GameState->GetServerWorldTimeSeconds() : (GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f);
}

bool ARetakeDoor::Interact(APawn* Pawn)
{
	if (!HasAuthority() || !Pawn)
	{
		DebugMessage(FString::Printf(TEXT("%s: Interact ignored (%s)"), *GetActorNameOrLabel(), HasAuthority() ? TEXT("no pawn") : TEXT("not authority")), FColor::Red);
		return false;
	}
	const ERetakeDoorState Before = DoorState;
	bool bAccepted = false;
	if (CanAcceptRequest())
	{
		switch (DoorState)
		{
		case ERetakeDoorState::Closed: bAccepted = OpenAwayFrom(Pawn->GetActorLocation()); break;
		case ERetakeDoorState::Open:
		case ERetakeDoorState::Opening: bAccepted = Close(); break;
		case ERetakeDoorState::Closing: SetDoorState(ERetakeDoorState::Opening); bAccepted = true; break;
		default: break;
		}
	}
	if (bAccepted) NextRequestTime = ServerTime() + ReuseDelay;
	DebugMessage(FString::Printf(TEXT("%s: Interact by %s from %s -> %s (target yaw %.0f)"), *GetActorNameOrLabel(), *Pawn->GetName(),
		DoorStateName(Before), bAccepted ? DoorStateName(DoorState) : TEXT("refused"), TargetYaw), bAccepted ? FColor::Green : FColor::Red);
	return bAccepted;
}

bool ARetakeDoor::Open() { return StartOpening(nullptr); }

bool ARetakeDoor::OpenAwayFrom(const FVector& Location) { return StartOpening(&Location); }

bool ARetakeDoor::StartOpening(const FVector* AwayFrom)
{
	if (!HasAuthority() || IsOpen() || DoorState == ERetakeDoorState::Locked || IsBroken()) return false;
	if (DoorState == ERetakeDoorState::Closed)
	{
		// Positive yaw moves the leaf toward its normal, so a user on the normal's side gets a negative swing.
		float Sign = OpenAngle < 0.f ? -1.f : 1.f;
		if (bSwingAwayFromUser && AwayFrom)
		{
			const float Side = FVector::DotProduct(*AwayFrom - GetHingeLocation(), LeafNormal());
			if (!FMath::IsNearlyZero(Side)) Sign = Side > 0.f ? -1.f : 1.f;
		}
		TargetYaw = FMath::Abs(OpenAngle) * Sign;
	}
	GetWorldTimerManager().ClearTimer(AutoCloseTimer);
	SetDoorState(ERetakeDoorState::Opening);
	return true;
}

bool ARetakeDoor::Close()
{
	if (!HasAuthority() || DoorState == ERetakeDoorState::Closed || DoorState == ERetakeDoorState::Locked || IsBroken()) return false;
	GetWorldTimerManager().ClearTimer(AutoCloseTimer);
	SetDoorState(ERetakeDoorState::Closing);
	return true;
}

void ARetakeDoor::SetLocked(bool bLocked)
{
	if (!HasAuthority()) return;
	if (bLocked && DoorState == ERetakeDoorState::Closed) SetDoorState(ERetakeDoorState::Locked);
	else if (!bLocked && DoorState == ERetakeDoorState::Locked) SetDoorState(ERetakeDoorState::Closed);
}

void ARetakeDoor::HandOverLeaf()
{
	CaptureLeafDefaults();
	bLeafHandedOver = true;
	// The leaf actor draws the leaf and answers shots, grenades and sight; DoorMesh keeps only the leaf's pawn body.
	DoorMesh->SetVisibility(false);
	DoorMesh->SetCollisionResponseToAllChannels(ECR_Ignore);
	DoorMesh->SetCollisionResponseToChannel(ECC_Pawn, bLeafBlocksPawns ? ECR_Block : ECR_Overlap);
	UpdatePawnBlocking();
}

void ARetakeDoor::BreakLeaf()
{
	if (!HasAuthority() || IsBroken()) return;
	GetWorldTimerManager().ClearTimer(AutoCloseTimer);
	SetDoorState(ERetakeDoorState::Broken);
}

FTransform ARetakeDoor::GetClosedPivotTransform() const
{
	// Before BeginPlay the pivot still holds its authored (closed) rotation.
	const FRotator Closed = HasActorBegunPlay() ? ClosedRotation : DoorPivot->GetRelativeRotation();
	return FTransform(Closed, DoorPivot->GetRelativeLocation(), DoorPivot->GetRelativeScale3D()) * Root->GetComponentTransform();
}

void ARetakeDoor::ResetForRound()
{
	if (!HasAuthority()) return;
	GetWorldTimerManager().ClearTimer(AutoCloseTimer);
	TargetYaw = OpenAngle;
	NextRequestTime = 0.f;
	// The server snaps, so spawns and collision see a closed leaf immediately; OnRep_DoorState restores blocking.
	Progress = 0.f;
	ApplyProgress();
	SetDoorState(bStartLocked ? ERetakeDoorState::Locked : ERetakeDoorState::Closed);
	UpdatePawnBlocking();
}

void ARetakeDoor::SetDoorState(ERetakeDoorState NewState)
{
	if (DoorState == NewState) return;
	FlushNetDormancy();
	DoorState = NewState;
	OnRep_DoorState();
	ForceNetUpdate();
}

void ARetakeDoor::OnRep_DoorState()
{
	if (!HasActorBegunPlay()) return; // BeginPlay snaps to the replicated pose.
	DebugMessage(FString::Printf(TEXT("%s: state %s on %s"), *GetActorNameOrLabel(), DoorStateName(DoorState), HasAuthority() ? TEXT("server") : TEXT("client")), FColor::Cyan);
	if (HasAuthority() && DoorState == ERetakeDoorState::Open && AutoCloseDelay > 0.f)
	{
		AutoCloseTime = GetWorld()->GetTimeSeconds() + AutoCloseDelay;
		GetWorldTimerManager().SetTimer(AutoCloseTimer, this, &ARetakeDoor::UpdateAutoClose, FMath::Min(AutoCloseDelay, DoorwayCheckInterval), false);
	}
	UpdatePawnBlocking();
	// Keep animating until the local leaf reaches the pose the state describes, even if the server got there first,
	// and keep checking while a pawn is still inside the arc so blocking can be restored. A broken door stays put.
	SetActorTickEnabled(!IsBroken() && (Progress != (IsOpen() ? 1.f : 0.f) || !bLeafBlocksPawns));
	URetakeMapElementSubsystem::NotifyStateChanged(*this, StateChannel);
}

void ARetakeDoor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (IsBroken()) { SetActorTickEnabled(false); return; }
	// The leaf moves whenever it is away from the pose its state describes. During a swing that honours obstruction and pushes
	// pawns; a client catching up with a state the server already reached (it missed Opening or Closing, or a round reset
	// snapped the server) only moves the leaf.
	const float Goal = IsOpen() ? 1.f : 0.f;
	if (Progress != Goal)
	{
		const float Previous = Progress;
		const float Next = FMath::FInterpConstantTo(Progress, Goal, DeltaSeconds, 1.f / FMath::Max(OpenDuration, 0.05f));
		TArray<APawn*> Obstructions;
		if (IsMoving() && bStopWhenObstructed) FindPawnsInLeaf(Obstructions, &Next);
		if (Obstructions.IsEmpty())
		{
			Progress = Next;
			ApplyProgress();
			const float YawDelta = TargetYaw * (EvaluateCurve(Progress) - EvaluateCurve(Previous));
			if (IsMoving() && !bStopWhenObstructed && PushSpeed > 0.f && !FMath::IsNearlyZero(YawDelta)) PushPawnsOutOfLeaf(FMath::Sign(YawDelta));
		}
	}
	if (Progress == Goal && HasAuthority())
	{
		if (DoorState == ERetakeDoorState::Opening) SetDoorState(ERetakeDoorState::Open);
		else if (DoorState == ERetakeDoorState::Closing) SetDoorState(ERetakeDoorState::Closed);
	}
	UpdatePawnBlocking();
	if (!IsMoving() && Progress == Goal && bLeafBlocksPawns) SetActorTickEnabled(false);
}

void ARetakeDoor::UpdateAutoClose()
{
	const double Now = GetWorld()->GetTimeSeconds();
	if (IsDoorwayOccupied()) AutoCloseTime = Now + AutoCloseDelay;
	const double Remaining = AutoCloseTime - Now;
	if (Remaining <= UE_KINDA_SMALL_NUMBER) { Close(); return; }
	GetWorldTimerManager().SetTimer(AutoCloseTimer, this, &ARetakeDoor::UpdateAutoClose, FMath::Min(static_cast<float>(Remaining), DoorwayCheckInterval), false);
}

bool ARetakeDoor::IsDoorwayOccupied() const
{
	const UStaticMesh* Mesh = DoorMesh ? DoorMesh->GetStaticMesh() : nullptr;
	if (!Mesh || !GetWorld()) return false;
	// The closed leaf's box grown horizontally by the leaf's width, the reach of its swing, on every side.
	const FTransform Leaf = DoorMesh->GetRelativeTransform() * GetClosedPivotTransform();
	const FBox Local = Mesh->GetBoundingBox();
	FVector Extent = Local.GetExtent() * Leaf.GetScale3D().GetAbs();
	const double Width = 2.0 * FMath::Max(Extent.X, Extent.Y);
	Extent.X += Width;
	Extent.Y += Width;
	// A pawn-channel query: bodies that ignore pawns (spectators, Retake's dead) never hold a door open.
	TArray<FOverlapResult> Overlaps;
	GetWorld()->OverlapMultiByChannel(Overlaps, Leaf.TransformPosition(Local.GetCenter()), Leaf.GetRotation(), ECC_Pawn,
		FCollisionShape::MakeBox(Extent), FCollisionQueryParams(SCENE_QUERY_STAT(RetakeDoorDoorway), false, this));
	return Overlaps.ContainsByPredicate([](const FOverlapResult& Overlap) { return Cast<APawn>(Overlap.GetActor()) != nullptr; });
}

void ARetakeDoor::FindPawnsInLeaf(TArray<APawn*>& OutPawns, const float* AtProgress) const
{
	OutPawns.Reset();
	if (!DoorMesh || !DoorMesh->GetStaticMesh() || !GetWorld()) return;
	FTransform Leaf = DoorMesh->GetComponentTransform();
	if (AtProgress)
	{
		// Pose the leaf would have at that progress, without moving anything.
		const FRotator PivotRotation = ClosedRotation + FRotator(0.f, TargetYaw * EvaluateCurve(*AtProgress), 0.f);
		const FTransform Pivot = FTransform(PivotRotation, DoorPivot->GetRelativeLocation(), DoorPivot->GetRelativeScale3D()) * Root->GetComponentTransform();
		Leaf = DoorMesh->GetRelativeTransform() * Pivot;
	}
	TArray<FOverlapResult> Overlaps;
	FComponentQueryParams Params(SCENE_QUERY_STAT(RetakeDoorLeaf), this);
	GetWorld()->ComponentOverlapMultiByChannel(Overlaps, DoorMesh, Leaf.GetLocation(), Leaf.GetRotation(), ECC_Pawn, Params);
	for (const FOverlapResult& Overlap : Overlaps)
		if (APawn* Pawn = Cast<APawn>(Overlap.GetActor())) OutPawns.AddUnique(Pawn);
}

void ARetakeDoor::PushPawnsOutOfLeaf(float YawDirection) const
{
	if (GetWorld() && GetWorld()->IsPlayingReplay()) return;
	TArray<APawn*> Pawns;
	FindPawnsInLeaf(Pawns);
	if (Pawns.IsEmpty()) return;
	const FVector Hinge = GetHingeLocation();
	const FVector Up = DoorPivot->GetUpVector();
	for (APawn* Pawn : Pawns)
	{
		// The push is part of the movement simulation: the server drives every pawn, a client only its own (predicted).
		if (!HasAuthority() && !Pawn->IsLocallyControlled()) continue;
		IRetakeMapElementPushable* Pushable = Cast<IRetakeMapElementPushable>(Pawn);
		if (!Pushable) continue;
		FVector Radial = FVector::VectorPlaneProject(Pawn->GetActorLocation() - Hinge, Up);
		if (Radial.IsNearlyZero(1.f)) Radial = FVector::VectorPlaneProject(GetInteractionLocation() - Hinge, Up);
		// A point on the leaf moves along Up x Radial for positive yaw; that tangent carries the pawn out of the arc.
		const FVector Direction = (FVector::CrossProduct(Up, Radial).GetSafeNormal() * YawDirection);
		if (Direction.IsNearlyZero()) continue;
		Pushable->ApplyMapElementPush(*this, Direction, PushSpeed);
	}
}

void ARetakeDoor::CaptureLeafDefaults()
{
	if (bLeafDefaultsCaptured || !DoorMesh) return;
	bLeafDefaultsCaptured = true;
	bAuthoredLeafVisible = DoorMesh->IsVisible();
	LeafCollision = DoorMesh->GetCollisionEnabled();
}

void ARetakeDoor::UpdatePawnBlocking()
{
	if (!DoorMesh) return;
	CaptureLeafDefaults();
	// Destruction is persistent state, not a cosmetic event a reconnecting client must have witnessed.
	// A handed-over mesh remains hidden through round resets; its replacement collection draws the restored leaf.
	const bool bVisible = bAuthoredLeafVisible && !bLeafHandedOver && !IsBroken();
	if (DoorMesh->IsVisible() != bVisible) DoorMesh->SetVisibility(bVisible);
	// A broken door has no leaf left to block anything; the round reset brings the authored collision back.
	const ECollisionEnabled::Type Collision = IsBroken() ? ECollisionEnabled::NoCollision : LeafCollision.GetValue();
	if (DoorMesh->GetCollisionEnabled() != Collision) DoorMesh->SetCollisionEnabled(Collision);
	if (IsBroken()) return;
	// The swinging leaf stays solid, so nobody walks through it before it has moved out of the way; pawns it swings into are
	// pushed out of its arc and depenetrated by their own movement. Only a pawn left inside the leaf at rest (pinned against a
	// wall) makes it overlap pawns until the arc is clear.
	bool bShouldBlock = true;
	if (PushSpeed > 0.f && !bStopWhenObstructed && !IsMoving())
	{
		TArray<APawn*> Pawns;
		FindPawnsInLeaf(Pawns);
		bShouldBlock = Pawns.IsEmpty();
	}
	if (bShouldBlock == bLeafBlocksPawns) return;
	bLeafBlocksPawns = bShouldBlock;
	DoorMesh->SetCollisionResponseToChannel(ECC_Pawn, bShouldBlock ? ECR_Block : ECR_Overlap);
}
