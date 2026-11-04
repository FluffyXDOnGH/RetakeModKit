// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "RetakeTraversalObstacleComponent.h"
#include "RetakeDestructibleTraversalComponent.generated.h"

/**
 * Opt-in solo traversal on a destructible placement. Author ordinary ledge splines on that placement; its round actor
 * reads them through its own component and admits traversal only while its collection remains intact at the authored pose.
 * No collision preview or activation graph is needed. The component is disabled by default.
 */
UCLASS(ClassGroup=(Retake),meta=(BlueprintSpawnableComponent))
class RETAKEDESTRUCTION_API URetakeDestructibleTraversalComponent : public URetakeTraversalObstacleComponent
{
    GENERATED_BODY()
public:
    URetakeDestructibleTraversalComponent();
    /** Runtime actor: copy the persistent placement's settings after its replicated reference resolves. */
    void InitializeFromPlacement();
    virtual bool IsUsable() const override;
    virtual bool GetBlockingComponents(TArray<UPrimitiveComponent*>& Result) const override;
    virtual void GetRecoveryBlockingComponents(TArray<UPrimitiveComponent*>& Result) const override;
    /** This contract is solo vault/climb only. */
    virtual FName GetBoostProfileId(FName Ledge) const override { return NAME_None; }

protected:
    virtual const AActor* GetLedgeOwner() const override;
    virtual void GetCollisionForValidation(FBox& Body,bool& bBlocksPawn,bool& bBlocksTraversable) const override;
    virtual void CollectIssues(TArray<FText>& Errors,TArray<FText>& Warnings) const override;
};

