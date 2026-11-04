// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeDetonationTarget.h"
#include "RetakeMapElementSubsystem.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

const FName ARetakeDetonationTarget::StateChannel(TEXT("Detonation.State"));

ARetakeDetonationTarget::ARetakeDetonationTarget()
{
    bReplicates = true;
    PrimaryActorTick.bCanEverTick = false;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
    IntactMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("IntactMesh"));
    IntactMesh->SetupAttachment(RootComponent);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
    IntactMesh->SetStaticMesh(Cube.Object);
    IntactMesh->SetRelativeLocation(FVector(0, 0, 50));
    IntactMesh->SetCollisionProfileName(TEXT("BlockAll"));
    DestroyedMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DestroyedMesh"));
    DestroyedMesh->SetupAttachment(RootComponent);
    DestroyedMesh->SetCollisionProfileName(TEXT("BlockAll"));
    DestroyedMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    DestroyedMesh->SetVisibility(false);
    DestroyedMesh->SetHiddenInGame(true);
    ExplosionPoint = CreateDefaultSubobject<USceneComponent>(TEXT("ExplosionPoint"));
    ExplosionPoint->SetupAttachment(RootComponent);
#if WITH_EDITORONLY_DATA
    ExplosionPoint->bVisualizeComponent = true;
#endif
}

FVector ARetakeDetonationTarget::GetExplosionLocation() const
{
    return ExplosionPoint ? ExplosionPoint->GetComponentLocation() : GetActorLocation();
}

void ARetakeDetonationTarget::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    ApplyRepresentation();
}

void ARetakeDetonationTarget::BeginPlay()
{
    Super::BeginPlay();
    ApplyRepresentation();
    URetakeMapElementSubsystem::Register(*this);
}

void ARetakeDetonationTarget::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    URetakeMapElementSubsystem::Unregister(*this);
    Super::EndPlay(EndPlayReason);
}

void ARetakeDetonationTarget::ApplyRepresentation()
{
    const bool bShowIntact = bAvailable && !bDestroyed;
    IntactMesh->SetVisibility(bShowIntact);
    IntactMesh->SetHiddenInGame(!bShowIntact);
    IntactMesh->SetCollisionEnabled(bShowIntact ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
    const bool bShowWreck = bAvailable && bDestroyed && DestroyedMesh->GetStaticMesh();
    DestroyedMesh->SetVisibility(bShowWreck);
    DestroyedMesh->SetHiddenInGame(!bShowWreck);
    DestroyedMesh->SetCollisionEnabled(bShowWreck ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
}

void ARetakeDetonationTarget::SetDestroyed(bool bNewDestroyed)
{
    if (!HasAuthority() || bDestroyed == bNewDestroyed) return;
    bDestroyed = bNewDestroyed;
    OnRep_Destroyed();
    ForceNetUpdate();
}

void ARetakeDetonationTarget::SetPreviewDestroyed(bool bNewDestroyed)
{
    bDestroyed = bNewDestroyed;
    ApplyRepresentation();
}

void ARetakeDetonationTarget::SetAvailable(bool bNewAvailable)
{
    if (bAvailable == bNewAvailable) return;
    bAvailable = bNewAvailable;
    ApplyRepresentation();
    URetakeMapElementSubsystem::NotifyStateChanged(*this, StateChannel);
}

void ARetakeDetonationTarget::OnRep_Destroyed()
{
    ApplyRepresentation();
    URetakeMapElementSubsystem::NotifyStateChanged(*this, StateChannel);
}

void ARetakeDetonationTarget::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ARetakeDetonationTarget, bDestroyed);
}
