// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

namespace RetakeVisibilityCoverage
{
    using FPolygon = TArray<FVector2D,TInlineAllocator<16>>;
    /** CCW convex hull; non-finite, oversized or degenerate input is rejected. */
    RETAKEMAPSDK_API bool Hull(TConstArrayView<FVector2D> Points,FPolygon& Out);
    RETAKEMAPSDK_API double Area(const FPolygon& Polygon);
    RETAKEMAPSDK_API FBox2D Bounds(const FPolygon& Polygon);
    /** Convex intersection, sharing the caller's bounded clipping-work allowance. */
    RETAKEMAPSDK_API bool Intersect(const FPolygon& A,const FPolygon& B,FPolygon& Out,int32& Work);
    /** True only when polygon subtraction leaves no uncovered area. Never fills holes. */
    RETAKEMAPSDK_API bool Covers(const FPolygon& Required,TConstArrayView<FPolygon> Covers,int32& Work);
    RETAKEMAPSDK_API FPolygon Rectangle(const FBox2D& Box);
}
