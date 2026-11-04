// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "RetakePieceMask.generated.h"

/** One bit per geometry collection transform. Compact enough to replicate whole with the destructible. */
USTRUCT()
struct RETAKEDESTRUCTION_API FRetakePieceMask
{
    GENERATED_BODY()

    void Reset(int32 NumPieces) { Bits.Init(0,(NumPieces+7)/8); }
    bool Contains(int32 Index) const { return Index>=0 && Bits.IsValidIndex(Index/8) && (Bits[Index/8]&(1<<(Index%8)))!=0; }
    /** Returns true when the bit was newly set. Out-of-range indices are ignored. */
    bool Add(int32 Index)
    {
        if (Index<0 || !Bits.IsValidIndex(Index/8) || Contains(Index)) return false;
        Bits[Index/8]|=1<<(Index%8);
        return true;
    }
    bool IsEmpty() const { return !Bits.ContainsByPredicate([](uint8 Byte){ return Byte!=0; }); }

private:
    UPROPERTY() TArray<uint8> Bits;
};
