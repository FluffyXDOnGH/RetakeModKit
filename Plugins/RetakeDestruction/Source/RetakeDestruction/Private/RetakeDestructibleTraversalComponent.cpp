// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeDestructibleTraversalComponent.h"
#include "RetakeDestructibleActor.h"
#include "RetakeDestructiblePlacement.h"
#include "RetakeDestructionDefinition.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"

URetakeDestructibleTraversalComponent::URetakeDestructibleTraversalComponent()
{ bTraversalEnabled=false; }

void URetakeDestructibleTraversalComponent::InitializeFromPlacement()
{
    const auto* Actor=Cast<ARetakeDestructibleActor>(GetOwner());
    const auto* Placement=Actor ? Actor->GetPlacement():nullptr;
    const auto* Source=IsValid(Placement) ? Placement->Traversal.Get():nullptr;
    bTraversalEnabled=Source && !Source->IsEditorOnly() && Source->bTraversalEnabled && !Placement->Door;
    if (Source)
    {
        MinimumWidth=Source->MinimumWidth;
        LedgeAccess=Source->LedgeAccess;
        OppositeLedges=Source->OppositeLedges;
    }
}

const AActor* URetakeDestructibleTraversalComponent::GetLedgeOwner() const
{
    if (const auto* Actor=Cast<ARetakeDestructibleActor>(GetOwner())) return Actor->GetPlacement();
    return GetOwner();
}

bool URetakeDestructibleTraversalComponent::IsUsable() const
{
    const auto* Actor=Cast<ARetakeDestructibleActor>(GetOwner());
    // The placement's editor-only preview never acts as a second gameplay obstacle.
    return Actor && Actor->CanTraverseIntact() && Super::IsUsable();
}

bool URetakeDestructibleTraversalComponent::GetBlockingComponents(TArray<UPrimitiveComponent*>& Result) const
{
    Result.Reset();
    if (!IsUsable()) return false;
    // The collection simulates even while anchored. CanTraverseIntact checks its actual root and destruction state,
    // rather than relaxing the ordinary obstacle contract for arbitrary simulated bodies.
    GetRecoveryBlockingComponents(Result);
    return !Result.IsEmpty();
}

void URetakeDestructibleTraversalComponent::GetRecoveryBlockingComponents(TArray<UPrimitiveComponent*>& Result) const
{
    Result.Reset();
    const auto* Actor=Cast<ARetakeDestructibleActor>(GetOwner());
    if (!IsValid(Actor)) return;
    for (UPrimitiveComponent* Component:{static_cast<UPrimitiveComponent*>(Actor->GetCollectionComponent()),
        static_cast<UPrimitiveComponent*>(Actor->GetFrameComponent())})
        if (IsValid(Component) && Component->IsQueryCollisionEnabled() && Component->GetCollisionResponseToChannel(ECC_Pawn)==ECR_Block)
            Result.Add(Component);
}

void URetakeDestructibleTraversalComponent::GetCollisionForValidation(FBox& Body,bool& bBlocksPawn,bool& bBlocksTraversable) const
{
    const auto* Placement=Cast<ARetakeDestructiblePlacement>(GetOwner());
    if (!Placement) { Super::GetCollisionForValidation(Body,bBlocksPawn,bBlocksTraversable);return; }
    Body=FBox(ForceInit);bBlocksPawn=bBlocksTraversable=false;
    if (!Placement->Definition || !Placement->Definition->Collection) return;
    // Only validation reads the collection preview; it has no collision and is stripped from cooked maps.
    TInlineComponentArray<UGeometryCollectionComponent*> Collections(Placement);
    for (const auto* Collection:Collections)
        if (Collection->GetRestCollection()==Placement->Definition->Collection)
        {
            Body+=Collection->CalcBounds(Collection->GetComponentTransform()).GetBox();
            bBlocksPawn=bBlocksTraversable=true;
        }
}

void URetakeDestructibleTraversalComponent::CollectIssues(TArray<FText>& Errors,TArray<FText>& Warnings) const
{
    // Existing destructibles have no ledges and remain valid with the opt-in disabled.
    if (!bTraversalEnabled) return;
    Super::CollectIssues(Errors,Warnings);
    const auto* Placement=Cast<ARetakeDestructiblePlacement>(GetOwner());
    if (!IsTemplate() && !Placement && !Cast<ARetakeDestructibleActor>(GetOwner()))
        Errors.Add(NSLOCTEXT("RetakeDestruction","TraversalOwner","Destructible traversal belongs to a Retake Destructible Placement."));
    if (Placement && Placement->Door)
        Errors.Add(NSLOCTEXT("RetakeDestruction","TraversalDoor","Destructible traversal supports stationary props, not door leaves."));
    for (const auto& [Ledge,Settings]:LedgeAccess)
        if (Settings.Access==ERetakeLedgeAccess::BoostOnly || Settings.Access==ERetakeLedgeAccess::Either)
            Errors.Add(NSLOCTEXT("RetakeDestruction","TraversalSolo","Destructible ledges support SoloOnly or Disabled access; boosts are not supported."));
}

