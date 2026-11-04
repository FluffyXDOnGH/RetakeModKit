// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeFreezeTagRelay.h"
#include "RetakeMapElementSubsystem.h"
#include "Components/BillboardComponent.h"
#include "Components/DrawSphereComponent.h"
#include "Components/SceneComponent.h"

ARetakeFreezeTagRelay::ARetakeFreezeTagRelay()
{
    PrimaryActorTick.bCanEverTick = false;
    SetReplicates(false);
    Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    RootComponent = Root;
#if WITH_EDITORONLY_DATA
    RadiusPreview = CreateEditorOnlyDefaultSubobject<UDrawSphereComponent>(TEXT("RadiusPreview"));
    if (RadiusPreview)
    {
        RadiusPreview->SetupAttachment(Root);
        RadiusPreview->SetSphereRadius(300.f);
        RadiusPreview->ShapeColor = FColor(80, 180, 255);
        RadiusPreview->SetHiddenInGame(true);
        RadiusPreview->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    }
    Sprite = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("Sprite"));
    if (Sprite) Sprite->SetupAttachment(Root);
#endif
}

void ARetakeFreezeTagRelay::BeginPlay()
{
    Super::BeginPlay();
    URetakeMapElementSubsystem::Register(*this);
}

void ARetakeFreezeTagRelay::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    URetakeMapElementSubsystem::Unregister(*this);
    Super::EndPlay(EndPlayReason);
}
