// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeVisibilityGeometry.h"

namespace
{
struct FDirectedEdge
{
    FVector A,B;
    int32 LOD=0;
    bool operator==(const FDirectedEdge& Other) const { return A==Other.A && B==Other.B && LOD==Other.LOD; }
    friend uint32 GetTypeHash(const FDirectedEdge& Edge) { return HashCombineFast(HashCombineFast(GetTypeHash(Edge.A),GetTypeHash(Edge.B)),::GetTypeHash(Edge.LOD)); }
};
}

void FRetakeSightPolygonIndex::MergeCoplanarPolygons()
{
    TMap<FDirectedEdge,int32> Edges;
    TArray<int32> Pending;
    for (int32 I=0;I<LocalPolygons.Num();++I) Pending.Add(I);
    while (!Pending.IsEmpty())
    {
        const int32 Index=Pending.Pop(EAllowShrinking::No);
        auto& A=LocalPolygons[Index]; if (A.Vertices.IsEmpty()) continue;
        for (int32 EdgeIndex=0;EdgeIndex<A.Vertices.Num();++EdgeIndex)
        {
            const FVector X=A.Vertices[EdgeIndex],Y=A.Vertices[(EdgeIndex+1)%A.Vertices.Num()];
            const int32* Found=Edges.Find(FDirectedEdge{Y,X,A.LOD});
            if (!Found || *Found==Index || LocalPolygons[*Found].Vertices.IsEmpty())
            { Edges.Add(FDirectedEdge{X,Y,A.LOD},Index); continue; }
            const int32 Other=*Found; auto& B=LocalPolygons[Other];
            if (A.bTwoSided!=B.bTwoSided || !A.Normal.Equals(B.Normal,1.e-9)) continue;
            bool Coplanar=true;
            for (const FVector& V:B.Vertices) if (FMath::Abs(FVector::DotProduct(V-A.Vertices[0],A.Normal))>1.e-7) { Coplanar=false; break; }
            if (!Coplanar) continue;
            // A merged pair can share a chain of edges, not just the edge that
            // found the neighbor. Remove that complete chain from the union.
            TSet<FDirectedEdge> PairEdges;
            for (const auto* Polygon:{&A,&B})
                for (int32 J=0;J<Polygon->Vertices.Num();++J)
                    PairEdges.Add(FDirectedEdge{Polygon->Vertices[J],Polygon->Vertices[(J+1)%Polygon->Vertices.Num()],A.LOD});
            TMap<FVector,FVector> Boundary;
            bool Unique=true;
            for (const auto* Polygon:{&A,&B})
                for (int32 J=0;J<Polygon->Vertices.Num();++J)
                {
                    const FVector From=Polygon->Vertices[J],To=Polygon->Vertices[(J+1)%Polygon->Vertices.Num()];
                    if (PairEdges.Contains(FDirectedEdge{To,From,A.LOD})) continue;
                    if (const FVector* Next=Boundary.Find(From);Next && *Next!=To) Unique=false;
                    Boundary.Add(From,To);
                }
            if (!Unique || Boundary.Num()<3) continue;
            TArray<FVector> Merged;
            const FVector First=Boundary.CreateConstIterator().Key(); FVector Current=First;
            while (const FVector* Next=Boundary.Find(Current))
            {
                if (Merged.Contains(Current)) break;
                Merged.Add(Current); Current=*Next;
            }
            if (Current!=First || Merged.Num()!=Boundary.Num()) continue;
            bool Convex=true;
            for (int32 J=0;J<Merged.Num();++J)
                if (FVector::DotProduct(FVector::CrossProduct(Merged[(J+1)%Merged.Num()]-Merged[J],Merged[(J+2)%Merged.Num()]-Merged[(J+1)%Merged.Num()]),A.Normal)<-1.e-8)
                { Convex=false; break; }
            if (!Convex) continue;
            A.Vertices=MoveTemp(Merged); B.Vertices.Reset(); Pending.Add(Index); break;
        }
    }
    LocalPolygons.RemoveAll([](const FRetakeSightPolygon& P){return P.Vertices.IsEmpty();});
}

void FRetakeSightPolygonIndex::BuildIndex()
{
    Nodes.Reset(); PolygonOrder.Reset();
    TArray<FBox> Bounds; Bounds.Reserve(LocalPolygons.Num());
    for (int32 I=0;I<LocalPolygons.Num();++I)
    { FBox Box(ForceInit); for (const FVector& V:LocalPolygons[I].Vertices) Box+=V; Bounds.Add(Box.ExpandBy(.1)); PolygonOrder.Add(I); }
    TFunction<int32(int32,int32)> Split=[&](int32 First,int32 Count)
    {
        const int32 Index=Nodes.AddDefaulted(); FBox Box(ForceInit);
        for (int32 I=First;I<First+Count;++I) Box+=Bounds[PolygonOrder[I]];
        Nodes[Index].Bounds=Box; Nodes[Index].First=First; Nodes[Index].Count=Count;
        if (Count>16)
        {
            const FVector Extent=Box.GetExtent(); const int32 Axis=Extent.X>Extent.Y ? (Extent.X>Extent.Z ? 0:2):(Extent.Y>Extent.Z ? 1:2);
            TArrayView<int32>(PolygonOrder.GetData()+First,Count).Sort([&](int32 A,int32 B){return Bounds[A].GetCenter()[Axis]<Bounds[B].GetCenter()[Axis];});
            const int32 Left=Split(First,Count/2),Right=Split(First+Count/2,Count-Count/2);
            Nodes[Index].Left=Left; Nodes[Index].Right=Right; Nodes[Index].Count=0;
        }
        return Index;
    };
    if (!PolygonOrder.IsEmpty()) Split(0,PolygonOrder.Num());
}
void FRetakeSightPolygonIndex::FindCandidates(const FVector& Start,const FVector& End,FRetakeVisibilityCandidates& Out) const
{
    Out.Reset(); if (Nodes.IsEmpty()) return;
    TArray<int32,TInlineAllocator<64>> Stack; Stack.Add(0);
    while (!Stack.IsEmpty())
    {
        const FNode& Node=Nodes[Stack.Pop(EAllowShrinking::No)];
        if (!FMath::LineBoxIntersection(Node.Bounds,Start,End,End-Start)) continue;
        if (Node.Left!=INDEX_NONE) { Stack.Add(Node.Left); Stack.Add(Node.Right); }
        else for (int32 I=Node.First;I<Node.First+Node.Count;++I) Out.Add(PolygonOrder[I]);
    }
}
void FRetakeSightPolygonIndex::FindCandidatesInBounds(const FBox& Bounds,FRetakeVisibilityCandidates& Out) const
{
    Out.Reset(); if (Nodes.IsEmpty()) return;
    TArray<int32,TInlineAllocator<64>> Stack; Stack.Add(0);
    while (!Stack.IsEmpty())
    {
        const FNode& Node=Nodes[Stack.Pop(EAllowShrinking::No)];
        if (!Node.Bounds.Intersect(Bounds)) continue;
        if (Node.Left!=INDEX_NONE) { Stack.Add(Node.Left); Stack.Add(Node.Right); }
        else for (int32 I=Node.First;I<Node.First+Node.Count;++I) Out.Add(PolygonOrder[I]);
    }
}
