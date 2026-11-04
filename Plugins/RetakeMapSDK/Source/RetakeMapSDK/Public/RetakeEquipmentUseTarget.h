// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "RetakeEquipmentUseTarget.generated.h"

class APawn;
struct FHitResult;

/** Native target adapter. Creator content cannot supply arbitrary runtime code or Blueprint implementations. */
UINTERFACE(meta=(CannotImplementInterfaceInBlueprint))
class RETAKEMAPSDK_API URetakeEquipmentUseTarget : public UInterface
{
    GENERATED_BODY()
};

class RETAKEMAPSDK_API IRetakeEquipmentUseTarget
{
    GENERATED_BODY()
public:
    /** Called on authority after ownership, input, cadence and unobstructed-range checks.
     * Implementations own target-specific permissions and activation; false has no gameplay effect. */
    virtual bool TryActivateFromEquipment(APawn* User,FName EquipmentId,const FHitResult& Hit) { return false; }
};
