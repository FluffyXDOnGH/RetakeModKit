// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeVisibilityGeometry.h"

void RetakeVisibilityGeometry::Corners(const FBox& Box, FVector (&Out)[8])
{
    for (int32 I=0; I<8; ++I)
        Out[I]=FVector(I&1 ? Box.Max.X:Box.Min.X,I&2 ? Box.Max.Y:Box.Min.Y,I&4 ? Box.Max.Z:Box.Min.Z);
}

namespace
{
bool Inside(const FRetakeSightPolygon& Polygon, const FVector& Point, double Margin)
{
    for (int32 I=0; I<Polygon.Vertices.Num(); ++I)
    {
        const FVector Edge=Polygon.Vertices[(I+1)%Polygon.Vertices.Num()]-Polygon.Vertices[I];
        if (FVector::DotProduct(FVector::CrossProduct(Edge,Point-Polygon.Vertices[I]),Polygon.Normal) < Margin*Edge.Size()) return false;
    }
    return true;
}
}

bool RetakeVisibilityGeometry::Occludes(const FRetakeSightPolygon& P,const FBox& Observer,const FBox& Subject)
{
    if (!Observer.IsValid || !Subject.IsValid || Observer.Min.ContainsNaN() || Observer.Max.ContainsNaN()
        || Subject.Min.ContainsNaN() || Subject.Max.ContainsNaN() || P.Vertices.Num()<3 || !P.Normal.IsNormalized()) return false;
    FVector A[8],B[8]; Corners(Observer,A); Corners(Subject,B);
    double DA[8],DB[8];
    for (int32 I=0; I<8; ++I) { DA[I]=FVector::DotProduct(A[I]-P.Vertices[0],P.Normal); DB[I]=FVector::DotProduct(B[I]-P.Vertices[0],P.Normal); }
    const bool Front=DA[0]>.1;
    if (!Front && !P.bTwoSided) return false;
    for (int32 I=0; I<8; ++I)
        if ((Front ? DA[I]<=.1 || DB[I]>=-.1 : DA[I]>=-.1 || DB[I]<=.1)) return false;
    // Cross-sections of the convex hull of both boxes are contained in the convex
    // hull of these 64 edge intersections. This is a proof for the entire beam,
    // unlike taking several blocked sight rays and assuming the gaps are blocked.
    for (int32 I=0; I<8; ++I) for (int32 J=0; J<8; ++J)
        if (!Inside(P,FMath::Lerp(A[I],B[J],DA[I]/(DA[I]-DB[J])),.1)) return false;
    return true;
}

bool RetakeVisibilityGeometry::SegmentPolygon(const FVector& Start,const FVector& End,const FRetakeSightPolygon& P,double& Distance)
{
    if (P.Vertices.Num()<3) return false;
    const double A=FVector::DotProduct(Start-P.Vertices[0],P.Normal),B=FVector::DotProduct(End-P.Vertices[0],P.Normal);
    if (FMath::Abs(A-B)<UE_DOUBLE_SMALL_NUMBER || A*B>0) return false;
    const double T=A/(A-B);
    if (T<0 || T>1 || !Inside(P,FMath::Lerp(Start,End,T),0)) return false;
    Distance=(End-Start).Size()*T;
    return true;
}
