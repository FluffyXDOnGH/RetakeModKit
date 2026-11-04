// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeVisibilityCoverage.h"

namespace RetakeVisibilityCoverage
{
namespace
{
constexpr int32 MaxVertices=64,MaxFragments=96;
double Cross(const FVector2D& A,const FVector2D& B) { return A.X*B.Y-A.Y*B.X; }
bool HasArea(const FPolygon& P)
{
    // Origin-relative double precision; only numerical zero-area slivers vanish.
    return P.Num()>=3 && Area(P)>1.e-9;
}
bool Clip(const FPolygon& Input,const FVector2D& A,const FVector2D& B,bool bInside,FPolygon& Out,int32& Work)
{
    Out.Reset();
    if (Input.IsEmpty()) return true;
    if ((Work-=Input.Num())<0) return false;
    const FVector2D Edge=B-A;
    FVector2D Previous=Input.Last();
    double Before=Cross(Edge,Previous-A)*(bInside ? 1.:-1.);
    for (const FVector2D& Current:Input)
    {
        const double After=Cross(Edge,Current-A)*(bInside ? 1.:-1.);
        if ((Before>=0)!=(After>=0)) Out.Add(Previous+(Current-Previous)*(Before/(Before-After)));
        if (After>=0) Out.Add(Current);
        Previous=Current; Before=After;
        if (Out.Num()>MaxVertices) { Work=-1; return false; }
    }
    return !Out.ContainsByPredicate([](const FVector2D& V){return !FMath::IsFinite(V.X) || !FMath::IsFinite(V.Y);});
}
}

double Area(const FPolygon& P)
{
    double Sum=0;
    for (int32 I=1;I+1<P.Num();++I) Sum+=Cross(P[I]-P[0],P[I+1]-P[0]);
    return FMath::Abs(Sum)*.5;
}
FBox2D Bounds(const FPolygon& P)
{ FBox2D Box(ForceInit); for (const FVector2D& V:P) Box+=V; return Box; }
FPolygon Rectangle(const FBox2D& B)
{ return {B.Min,FVector2D(B.Max.X,B.Min.Y),B.Max,FVector2D(B.Min.X,B.Max.Y)}; }
bool Hull(TConstArrayView<FVector2D> Input,FPolygon& Out)
{
    Out.Reset(); if (Input.Num()<3 || Input.Num()>256) return false;
    TArray<FVector2D,TInlineAllocator<256>> Points;
    for (const FVector2D& V:Input)
    { if (!FMath::IsFinite(V.X) || !FMath::IsFinite(V.Y)) return false; Points.Add(V); }
    Points.Sort([](const FVector2D& A,const FVector2D& B){return A.X==B.X ? A.Y<B.Y:A.X<B.X;});
    for (int32 I=Points.Num()-1;I>0;--I) if (Points[I]==Points[I-1]) Points.RemoveAt(I,1,EAllowShrinking::No);
    if (Points.Num()<3) return false;
    for (const FVector2D& V:Points)
    {
        while (Out.Num()>1 && Cross(Out.Last()-Out[Out.Num()-2],V-Out.Last())<=0) Out.Pop(EAllowShrinking::No);
        Out.Add(V);
    }
    const int32 Lower=Out.Num();
    for (int32 I=Points.Num()-2;I>=0;--I)
    {
        while (Out.Num()>Lower && Cross(Out.Last()-Out[Out.Num()-2],Points[I]-Out.Last())<=0) Out.Pop(EAllowShrinking::No);
        Out.Add(Points[I]);
    }
    Out.Pop(EAllowShrinking::No);
    return Out.Num()<=MaxVertices && HasArea(Out);
}
bool Intersect(const FPolygon& A,const FPolygon& B,FPolygon& Out,int32& Work)
{
    Out=A;
    for (int32 I=0;I<B.Num() && !Out.IsEmpty();++I)
    {
        FPolygon Next;
        if (!Clip(Out,B[I],B[(I+1)%B.Num()],true,Next,Work)) return false;
        Out=MoveTemp(Next);
    }
    return true;
}
bool Covers(const FPolygon& Required,TConstArrayView<FPolygon> Polygons,int32& Work)
{
    if (!HasArea(Required) || Polygons.IsEmpty() || Polygons.Num()>64) return false;
    TArray<FPolygon,TInlineAllocator<16>> Remaining; Remaining.Add(Required);
    for (const FPolygon& Cover:Polygons)
    {
        if (!HasArea(Cover)) continue;
        const FBox2D CoverBounds=Bounds(Cover);
        TArray<FPolygon,TInlineAllocator<16>> Next;
        for (const FPolygon& Fragment:Remaining)
        {
            if (!Bounds(Fragment).Intersect(CoverBounds))
            { Next.Add(Fragment); if (Next.Num()>MaxFragments) { Work=-1; return false; } continue; }
            FPolygon Inside=Fragment;
            for (int32 I=0;I<Cover.Num() && HasArea(Inside);++I)
            {
                FPolygon Outside,Clipped;
                if (!Clip(Inside,Cover[I],Cover[(I+1)%Cover.Num()],false,Outside,Work)
                    || !Clip(Inside,Cover[I],Cover[(I+1)%Cover.Num()],true,Clipped,Work)) return false;
                if (HasArea(Outside)) Next.Add(MoveTemp(Outside));
                if (Next.Num()>MaxFragments) { Work=-1; return false; }
                Inside=MoveTemp(Clipped);
            }
        }
        Remaining=MoveTemp(Next);
        if (Remaining.IsEmpty()) return true;
    }
    return false;
}
}
