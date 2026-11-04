// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeTraversalObstacleComponent.h"
#include "RetakeMapSDKIds.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SplineComponent.h"
#include "GameFramework/Actor.h"
#include "Misc/DataValidation.h"

namespace
{
    /** Traversal probes sweep this channel ("Traversable"), so solo ledges need collision that blocks it. */
    constexpr ECollisionChannel TraversableChannel=ECC_GameTraceChannel1;
    /** cos(30 degrees): an up vector closer to vertical than this does not point out of a wall. */
    constexpr float MaximumLedgeUpZ=.866f;
    /** The runtime boost rule: a boost ledge must be upright. */
    constexpr float MaximumBoostLedgeUpZ=.05f;

    FText AsText(FName Value) { return FText::AsCultureInvariant(Value.ToString()); }
}

URetakeTraversalObstacleComponent::URetakeTraversalObstacleComponent()
{ PrimaryComponentTick.bCanEverTick=false; }

const AActor* URetakeTraversalObstacleComponent::GetLedgeOwner() const { return GetOwner(); }

bool URetakeTraversalObstacleComponent::IsLedge(const USplineComponent& Spline) const
{
    const auto* Settings=LedgeAccess.Find(Spline.GetFName());
    return !Settings || Settings->Access!=ERetakeLedgeAccess::Disabled;
}
void URetakeTraversalObstacleComponent::GetLedges(TArray<USplineComponent*>& OutLedges) const
{
    OutLedges.Reset();
    if (const AActor* Owner=GetLedgeOwner())
    {
        TInlineComponentArray<USplineComponent*> Splines(Owner);
        for (USplineComponent* Spline:Splines) if (IsLedge(*Spline)) OutLedges.Add(Spline);
    }
}
bool URetakeTraversalObstacleComponent::IsUsable() const
{
    const AActor* Owner=GetOwner();
    if (!bTraversalEnabled || !IsValid(Owner) || Owner->IsActorBeingDestroyed() || !Owner->GetRootComponent()
        || !Owner->GetVelocity().IsNearlyZero(.01f)) return false;
    const AActor* LedgeOwner=GetLedgeOwner();
    if (!IsValid(LedgeOwner) || LedgeOwner->IsActorBeingDestroyed()) return false;
    TInlineComponentArray<USplineComponent*> Splines(LedgeOwner);
    return Splines.ContainsByPredicate([this](const USplineComponent* Spline) { return IsLedge(*Spline); });
}
FString URetakeTraversalObstacleComponent::DescribeAuthoredLedges() const
{
    TArray<USplineComponent*> Ledges;GetLedges(Ledges);
    FString Result=FString::Printf(TEXT("enabled=%d ledges=%d minimum_width=%.1f owner_speed=%.2f"),
        bTraversalEnabled,Ledges.Num(),MinimumWidth,GetOwner()?GetOwner()->GetVelocity().Size():0.);
    for (const USplineComponent* Ledge:Ledges)
        Result+=FString::Printf(TEXT(" [spline=%s length=%.1f boost_profile=%s]"),*Ledge->GetName(),Ledge->GetSplineLength(),
            *GetBoostProfileId(Ledge->GetFName()).ToString());
    return Result;
}
bool URetakeTraversalObstacleComponent::GetBlockingComponents(TArray<UPrimitiveComponent*>& Result) const
{
    Result.Reset();if (!IsUsable()) return false;
    TInlineComponentArray<UPrimitiveComponent*> Components(GetOwner());
    for (auto* Component:Components)
        if (Component->IsQueryCollisionEnabled() && Component->GetCollisionResponseToChannel(ECC_Pawn)==ECR_Block)
        {
            if (Component->IsSimulatingPhysics() || !Component->GetComponentVelocity().IsNearlyZero(.01f)) { Result.Reset();return false; }
            Result.Add(Component);
        }
    return !Result.IsEmpty();
}
bool URetakeTraversalObstacleComponent::QueryAuthoredLedges(FVector HitLocation,FVector ActorLocation,FRetakeAuthoredLedgeQuery& Result) const
{
    Result={};if (!IsUsable() || HitLocation.ContainsNaN() || ActorLocation.ContainsNaN()) return false;
    TInlineComponentArray<USplineComponent*> Splines(GetLedgeOwner());
    USplineComponent* Closest=nullptr;double Best=TNumericLimits<double>::Max();
    for (USplineComponent* Ledge:Splines)
    {
        if (!IsLedge(*Ledge)) continue;
        const FVector Point=Ledge->FindLocationClosestToWorldLocation(ActorLocation,ESplineCoordinateSpace::World);
        const FVector Normal=Ledge->FindUpVectorClosestToWorldLocation(ActorLocation,ESplineCoordinateSpace::World);
        // Match the sample's outward bias so corners select a consistent face.
        const double Distance=FVector::DistSquared(Point+Normal*10.f,ActorLocation);
        if (Distance<Best) { Best=Distance;Closest=Ledge; }
    }
    if (!Closest || Closest->GetSplineLength()<MinimumWidth) return false;
    const FVector Local=Closest->FindLocationClosestToWorldLocation(HitLocation,ESplineCoordinateSpace::Local);
    const float Distance=FMath::Clamp(Closest->GetDistanceAlongSplineAtLocation(Local,ESplineCoordinateSpace::Local),
        MinimumWidth*.5f,Closest->GetSplineLength()-MinimumWidth*.5f);
    const FTransform Front=Closest->GetTransformAtDistanceAlongSpline(Distance,ESplineCoordinateSpace::World,false);
    Result.Length=Closest->GetSplineLength();Result.Along=Distance;
    Result.Ledge=Closest->GetFName();Result.FrontLocation=Front.GetLocation();Result.FrontNormal=Front.GetRotation().GetAxisZ();
    if (const FName* BackName=OppositeLedges.Find(Result.Ledge))
        if (USplineComponent* const* Back=Splines.FindByPredicate([BackName](const USplineComponent* Spline) { return Spline->GetFName()==*BackName; }))
        {
            const FTransform Transform=(*Back)->FindTransformClosestToWorldLocation(Result.FrontLocation,ESplineCoordinateSpace::World,false);
            Result.bHasBack=true;Result.BackLocation=Transform.GetLocation();Result.BackNormal=Transform.GetRotation().GetAxisZ();
        }
    return !Result.FrontLocation.ContainsNaN() && Result.FrontNormal.IsNormalized();
}

