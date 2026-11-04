// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "RetakeWeaponPoseBakeCommandlet.generated.h"

/** -run=RetakeWeaponPoseBake -Request=<json>. JSON: owner, basis, output, specifications[]. No private game module required. */
UCLASS()
class URetakeWeaponPoseBakeCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    URetakeWeaponPoseBakeCommandlet();
    virtual int32 Main(const FString& Params) override;
};
