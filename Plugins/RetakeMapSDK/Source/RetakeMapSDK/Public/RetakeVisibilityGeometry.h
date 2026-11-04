// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "RetakeVisibilityGeometry.generated.h"

/** Convex opaque polygon. Vertices wind counterclockwise around Normal. Coordinates are world space. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeSightPolygon
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, Category="Retake|Network") TArray<FVector> Vertices;
    UPROPERTY(EditAnywhere, Category="Retake|Network") FVector Normal = FVector::UpVector;
    UPROPERTY(EditAnywhere, Category="Retake|Network") bool bTwoSided = false;
    UPROPERTY(EditAnywhere, Category="Retake|Network") int32 LOD=0;
};

/** Candidate index list for occlusion queries. Queries run per observer, subject and body part every server frame; keep them off the heap. */
using FRetakeVisibilityCandidates=TArray<int32,TInlineAllocator<128>>;

namespace RetakeVisibilityGeometry
{
    /** A whole-beam certificate: every segment between the two boxes crosses the polygon interior. */
    RETAKEMAPSDK_API bool Occludes(const FRetakeSightPolygon& Polygon, const FBox& Observer, const FBox& Subject);
    RETAKEMAPSDK_API void Corners(const FBox& Box, FVector (&Out)[8]);
    RETAKEMAPSDK_API bool SegmentPolygon(const FVector& Start, const FVector& End, const FRetakeSightPolygon& Polygon, double& Distance);
}

/** Proof polygons in one coordinate space with a bounding-volume tree over them. Shared by the visibility bakes and the prepared destruction occluders. */
struct RETAKEMAPSDK_API FRetakeSightPolygonIndex
{
    struct FNode { FBox Bounds=FBox(ForceInit); int32 Left=INDEX_NONE,Right=INDEX_NONE,First=0,Count=0; };
    TArray<FRetakeSightPolygon> LocalPolygons;
    TArray<FNode> Nodes;
    TArray<int32> PolygonOrder;
    void MergeCoplanarPolygons();
    void BuildIndex();
    /** Polygons whose bounds the segment crosses. */
    void FindCandidates(const FVector& Start,const FVector& End,FRetakeVisibilityCandidates& Out) const;
    /** Polygons whose bounds meet Bounds. */
    void FindCandidatesInBounds(const FBox& Bounds,FRetakeVisibilityCandidates& Out) const;
};