bool URetakeTraversalObstacleComponent::AllowsSolo(FName Ledge) const
{
    const auto* Settings=LedgeAccess.Find(Ledge);
    return bTraversalEnabled && (!Settings || Settings->Access==ERetakeLedgeAccess::SoloOnly || Settings->Access==ERetakeLedgeAccess::Either);
}
FName URetakeTraversalObstacleComponent::GetBoostProfileId(FName Ledge) const
{
    const auto* Settings=LedgeAccess.Find(Ledge);
    return bTraversalEnabled && Settings && (Settings->Access==ERetakeLedgeAccess::BoostOnly || Settings->Access==ERetakeLedgeAccess::Either)
        ? Settings->BoostProfileId:NAME_None;
}

void URetakeTraversalObstacleComponent::GetRecoveryBlockingComponents(TArray<UPrimitiveComponent*>& Result) const
{
    Result.Reset();
    if (!IsValid(GetOwner())) return;
    TInlineComponentArray<UPrimitiveComponent*> Components(GetOwner());
    for (auto* Component:Components)
        if (Component->IsQueryCollisionEnabled() && Component->GetCollisionResponseToChannel(ECC_Pawn)==ECR_Block)
            Result.Add(Component);
}

void URetakeTraversalObstacleComponent::GetCollisionForValidation(FBox& Body,bool& bBlocksPawn,bool& bBlocksTraversable) const
{
    Body=FBox(ForceInit);bBlocksPawn=bBlocksTraversable=false;
    TInlineComponentArray<UPrimitiveComponent*> Primitives(GetOwner());
    for (const UPrimitiveComponent* Primitive:Primitives)
    {
        if (Primitive->IsEditorOnly() || !Primitive->IsQueryCollisionEnabled()) continue;
        if (Primitive->GetCollisionResponseToChannel(ECC_Pawn)==ECR_Block) { bBlocksPawn=true;Body+=Primitive->Bounds.GetBox(); }
        bBlocksTraversable|=Primitive->GetCollisionResponseToChannel(TraversableChannel)==ECR_Block;
    }
}

