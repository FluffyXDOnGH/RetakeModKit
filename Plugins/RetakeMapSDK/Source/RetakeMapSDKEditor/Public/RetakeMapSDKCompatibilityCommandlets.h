// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "RetakeMapSDKCompatibilityCommandlets.generated.h"

/** Reflection snapshot of both SDK runtime modules. -Out=<json>; the creator runner compares it with the maintained baseline. */
UCLASS()
class URetakeMapSDKContractCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    URetakeMapSDKContractCommandlet();
    virtual int32 Main(const FString& Params) override;
};

/** Cook-time engine facts, independent of the SDK contract. -Out=<json> -Platform=<Windows|LinuxServer>. */
UCLASS()
class URetakeMapSDKRecordCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    URetakeMapSDKRecordCommandlet();
    virtual int32 Main(const FString& Params) override;
};


/** Strip a cooked development registry to allowed base packages and no asset tags. -In=<bin> -Base=<json> -Out=<bin>. */
UCLASS()
class URetakeMapSDKBaseRegistryCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    URetakeMapSDKBaseRegistryCommandlet();
    virtual int32 Main(const FString& Params) override;
};
