// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeModeObjectiveMarker.h"
#include "RetakeMapElementSubsystem.h"
#include "Components/ArrowComponent.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Misc/DataValidation.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

const FName ARetakeModeObjectiveMarker::StateChannel(TEXT("Objective.State"));

ARetakeModeObjectiveMarker::ARetakeModeObjectiveMarker()
{
    PrimaryActorTick.bCanEverTick = false;
    bReplicates = true;
    bAlwaysRelevant = true;
    Zone = CreateDefaultSubobject<UBoxComponent>(TEXT("Zone"));
    SetRootComponent(Zone);
    Zone->SetBoxExtent(FVector(220, 220, 180));
    Zone->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
    Visual = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Visual"));
    Visual->SetupAttachment(Zone);
    Visual->SetStaticMesh(Cube.Object);
    Visual->SetRelativeScale3D(FVector(1.2f, 1.2f, 0.08f));
    Visual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Label = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Label"));
    Label->SetupAttachment(Zone);
    Label->SetRelativeLocation(FVector(0, 0, 100));
    Label->SetWorldSize(45);
    Label->SetHorizontalAlignment(EHTA_Center);
    UplinkAnchor = CreateDefaultSubobject<USceneComponent>(TEXT("UplinkAnchor"));
    UplinkAnchor->SetupAttachment(Zone);
    UplinkAnchor->SetAbsolute(false, false, true); // Resizing a trigger zone must not resize the device or its collider.
    UplinkAnchor->SetRelativeRotation(FRotator(0, -90, 0));
    UplinkBody = CreateDefaultSubobject<UBoxComponent>(TEXT("UplinkBody"));
    UplinkBody->SetupAttachment(UplinkAnchor);
    UplinkBody->SetRelativeLocation(FVector(0, 0, 33));
    UplinkBody->SetBoxExtent(FVector(62, 43, 33));
    UplinkBody->SetCollisionProfileName(TEXT("BlockAll"));
    UplinkBody->SetGenerateOverlapEvents(false);
    UplinkBody->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    UplinkWreck = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("UplinkWreck"));
    UplinkWreck->SetupAttachment(Zone);
    UplinkWreck->SetAbsolute(false, false, true);
    UplinkWreck->SetRelativeRotation(FRotator(0, -90, 0));
    UplinkWreck->SetCollisionProfileName(TEXT("BlockAll"));
    UplinkWreck->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    UplinkWreck->SetVisibility(false);
    UplinkWreck->SetHiddenInGame(true);
#if WITH_EDITORONLY_DATA
    DeviceProxy = CreateEditorOnlyDefaultSubobject<UStaticMeshComponent>(TEXT("DeviceProxy"));
    if (DeviceProxy)
    {
        DeviceProxy->SetupAttachment(UplinkAnchor);
        DeviceProxy->SetStaticMesh(Cube.Object);
        DeviceProxy->SetRelativeLocation(FVector(0, 0, 33));
        DeviceProxy->SetRelativeScale3D(FVector(1.24f, 0.86f, 0.66f)); // UplinkBody's 124 x 86 x 66 cm on the 100 cm cube.
        DeviceProxy->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        DeviceProxy->SetHiddenInGame(true);
        DeviceProxy->SetCastShadow(false);
        DeviceProxy->bIsEditorOnly = true;
    }
    DeviceFront = CreateEditorOnlyDefaultSubobject<UArrowComponent>(TEXT("DeviceFront"));
    if (DeviceFront)
    {
        DeviceFront->SetupAttachment(UplinkAnchor);
        DeviceFront->SetRelativeLocation(FVector(0, 0, 33));
        DeviceFront->SetRelativeRotation(FRotator(0, 90, 0)); // The device front is the anchor's +Y.
        DeviceFront->SetHiddenInGame(true);
    }
#endif
}

void ARetakeModeObjectiveMarker::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    OnRep_MarkerMetadata();
}

void ARetakeModeObjectiveMarker::PostRegisterAllComponents()
{
    Super::PostRegisterAllComponents();
    OnRep_MarkerMetadata();
}

void ARetakeModeObjectiveMarker::BeginPlay()
{
    Super::BeginPlay();
    URetakeMapElementSubsystem::Register(*this);
}

void ARetakeModeObjectiveMarker::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    URetakeMapElementSubsystem::Unregister(*this);
    Super::EndPlay(EndPlayReason);
}