void URetakeTraversalObstacleComponent::CollectIssues(TArray<FText>& Errors,TArray<FText>& Warnings) const
{
    if (!FMath::IsFinite(MinimumWidth) || MinimumWidth<1.f) Errors.Add(NSLOCTEXT("RetakeMapSDK","LedgeMinimumWidth","Minimum Width must be at least 1 cm."));
    for (const auto& [Ledge,Settings]:LedgeAccess)
        if ((Settings.Access==ERetakeLedgeAccess::BoostOnly || Settings.Access==ERetakeLedgeAccess::Either)
            && !RetakeMapSDKIds::BoostProfiles().Contains(Settings.BoostProfileId))
            Errors.Add(FText::Format(NSLOCTEXT("RetakeMapSDK","LedgeBoostProfile","Boost ledge {0} needs a known Boost Profile Id (it has '{1}')."),
                AsText(Ledge),AsText(Settings.BoostProfileId)));
    // Blueprint templates are checked where they are placed: the ledges are the placed actor's splines.
    const AActor* Owner=GetOwner();
    if (IsTemplate() || !Owner) return;
    TInlineComponentArray<URetakeTraversalObstacleComponent*> Obstacles(Owner);
    if (Obstacles.Num()>1 && Obstacles[0]!=this) Errors.Add(NSLOCTEXT("RetakeMapSDK","LedgeComponents","Use one Retake Traversal Obstacle component per actor."));
    // Editor-only components are stripped at cook, so the game would see no obstacle, ledge or wall.
    if (IsEditorOnly()) Errors.Add(NSLOCTEXT("RetakeMapSDK","LedgeEditorOnly","The traversal obstacle component must not be editor-only."));
    FBox Body(ForceInit);
    bool bBlocksPawn=false,bBlocksTraversable=false;
    GetCollisionForValidation(Body,bBlocksPawn,bBlocksTraversable);
    TInlineComponentArray<USplineComponent*> Splines(GetLedgeOwner());
    if (Splines.IsEmpty()) Errors.Add(NSLOCTEXT("RetakeMapSDK","LedgeNoSplines","Add a spline component along each ledge: every spline on the actor is a ledge."));
    const auto IsSpline=[&Splines](FName Spline) { return Splines.ContainsByPredicate([Spline](const USplineComponent* S) { return S->GetFName()==Spline; }); };
    for (const auto& Pair:LedgeAccess)
        if (!IsSpline(Pair.Key)) Errors.Add(FText::Format(NSLOCTEXT("RetakeMapSDK","LedgeAccessName","Ledge Access names {0}, which is not a spline on this actor."),AsText(Pair.Key)));
    for (const auto& Pair:OppositeLedges)
        if (!IsSpline(Pair.Key) || !IsSpline(Pair.Value))
            Errors.Add(FText::Format(NSLOCTEXT("RetakeMapSDK","LedgeOppositeName","Opposite Ledges {0} -> {1} must name two splines on this actor."),AsText(Pair.Key),AsText(Pair.Value)));
    bool bSolo=false;
    for (const USplineComponent* Spline:Splines)
    {
        if (!IsLedge(*Spline)) continue;
        const FName Ledge=Spline->GetFName();
        const ERetakeLedgeAccess Access=LedgeAccess.Contains(Ledge)?LedgeAccess[Ledge].Access:ERetakeLedgeAccess::SoloOnly;
        bSolo|=Access==ERetakeLedgeAccess::SoloOnly || Access==ERetakeLedgeAccess::Either;
        const bool bBoost=Access==ERetakeLedgeAccess::BoostOnly || Access==ERetakeLedgeAccess::Either;
        if (Spline->IsEditorOnly())
            Errors.Add(FText::Format(NSLOCTEXT("RetakeMapSDK","LedgeSplineEditorOnly","Ledge {0} must not be editor-only."),AsText(Ledge)));
        float UpZ=0.f;
        bool bInward=false;
        for (int32 Point=0;Point<Spline->GetNumberOfSplinePoints();++Point)
        {
            const FVector Up=Spline->GetUpVectorAtSplinePoint(Point,ESplineCoordinateSpace::World);
            UpZ=FMath::Max(UpZ,FMath::Abs(Up.Z));
            // Out of the wall: from the middle of the blocking body towards the edge, seen from above.
            const FVector Outward=(Spline->GetLocationAtSplinePoint(Point,ESplineCoordinateSpace::World)-Body.GetCenter()).GetSafeNormal2D();
            bInward|=Body.IsValid && FVector::DotProduct(Up.GetSafeNormal2D(),Outward)<0.;
        }
        if (bInward)
            Errors.Add(FText::Format(NSLOCTEXT("RetakeMapSDK","LedgeInward","Ledge {0}'s up vector points into the obstacle; it must point out of the wall."),AsText(Ledge)));
        else if (UpZ>MaximumLedgeUpZ)
            Errors.Add(FText::Format(NSLOCTEXT("RetakeMapSDK","LedgeUpVector","Ledge {0}'s up vector must point out of the wall, away from the obstacle; it points within 30 degrees of vertical."),AsText(Ledge)));
        else if (bBoost && UpZ>MaximumBoostLedgeUpZ)
            Errors.Add(FText::Format(NSLOCTEXT("RetakeMapSDK","LedgeBoostUpright","Boost ledge {0} must be upright: its up vector must be level (within 3 degrees)."),AsText(Ledge)));
        if (Spline->GetSplineLength()<MinimumWidth)
            Warnings.Add(FText::Format(NSLOCTEXT("RetakeMapSDK","LedgeShort","Ledge {0} is shorter than Minimum Width ({1} cm), so it cannot be used."),AsText(Ledge),FText::AsNumber(MinimumWidth)));
    }
    if (!bBlocksPawn) Errors.Add(NSLOCTEXT("RetakeMapSDK","LedgePawnCollision","The actor needs collision that blocks Pawn."));
    if (bSolo && !bBlocksTraversable)
        Errors.Add(NSLOCTEXT("RetakeMapSDK","LedgeTraversable","Solo ledges need collision that blocks the Traversable channel (collision preset TraversalObjectPreset)."));
}
FText URetakeTraversalObstacleComponent::GetConfigurationError() const
{
    TArray<FText> Errors,Warnings;CollectIssues(Errors,Warnings);
    return Errors.IsEmpty()?FText::GetEmpty():Errors[0];
}

#if WITH_EDITOR
EDataValidationResult URetakeTraversalObstacleComponent::IsDataValid(FDataValidationContext& Context) const
{
    EDataValidationResult Result=Super::IsDataValid(Context);
    TArray<FText> Errors,Warnings;CollectIssues(Errors,Warnings);
    for (const FText& Error:Errors) Context.AddError(Error);
    for (const FText& Warning:Warnings) Context.AddWarning(Warning);
    if (!Errors.IsEmpty()) Result=EDataValidationResult::Invalid;
    return Result;
}
#endif
