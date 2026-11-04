// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeSpectatorOverview.h"
#include "RetakeMapElementSubsystem.h"
#include "Components/ArrowComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

ARetakeSpectatorOverview::ARetakeSpectatorOverview()
{
    PrimaryActorTick.bCanEverTick = false;
    bReplicates = false;
    View = CreateDefaultSubobject<USceneComponent>(TEXT("View"));
    SetRootComponent(View);
#if WITH_EDITORONLY_DATA
    static ConstructorHelpers::FObjectFinder<UStaticMesh> CameraMesh(TEXT("/Engine/EditorMeshes/MatineeCam_SM.MatineeCam_SM"));
    ViewProxy = CreateEditorOnlyDefaultSubobject<UStaticMeshComponent>(TEXT("ViewProxy"));
    if (ViewProxy)
    {
        ViewProxy->SetupAttachment(View);
        ViewProxy->SetStaticMesh(CameraMesh.Object);
        ViewProxy->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        ViewProxy->SetHiddenInGame(true);
        ViewProxy->SetCastShadow(false);
        ViewProxy->bIsEditorOnly = true;
    }
    ViewDirection = CreateEditorOnlyDefaultSubobject<UArrowComponent>(TEXT("ViewDirection"));
    if (ViewDirection)
    {
        ViewDirection->SetupAttachment(View);
        ViewDirection->ArrowSize = 2.f;
        ViewDirection->SetHiddenInGame(true);
    }
#endif
}

void ARetakeSpectatorOverview::BeginPlay()
{
    Super::BeginPlay();
    URetakeMapElementSubsystem::Register(*this);
}

void ARetakeSpectatorOverview::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    URetakeMapElementSubsystem::Unregister(*this);
    Super::EndPlay(EndPlayReason);
}
