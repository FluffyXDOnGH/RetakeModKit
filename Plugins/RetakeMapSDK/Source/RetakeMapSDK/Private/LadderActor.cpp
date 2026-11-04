// Copyright 2026 Codnyx. All Rights Reserved.
#include "LadderActor.h"
#include "RetakeMapElementSubsystem.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/ArrowComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "DrawDebugHelpers.h"
#include "Net/UnrealNetwork.h"
#include "Misc/DataValidation.h"

ALadderActor::ALadderActor()
{
    bReplicates = true; SetReplicateMovement(false); PrimaryActorTick.bCanEverTick = true;
    LadderRoot = CreateDefaultSubobject<USceneComponent>(TEXT("LadderRoot")); SetRootComponent(LadderRoot);
    CompleteMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("CompleteMesh"));
    Rungs = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Rungs"));
    LeftRail = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("LeftRail"));
    RightRail = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("RightRail"));
    for (UStaticMeshComponent* Comp : { CompleteMesh.Get(), static_cast<UStaticMeshComponent*>(Rungs.Get()), LeftRail.Get(), RightRail.Get() })
    {
        Comp->SetupAttachment(LadderRoot); Comp->SetCollisionProfileName(TEXT("BlockAll"));
        Comp->SetCanEverAffectNavigation(false);
    }
    InteractionVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("InteractionVolume"));
    InteractionVolume->SetupAttachment(LadderRoot); InteractionVolume->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    InteractionVolume->SetCollisionResponseToAllChannels(ECR_Ignore);
    InteractionVolume->SetCollisionResponseToChannel(ECC_Pawn,ECR_Overlap);
    InteractionVolume->SetGenerateOverlapEvents(false);
    BottomAnchor = CreateDefaultSubobject<UArrowComponent>(TEXT("BottomExit")); BottomAnchor->SetupAttachment(LadderRoot);
    TopAnchor = CreateDefaultSubobject<UArrowComponent>(TEXT("TopExit")); TopAnchor->SetupAttachment(LadderRoot);
}
void ALadderActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME_CONDITION(ALadderActor, Geometry, COND_InitialOnly);
}
void ALadderActor::OnConstruction(const FTransform& Transform) { Super::OnConstruction(Transform); RebuildGeometry(); }
void ALadderActor::OnRep_Configuration() { RebuildGeometry(); }
void ALadderActor::BeginPlay()
{
    Super::BeginPlay(); InitialTransform = GetActorTransform(); SetActorTickEnabled(false);
    URetakeMapElementSubsystem::Register(*this);
}
void ALadderActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    URetakeMapElementSubsystem::Unregister(*this);
    Super::EndPlay(EndPlayReason);
}