void ARetakeModeObjectiveMarker::OnRep_MarkerMetadata()
{
    // Client construction can precede initial actor replication. Rebuild the local text
    // once the server-authored metadata arrives instead of retaining the CDO's empty ID.
    if (Label) Label->SetText(FText::AsCultureInvariant(ObjectiveId.ToString()));
    SetActorHiddenInGame(!bObjectiveEnabled);
    const bool bUplink = UsesFieldUplink();
    // Field Uplink and planted-charge sites are presented by the game (and its editor preview); the pad and label mark only
    // free-placement sites.
    const bool bShowPad = !bUplink && !UsesPlantedCharge();
    if (Visual) Visual->SetVisibility(bShowPad);
    if (Label) Label->SetVisibility(bShowPad);
    if (UplinkBody) UplinkBody->SetCollisionEnabled(bUplink && bObjectiveEnabled && !bUplinkDestroyed ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
    if (UplinkWreck)
    {
        const bool bShowWreck = bUplink && bUplinkDestroyed;
        UplinkWreck->SetVisibility(bShowWreck);
        UplinkWreck->SetHiddenInGame(!bShowWreck);
        UplinkWreck->SetCollisionEnabled(bShowWreck && bObjectiveEnabled ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
    }
#if WITH_EDITORONLY_DATA
    if (DeviceProxy) DeviceProxy->SetVisibility(bUplink);
    if (DeviceFront) DeviceFront->SetVisibility(bUplink);
#endif
    URetakeMapElementSubsystem::NotifyStateChanged(*this, StateChannel);
}

void ARetakeModeObjectiveMarker::SetObjectiveEnabled(bool bEnabled)
{
    if (!HasAuthority()) return;
    bObjectiveEnabled = bEnabled;
    SetActorHiddenInGame(!bEnabled);
    OnRep_MarkerMetadata();
    ForceNetUpdate();
}

void ARetakeModeObjectiveMarker::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME_CONDITION(ARetakeModeObjectiveMarker, ObjectiveId, COND_InitialOnly);
    DOREPLIFETIME_CONDITION(ARetakeModeObjectiveMarker, Kind, COND_InitialOnly);
    DOREPLIFETIME_CONDITION(ARetakeModeObjectiveMarker, TeamIndex, COND_InitialOnly);
    DOREPLIFETIME_CONDITION(ARetakeModeObjectiveMarker, bUseFieldUplink, COND_InitialOnly);
    DOREPLIFETIME_CONDITION(ARetakeModeObjectiveMarker, DevicePresentationId, COND_InitialOnly);
    DOREPLIFETIME(ARetakeModeObjectiveMarker, bObjectiveEnabled);
    DOREPLIFETIME(ARetakeModeObjectiveMarker, bUplinkDestroyed);
    DOREPLIFETIME(ARetakeModeObjectiveMarker, DetonationMode);
}

FVector ARetakeModeObjectiveMarker::GetBlastOrigin(const FVector& PlantedLocation) const
{
    return DetonatesRemotely() && IsValid(DetonationTarget) ? DetonationTarget->GetExplosionLocation() : PlantedLocation;
}

bool ARetakeModeObjectiveMarker::ExecuteDetonation(AActor*& OutSource)
{
    if (!HasAuthority()) return false;
    if (DetonationMode == ERetakeUplinkDetonationMode::TriggerTarget)
    {
        if (!IsValid(DetonationTarget) || DetonationTarget->GetWorld() != GetWorld()) return false;
        OutSource = DetonationTarget;
        // The objective event owns the gameplay explosion effect; the target only changes representation.
        DetonationTarget->SetDestroyed(true);
        return true;
    }
    OutSource = this;
    bUplinkDestroyed = true;
    OnRep_MarkerMetadata();
    ForceNetUpdate();
    return true;
}

void ARetakeModeObjectiveMarker::ResetDetonation()
{
    if (!HasAuthority()) return;
    bUplinkDestroyed = false;
    if (IsValid(DetonationTarget)) DetonationTarget->SetDestroyed(false);
    OnRep_MarkerMetadata();
    ForceNetUpdate();
}

bool ARetakeModeObjectiveMarker::ContainsPawn(const APawn* Pawn) const
{
    if (!IsValid(Pawn) || !Zone) return false;
    const FVector Local = Zone->GetComponentTransform().InverseTransformPosition(Pawn->GetActorLocation());
    const FVector Extent = Zone->GetUnscaledBoxExtent();
    return FMath::Abs(Local.X) <= Extent.X && FMath::Abs(Local.Y) <= Extent.Y && FMath::Abs(Local.Z) <= Extent.Z;
}

bool ARetakeModeObjectiveMarker::IsChargeSeated(const FVector& Location) const
{
    const UWorld* World = GetWorld();
    if (!World || Location.ContainsNaN()) return false;
    const FCollisionQueryParams Params(SCENE_QUERY_STAT(RetakeChargeSeat), true, this);
    return World->OverlapBlockingTestByChannel(Location, FQuat::Identity, ECC_Visibility, FCollisionShape::MakeSphere(ChargeSeatToleranceCm), Params);
}

FString ARetakeModeObjectiveMarker::GetChargeSeatWarning() const
{
    if (!GetWorld()) return FString();
    if (UsesPlantedCharge() && !IsChargeSeated(GetActorLocation()))
        return FString::Printf(TEXT("The charge point floats: nothing solid lies within %.0f cm of the marker, so the planted charge and its explosion would hang in the air. Seat the marker 0.5-1 cm off the surface the charge is mounted on."),
            ChargeSeatToleranceCm);
    if (DetonatesRemotely() && IsValid(DetonationTarget) && !IsChargeSeated(DetonationTarget->GetExplosionLocation()))
        return FString::Printf(TEXT("%s's Explosion Point floats: nothing solid lies within %.0f cm of it, so the remote detonation would explode in the air. Move it onto the target's surface, where its charge sits."),
            *DetonationTarget->GetActorNameOrLabel(), ChargeSeatToleranceCm);
    return FString();
}

FString ARetakeModeObjectiveMarker::GetPlacementError() const
{
    const FVector Extent = Zone ? Zone->GetScaledBoxExtent() : FVector::ZeroVector;
    if (GetActorLocation().ContainsNaN() || Extent.ContainsNaN() || Extent.GetMin() <= KINDA_SMALL_NUMBER) return TEXT("The zone needs a valid location and a positive size.");
    if (!FMath::IsFinite(InteractionRange) || InteractionRange <= 0) return TEXT("Interaction Range must be positive.");
    if (!FMath::IsFinite(PickupRadius) || PickupRadius <= 0) return TEXT("Pickup Radius must be positive.");
    return FString();
}

#if WITH_EDITOR
EDataValidationResult ARetakeModeObjectiveMarker::IsDataValid(FDataValidationContext& Context) const
{
    EDataValidationResult Result = Super::IsDataValid(Context);
    if (const FString PlacementError = IsTemplate() ? FString() : GetPlacementError(); !PlacementError.IsEmpty())
    {
        Context.AddError(FText::FromString(PlacementError));
        Result = EDataValidationResult::Invalid;
    }
    if (UsesFieldUplink() && !RetakeMapSDKIds::DevicePresentations().Contains(DevicePresentationId))
    {
        Context.AddError(FText::FromString(FString::Printf(TEXT("Unknown device presentation '%s'."), *DevicePresentationId.ToString())));
        Result = EDataValidationResult::Invalid;
    }
    if (DetonationMode == ERetakeUplinkDetonationMode::TriggerTarget && !IsValid(DetonationTarget) && !IsTemplate())
    {
        Context.AddError(FText::FromString(TEXT("Trigger Target needs a detonation target placed in the same level.")));
        Result = EDataValidationResult::Invalid;
    }
    if (const FString SeatWarning = IsTemplate() ? FString() : GetChargeSeatWarning(); !SeatWarning.IsEmpty())
        Context.AddWarning(FText::FromString(SeatWarning));
    if (!PlacementGroup.IsNone() && !IsTemplate())
    {
        if (Kind != ERetakeModeMarkerKind::BombSite)
        {
            Context.AddError(FText::FromString(TEXT("Placement Group applies to bomb sites only.")));
            Result = EDataValidationResult::Invalid;
        }
        int32 Members = 0;
        if (const ULevel* Level = GetLevel())
            for (const AActor* Actor : Level->Actors)
                if (const auto* Other = Cast<ARetakeModeObjectiveMarker>(Actor); Other && Other->PlacementGroup == PlacementGroup) ++Members;
        if (Members < 2)
            Context.AddWarning(FText::FromString(FString::Printf(TEXT("Placement group %s has one member, so it is always chosen. Add another placement or clear the group."),
                *PlacementGroup.ToString())));
    }
    return Result;
}
#endif

ARetakeFieldUplinkBombSite::ARetakeFieldUplinkBombSite()
{
    Kind = ERetakeModeMarkerKind::BombSite;
    bFixedBombPlacement = true;
    bUseFieldUplink = true;
}
