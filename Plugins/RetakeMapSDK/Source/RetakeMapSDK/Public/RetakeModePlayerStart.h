// Copyright 2026 Codnyx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerStart.h"
#include "RetakeModePlayerStart.generated.h"

/** SDK contract (MKD-029): cooked maps store these numbers. Append only; never reorder or renumber. */
UENUM(BlueprintType)
enum class ERetakeModeStartRole : uint8
{
    Shared = 0,
    Team = 1,
    Attacker = 2,
    Defender = 3
};

/** Explicit map-authored start policy. OBJ starts belong to fixed map faction roles; players change faction at halftime.
 *  Freeze Tag uses Team starts whose TeamId is a physical spawn end (0/1); the mode swaps ends between rounds.
 *  Data only: the game decides which starts a mode accepts (RetakeModeStarts in the Retake module). */
UCLASS()
class RETAKEMAPSDK_API ARetakeModePlayerStart : public APlayerStart
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Spawn")
    ERetakeModeStartRole StartRole = ERetakeModeStartRole::Shared;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Spawn", meta=(ClampMin="-1"))
    int32 TeamId = INDEX_NONE;
};
