// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeDestructiblePlacement.h"
#include "RetakeDestructionDefinition.h"
#include "RetakeDestructibleTraversalComponent.h"
#include "RetakeMapElementSubsystem.h"
#include "RetakeDoor.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "Components/StaticMeshComponent.h"
#if WITH_EDITOR
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Misc/DataValidation.h"
#include "Misc/PackageName.h"
#endif

ARetakeDestructiblePlacement::ARetakeDestructiblePlacement()
{
    PrimaryActorTick.bCanEverTick=false;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
    Traversal=CreateDefaultSubobject<URetakeDestructibleTraversalComponent>(TEXT("Traversal"));
    Tags.Add(TEXT("Retake.Visibility.Ignore"));
#if WITH_EDITORONLY_DATA
    Preview=CreateEditorOnlyDefaultSubobject<UGeometryCollectionComponent>(TEXT("Preview"));
    FramePreview=CreateEditorOnlyDefaultSubobject<UStaticMeshComponent>(TEXT("FramePreview"));
    if (Preview) {Preview->SetupAttachment(RootComponent);Preview->SetSimulatePhysics(false);Preview->SetCollisionEnabled(ECollisionEnabled::NoCollision);Preview->SetCanEverAffectNavigation(false);}
    if (FramePreview) {FramePreview->SetupAttachment(RootComponent);FramePreview->SetCollisionEnabled(ECollisionEnabled::NoCollision);FramePreview->SetCanEverAffectNavigation(false);}
#endif
}
void ARetakeDestructiblePlacement::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
#if WITH_EDITORONLY_DATA
    if (Preview)
    {
        // The editor shows exactly what the game renders: the collection itself, intact. Construction owns every
        // preview setting, so levels saved with the former hidden intact-mesh preview are corrected on load.
        if (Preview->GetAttachParent()!=RootComponent) Preview->AttachToComponent(RootComponent,FAttachmentTransformRules::KeepRelativeTransform);
        Preview->bUseAttachParentBound=false;
        Preview->SetVisibility(true);
        Preview->SetRestCollection(Definition ? Definition->Collection.Get() : nullptr);
        Preview->EmptyOverrideMaterials();
        if (Definition) for (int32 I=0;I<Definition->Materials.Num();++I) if (Definition->Materials[I]) Preview->SetMaterial(I,Definition->Materials[I]);
        Preview->UpdateBounds();
    }
    if (FramePreview) FramePreview->SetStaticMesh(Definition ? Definition->FrameMesh.Get() : nullptr);
#endif
}
void ARetakeDestructiblePlacement::BeginPlay()
{
    Super::BeginPlay();
#if WITH_EDITORONLY_DATA
    if (Preview) Preview->DestroyComponent();
    if (FramePreview) FramePreview->DestroyComponent();
#endif
    URetakeMapElementSubsystem::Register(*this);
}
void ARetakeDestructiblePlacement::EndPlay(const EEndPlayReason::Type Reason)
{
    URetakeMapElementSubsystem::Unregister(*this);
    Super::EndPlay(Reason);
}
#if WITH_EDITOR
EDataValidationResult ARetakeDestructiblePlacement::IsDataValid(FDataValidationContext& Context) const
{
    EDataValidationResult Result=Super::IsDataValid(Context);
    auto Fail=[&](const FString& Text){Context.AddError(FText::FromString(Text));Result=EDataValidationResult::Invalid;};
    if (!Definition) {Fail(TEXT("Choose a destruction definition."));return Result;}
    // Built-in maps use /Game definitions and a mod uses its own: a definition from another root is not shipped with this map.
    const FName Root=FPackageName::GetPackageMountPoint(GetPackage()->GetName(),false);
    for (const UObject* Asset:{static_cast<const UObject*>(Definition.Get()),static_cast<const UObject*>(Definition->Collection.Get())})
        if (Asset && FPackageName::GetPackageMountPoint(Asset->GetPackage()->GetName(),false)!=Root)
            Fail(FString::Printf(TEXT("%s is outside this map's package root %s."),*Asset->GetPathName(),*Root.ToString()));
    if (Definition->IsDataValid(Context)==EDataValidationResult::Invalid) Result=EDataValidationResult::Invalid;
    if (Traversal && Traversal->IsDataValid(Context)==EDataValidationResult::Invalid) Result=EDataValidationResult::Invalid;
    if (Door)
    {
        // A door that starts locked has no door gameplay: it keeps its own solid leaf and is never destroyed.
        if (Door->bStartLocked)
            Fail(FString::Printf(TEXT("%s starts locked, so it keeps its own solid leaf and cannot be destroyed. Remove this leaf."),*Door->GetActorNameOrLabel()));
        // A door leaf replaces the door's own leaf mesh, so it must sit on that leaf (closed, as authored) and be its only one.
        const UStaticMeshComponent* Leaf=Door->DoorMesh;
        if (!Leaf || !Leaf->GetStaticMesh()) Fail(FString::Printf(TEXT("Door %s has no leaf mesh to replace."),*Door->GetActorNameOrLabel()));
        else if (!Leaf->Bounds.GetBox().ExpandBy(10.).IsInsideOrOn(GetActorLocation()))
            Fail(FString::Printf(TEXT("Place this door leaf on %s's closed leaf."),*Door->GetActorNameOrLabel()));
        if (const ULevel* Level=GetLevel()) for (const AActor* Actor:Level->Actors)
            if (const auto* Other=Cast<ARetakeDestructiblePlacement>(Actor); Other && Other!=this && Other->Door==Door)
            {Fail(FString::Printf(TEXT("%s is also the leaf of %s; a door has one leaf."),*Other->GetActorNameOrLabel(),*Door->GetActorNameOrLabel()));break;}
    }
    return Result;
}
#endif