// Correct arbitrary asset pivots before applying the designer's additional offset.
static FTransform FitMesh(UStaticMesh* Mesh, const FTransform& Offset, FVector Target, int32 FitAxis, float Length, bool bBottom)
{
    if (!Mesh) return FTransform::Identity;
    FTransform RotationOnly(Offset.GetRotation());
    const FBox Bounds = Mesh->GetBoundingBox().TransformBy(RotationOnly);
    FVector Scale = Offset.GetScale3D();
    const float Size = Bounds.GetSize()[FitAxis];
    // These source pieces use axis-aligned 90-degree rotations; map desired scaling back to mesh space.
    FVector LadderScale(1); LadderScale[FitAxis] = Size > UE_SMALL_NUMBER ? Length/Size : 1;
    const FVector SourceScale = RotationOnly.InverseTransformVectorNoScale(LadderScale).GetAbs();
    Scale *= SourceScale;
    FTransform Result(Offset.GetRotation(), FVector::ZeroVector, Scale);
    FBox Scaled = Mesh->GetBoundingBox().TransformBy(Result);
    FVector Center = Scaled.GetCenter(); if (bBottom) Center.Z = Scaled.Min.Z;
    Result.SetTranslation(Target-Center+Offset.GetTranslation()); return Result;
}
void ALadderActor::RebuildGeometry()
{
    if (HasActorBegunPlay()) return; // Geometry is immutable for a network session.
    const bool bWhole = Geometry.Type == ELadderGeometry::CompleteMesh;
    CompleteMesh->SetStaticMesh(Geometry.CompleteMesh);
    CompleteMesh->SetVisibility(bWhole); CompleteMesh->SetCollisionEnabled(bWhole ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
    CompleteMesh->SetRelativeTransform(FitMesh(Geometry.CompleteMesh, Geometry.CompleteMeshOffset, FVector::ZeroVector, 2, Geometry.Height, true));
    Rungs->ClearInstances(); Rungs->SetStaticMesh(Geometry.RungMesh); Rungs->SetVisibility(!bWhole);
    Rungs->SetCollisionEnabled(!bWhole ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
    if (!bWhole && Geometry.RungMesh) for (float Height : GetRungHeights())
        Rungs->AddInstance(FitMesh(Geometry.RungMesh, Geometry.RungMeshOffset, FVector(0,0,Height), 1, Geometry.Width, false));
    UStaticMeshComponent* Rails[] = { LeftRail, RightRail };
    for (int32 I=0; I<2; ++I)
    {
        UStaticMesh* Mesh = I ? Geometry.RightRailMesh.Get() : Geometry.LeftRailMesh.Get();
        if (!Mesh) Mesh = Geometry.LeftRailMesh;
        Rails[I]->SetStaticMesh(Mesh); Rails[I]->SetVisibility(!bWhole);
        Rails[I]->SetCollisionEnabled(!bWhole ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
        Rails[I]->SetRelativeTransform(FitMesh(Mesh, I ? Geometry.RightRailOffset : Geometry.LeftRailOffset,
            FVector(0, (I ? 1 : -1)*Geometry.Width*.5, -Geometry.RailExtensionBottom), 2,
            Geometry.Height+Geometry.RailExtensionBottom+Geometry.RailExtensionTop, true));
    }
    InteractionVolume->SetRelativeLocation(FVector(Geometry.GrabReach*.5,0,Geometry.Height*.5));
    InteractionVolume->SetBoxExtent(FVector(Geometry.GrabReach*.5,Geometry.Width*.5+35,Geometry.Height*.5+Geometry.EndInteractionHeight));
    // Exit offsets describe a level landing, not an offset tilted into the floor.
    BottomAnchor->SetRelativeLocation(GetActorTransform().InverseTransformPosition(ExitPosition(false,0)-FVector(0,0,2)));
    TopAnchor->SetRelativeLocation(GetActorTransform().InverseTransformPosition(ExitPosition(true,0)-FVector(0,0,2)));
    ConfigurationError=GetConfigurationError();
}
TArray<float> ALadderActor::GetRungHeights() const
{
    TArray<float> Values;
    if (Geometry.Type == ELadderGeometry::CompleteMesh)
    {
        for (float Value : Geometry.CompleteMeshRungs) Values.Add(Value*Geometry.Height/FMath::Max(1.f,Geometry.ReferenceHeight));
    }
    else if (Geometry.RungSpacing >= 1 && FMath::IsFinite(Geometry.Height))
    {
        for (float V=FMath::Max(0.f,Geometry.FirstRungOffset); V<=Geometry.Height-Geometry.LastRungOffset && Values.Num()<1024; V+=Geometry.RungSpacing) Values.Add(V);
    }
    Values.Sort(); return Values;
}
float ALadderActor::StepSpacing() const
{
    const auto Values = GetRungHeights();
    return Values.Num()>1 ? (Values.Last()-Values[0])/(Values.Num()-1) : Geometry.RungSpacing;
}
FVector ALadderActor::GetGripPosition(int32 Rung, float Side, FVector Offset) const
{
    const auto Heights = GetRungHeights();
    if (!Heights.IsValidIndex(Rung)) return GetActorLocation();
    return GetActorTransform().TransformPosition(FVector(0,Side,Heights[Rung])+Offset);
}
FVector ALadderActor::BodyPosition(float Distance, float CapsuleHalfHeight) const
{
    return GetActorLocation()+GetActorUpVector()*Distance+GetActorForwardVector()*Geometry.StandOff+FVector::UpVector*CapsuleHalfHeight;
}
float ALadderActor::DistanceAtWorldHeight(float WorldZ, float HalfHeight) const
{
    return (WorldZ-BodyPosition(0,HalfHeight).Z)/FMath::Max(.1,GetActorUpVector().Z);
}
float ALadderActor::ProjectDistance(FVector Location, float HalfHeight) const { return FVector::DotProduct(Location-BodyPosition(0,HalfHeight),GetActorUpVector()); }
FVector ALadderActor::ExitPosition(bool bTop, float HalfHeight) const
{
    const FVector End=GetActorLocation()+(bTop ? GetActorUpVector()*Geometry.Height : FVector::ZeroVector);
    const FRotator LandingFrame(0,GetActorRotation().Yaw,0);
    return End+LandingFrame.RotateVector(bTop ? Geometry.TopExitOffset : Geometry.BottomExitOffset)+FVector::UpVector*(HalfHeight+2);
}
FRotator ALadderActor::FacingRotation() const { return FRotator(0,(-GetActorForwardVector()).Rotation().Yaw,0); }
FText ALadderActor::GetConfigurationError() const
{
    if (!(FMath::IsFinite(Geometry.ClimbSpeed) && Geometry.ClimbSpeed>0 && FMath::IsFinite(Geometry.DescentSpeed) && Geometry.DescentSpeed>0))
        return FText::FromString(TEXT("Ascent and descent speeds must be positive and finite."));
    if (!(Geometry.Height>=100 && Geometry.Height<=20000 && Geometry.Width>=30 && Geometry.Width<=100))
        return FText::FromString(TEXT("Height must be 100..20000 cm and width 30..100 cm."));
    if (!GetActorScale3D().Equals(FVector::OneVector,.001) || GetActorUpVector().Z<.707)
        return FText::FromString(TEXT("Use unit Actor scale and a straight incline of at most 45 degrees. Set dimensions through Geometry."));
    if (HasActorBegunPlay() && !GetActorTransform().Equals(InitialTransform,.01)) return FText::FromString(TEXT("Moving ladders are not supported."));
    const auto Heights=GetRungHeights();
    if (Heights.Num()<2) return FText::FromString(TEXT("At least two calibrated rungs are required."));
    if (Geometry.Type==ELadderGeometry::Generated && Geometry.RungSpacing>0
        && (Geometry.Height-Geometry.LastRungOffset-Geometry.FirstRungOffset)/Geometry.RungSpacing>=1024)
        return FText::FromString(TEXT("More than 1024 rungs requested. Reduce height or increase spacing."));
    constexpr float Minimum=MinimumRungGap, Maximum=MaximumRungGap;
    for (int32 I=0;I<Heights.Num();++I)
    {
        if (!FMath::IsFinite(Heights[I]) || Heights[I]<0 || Heights[I]>Geometry.Height)
            return FText::FromString(TEXT("Rung positions must be finite and inside the ladder height."));
        if (I>0)
        {
            const float Gap=Heights[I]-Heights[I-1];
            if (Gap>Maximum+UE_KINDA_SMALL_NUMBER) return FText::FromString(FString::Printf(TEXT("Unreachable rung gap: rungs %d to %d are %.2f cm apart. Retake ladders support at most %.2f cm. Reduce spacing."),I,I+1,Gap,Maximum));
            if (Gap<Minimum-UE_KINDA_SMALL_NUMBER) return FText::FromString(FString::Printf(TEXT("Rungs %d to %d are %.2f cm apart; Retake ladders require at least %.2f cm."),I,I+1,Gap,Minimum));
        }
    }
    return FText::GetEmpty();
}
bool ALadderActor::IsUsable() const { return GetConfigurationError().IsEmpty(); }
float ALadderActor::GetClimbPhaseAtDistance(float Distance) const
{
    const auto Heights=GetRungHeights(); if (Heights.Num()<2) return 0;
    for (int32 I=1;I<Heights.Num();++I) if (Distance<=Heights[I]) return (I-1)+(Distance-Heights[I-1])/FMath::Max(1.f,Heights[I]-Heights[I-1]);
    return (Heights.Num()-1)+(Distance-Heights.Last())/FMath::Max(1.f,Heights.Last()-Heights[Heights.Num()-2]);
}
void ALadderActor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
#if WITH_EDITOR
    if (GetWorld() && !GetWorld()->IsGameWorld() && IsSelected() && bShowPreview)
    {
        const FColor GripColor=IsUsable() ? FColor::Cyan : FColor::Red;
        for (float H : GetRungHeights())
            DrawDebugLine(GetWorld(),GetActorTransform().TransformPosition(FVector(0,-Geometry.Width*.5,H)),GetActorTransform().TransformPosition(FVector(0,Geometry.Width*.5,H)),GripColor,false,0,0,2);
        DrawDebugLine(GetWorld(),BodyPosition(0,88),BodyPosition(Geometry.Height,88),FColor::Yellow,false,0,0,2);
        for (bool bTop : {false,true}) DrawDebugCapsule(GetWorld(),ExitPosition(bTop,88),88,34,FQuat::Identity,FColor::Green,false,0,0,1);
    }
#endif
}
#if WITH_EDITOR
EDataValidationResult ALadderActor::IsDataValid(FDataValidationContext& Context) const
{
    const EDataValidationResult Base = Super::IsDataValid(Context);
    const FText Error=GetConfigurationError();
    if (!Error.IsEmpty()) { Context.AddError(Error); return EDataValidationResult::Invalid; }
    return Base == EDataValidationResult::Invalid ? Base : EDataValidationResult::Valid;
}
#endif
