// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "RetakeMapElementPushable.generated.h"

UINTERFACE(MinimalAPI, meta=(CannotImplementInterfaceInBlueprint))
class URetakeMapElementPushable : public UInterface
{
	GENERATED_BODY()
};

/**
 * A pawn that moving SDK map elements may push (RetakeMapSDK seam, MKD-029). The element decides who and when: the server pushes
 * every overlapping pawn, a client only its locally controlled one, never during replay playback. The pawn decides how it moves.
 * Pawns that do not implement this are not pushed. Game thread.
 */
class RETAKEMAPSDK_API IRetakeMapElementPushable
{
	GENERATED_BODY()
public:
	/** Direction is unit length; Speed in cm/s is the minimum speed the pawn should have along Direction. */
	virtual void ApplyMapElementPush(const AActor& Source, const FVector& Direction, float Speed) = 0;
};
