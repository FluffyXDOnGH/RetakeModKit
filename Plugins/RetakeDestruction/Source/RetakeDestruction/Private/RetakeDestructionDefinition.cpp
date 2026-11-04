// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeDestructionDefinition.h"
#include "RetakeDestructibleActor.h"
#include "RetakeVisibilityCoverage.h"
#include "Engine/StaticMesh.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionAlgo.h"
#include "GeometryCollection/Facades/CollectionRemoveOnBreakFacade.h"
#include "GeometryCollection/Facades/CollectionAnchoringFacade.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Algo/Reverse.h"
#if WITH_EDITOR
#include "EdGraphSchema_Niagara.h"
#include "Misc/DataValidation.h"
#include "Misc/PackageName.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"

namespace
{
/** Niagara's stack life cycle modules and the enum values their static switches store. */
namespace NiagaraLifeCycle
{
    const TCHAR* EmitterState=TEXT("/Niagara/Modules/Emitter/EmitterState.EmitterState");
    const TCHAR* SystemState=TEXT("/Niagara/Modules/System/SystemState.SystemState");
    // ENiagara_EmitterStateLifeCycleMode, ENiagara_EmitterStateLoopBehavior and ENiagara_InfiniteLoopDuration (Fixed, Infinite).
    constexpr int32 LoopInfinite=0,ModeSelf=1,DurationFixed=0;
}
const UNiagaraNodeFunctionCall* FindModule(const UNiagaraScriptSourceBase* Source,const TCHAR* ModuleScript)
{
    const auto* Script=Cast<UNiagaraScriptSource>(Source);
    const UNiagaraGraph* Graph=Script ? Script->NodeGraph.Get():nullptr;
    if (Graph) for (const UEdGraphNode* Node:Graph->Nodes)
        if (const auto* Call=Cast<UNiagaraNodeFunctionCall>(Node); Call && Call->IsNodeEnabled() && Call->FunctionScript && Call->FunctionScript->GetPathName()==ModuleScript) return Call;
    return nullptr;
}
/** A module's enum static switch as this call sets it; INDEX_NONE when the call has no such input. */
int32 SwitchValue(const UNiagaraNodeFunctionCall& Module,const TCHAR* Input)
{
    for (const UEdGraphPin* Pin:Module.Pins)
        if (Pin->Direction==EGPD_Input && Pin->GetFName()==Input)
        {
            const FNiagaraVariable Value=UEdGraphSchema_Niagara::PinToNiagaraVariable(Pin,true);
            return Value.IsDataAllocated() ? Value.GetValue<int32>():INDEX_NONE;
        }
    return INDEX_NONE;
}
/**
 * Why System would run until something stops it, read from the Emitter State and System State modules of its stack; empty
 * when every enabled emitter ends by itself. A life cycle that cannot be read counts as endless.
 */
FString NeverEndingReason(const UNiagaraSystem& System)
{
    using namespace NiagaraLifeCycle;
    const UNiagaraScript* SystemScript=System.GetSystemSpawnScript();
    const UNiagaraNodeFunctionCall* SystemModule=FindModule(SystemScript ? SystemScript->GetLatestSource():nullptr,SystemState);
    const int32 SystemLoop=SystemModule ? SwitchValue(*SystemModule,TEXT("Loop Behavior")):INDEX_NONE;
    for (const FNiagaraEmitterHandle& Handle:System.GetEmitterHandles())
    {
        if (!Handle.GetIsEnabled()) continue;
        const FString Emitter=Handle.GetName().ToString();
        if (Handle.GetEmitterMode()!=ENiagaraEmitterMode::Standard) return Emitter+TEXT(" is a lightweight emitter, whose life cycle cannot be checked");
        const FVersionedNiagaraEmitterData* Data=Handle.GetEmitterData();
        const UNiagaraNodeFunctionCall* Module=FindModule(Data ? Data->GraphSource.Get():nullptr,EmitterState);
        if (!Module) return Emitter+TEXT(" has no Emitter State module");
        if (SwitchValue(*Module,TEXT("Life Cycle Mode"))==ModeSelf)
        {
            const int32 Loop=SwitchValue(*Module,TEXT("Loop Behavior"));
            if (Loop==INDEX_NONE || Loop==LoopInfinite) return Emitter+TEXT(" loops forever");
            // Once and Multiple end only after a fixed loop duration; an infinite duration never finishes its loop.
            if (SwitchValue(*Module,TEXT("Loop Duration Mode"))!=DurationFixed) return Emitter+TEXT(" runs an endless loop");
        }
        else if (SystemLoop==INDEX_NONE || SystemLoop==LoopInfinite) return Emitter+TEXT(" follows the system life cycle, which loops forever");
    }
    return FString();
}
bool PrepareBaseAnchors(UGeometryCollection* Collection,bool bAnchorBase,float Depth,bool bAnchorEnds,float EndDepth)
{
    auto& Data=*Collection->GetGeometryCollection();
    const FName Attribute(TEXT("RetakeBaseAnchored"));
    const auto* Previous=Data.FindAttribute<bool>(Attribute,FGeometryCollection::TransformGroup);
    if (!bAnchorBase && !bAnchorEnds && !Previous) return true;
    TBitArray<> Desired(false,Data.Transform.Num());
    if (bAnchorBase || bAnchorEnds)
    {
        TArray<FTransform3f> Global;GeometryCollectionAlgo::GlobalMatrices(Data.Transform,Data.Parent,Global);
        TMap<int32,FBox> LeafBounds;FBox Total(ForceInit);
        for (int32 G=0;G<Data.TransformIndex.Num();++G)
        {
            const int32 Bone=Data.TransformIndex[G];
            if (!Global.IsValidIndex(Bone) || !Data.Children[Bone].IsEmpty() || !Data.BoundingBox[G].IsValid) continue;
            const FBox Bounds=Data.BoundingBox[G].TransformBy(FTransform(Global[Bone]).ToMatrixWithScale());
            LeafBounds.Add(Bone,Bounds);Total+=Bounds;
        }
        if (!Total.IsValid) return false;
        const int32 Long=Total.GetSize().X>=Total.GetSize().Y ? 0:1;
        for (const auto& Pair:LeafBounds)
            if ((bAnchorBase && Pair.Value.Min.Z<=Total.Min.Z+Depth+.01)
                || (bAnchorEnds && (Pair.Value.Min[Long]<=Total.Min[Long]+EndDepth+.01 || Pair.Value.Max[Long]>=Total.Max[Long]-EndDepth-.01))) Desired[Pair.Key]=true;
        if (Desired.CountSetBits()==0) return false;
    }
    Chaos::Facades::FCollectionAnchoringFacade Anchors(Data);
    bool Changed=!Previous || !Anchors.HasAnchoredAttribute();
    for (int32 I=0;I<Data.Transform.Num() && !Changed;++I)
    {
        const bool WasManaged=Previous && (*Previous)[I];
        const bool Authored=Anchors.IsAnchored(I) && !WasManaged;
        const bool Managed=Desired[I] && !Authored;
        Changed=WasManaged!=Managed || Anchors.IsAnchored(I)!=(Authored || Managed);
    }
    if (!Changed) return true;
    Collection->Modify();
    if (!Anchors.HasAnchoredAttribute()) Anchors.AddAnchoredAttribute();
    auto& Managed=Data.AddAttribute<bool>(Attribute,FGeometryCollection::TransformGroup);
    for (int32 I=0;I<Data.Transform.Num();++I)
    {
        const bool Authored=Anchors.IsAnchored(I) && !Managed[I];
        Managed[I]=Desired[I] && !Authored;
        Anchors.SetAnchored(I,Authored || Managed[I]);
    }
    Collection->MarkPackageDirty();return true;
}
}
#endif

void FRetakeDestructionPlaneCover::AppendIntactStrips(TFunctionRef<bool(int32)> IsReleased,TArray<FRetakeSightPolygon>& Out) const
{
    if (Columns<=0 || Cells.IsEmpty()) return;
    const int32 Rows=Cells.Num()/Columns;
    const FVector2D Step((GridMax.X-GridMin.X)/Columns,(GridMax.Y-GridMin.Y)/Rows);
    const FVector Normal=Polygon.Normal;
    auto Solid=[&](int32 Cell){ const auto& T=Cells[Cell].Transforms; return !T.IsEmpty() && !T.Contains(INDEX_NONE) && !T.ContainsByPredicate(IsReleased); };
    for (int32 Row=0;Row<Rows;++Row)
        for (int32 Col=0;Col<Columns;)
        {
            if (!Solid(Row*Columns+Col)) { ++Col; continue; }
            int32 End=Col; while (End+1<Columns && Solid(Row*Columns+End+1)) ++End;
            const FVector2D Lo=GridMin+Step*FVector2D(Col,Row),Hi=GridMin+Step*FVector2D(End+1,Row+1);
            FRetakeSightPolygon& Strip=Out.AddDefaulted_GetRef(); Strip.Normal=Normal; Strip.bTwoSided=Polygon.bTwoSided;
            for (const FVector2D& P:{Lo,FVector2D(Hi.X,Lo.Y),Hi,FVector2D(Lo.X,Hi.Y)}) Strip.Vertices.Add(U*P.X+V*P.Y+Normal*Offset);
            if (FVector::DotProduct(FVector::CrossProduct(Strip.Vertices[1]-Strip.Vertices[0],Strip.Vertices[2]-Strip.Vertices[0]),Normal)<0) Algo::Reverse(Strip.Vertices);
            Col=End+1;
        }
}

bool URetakeDestructionDefinition::AllowsGrenadePass(float ContactDamage,float IntoSurfaceSpeed,float& OutSpeedScale) const
{
    OutSpeedScale=1;
    if (!IsBrokenByGrenade(ContactDamage) || !FMath::IsFinite(IntoSurfaceSpeed) || IntoSurfaceSpeed<GrenadePassMinimumSpeed) return false;
    OutSpeedScale=FMath::Clamp(GrenadePassSpeedScale,0.f,1.f);
    return true;
}

bool URetakeDestructionDefinition::IsBrokenByGrenade(float ContactDamage) const
{
    return GrenadeBreakDamage>0 && FMath::IsFinite(ContactDamage) && ContactDamage>=GrenadeBreakDamage;
}

bool URetakeDestructionDefinition::PrepareCollection()
{
#if WITH_EDITOR
    if (!Collection || !Collection->GetGeometryCollection()) return false;
    Modify();
    auto& Data=*Collection->GetGeometryCollection();
    if (!PrepareBaseAnchors(Collection,bAnchorBase,BaseAnchorDepthCm,bAnchorEnds,EndAnchorDepthCm)) return false;
    GeometryCollection::Facades::FCollectionRemoveOnBreakFacade Removal(Data);
    const bool HasExplodedPreview=Data.HasAttribute(TEXT("ExplodedVector"),FGeometryCollection::TransformGroup);
    bool Changed=HasExplodedPreview || (!bUseAuthoredResponse && (Collection->bRemoveOnMaxSleep!=bRemoveDebrisOnSleep || Collection->MaximumSleepTime!=SleepSeconds
        || Collection->RemovalDuration!=FVector2D(FadeSeconds,FadeSeconds) || !Collection->bSlowMovingAsSleeping
        || Collection->bAutomaticCrumblePartialClusters || !Removal.IsValid()));
    const GeometryCollection::Facades::FRemoveOnBreakData RemovalSettings(bLimitDebrisLifetime,
        FVector2f(FMath::Max(.1f,MaximumDebrisSeconds-FadeSeconds)),false,FVector2f(FadeSeconds));
    if (!bUseAuthoredResponse && Removal.IsValid()) for (int32 I=0;I<Data.Transform.Num() && !Changed;++I)
        Changed=Removal.GetData(I).GetPackedData()!=RemovalSettings.GetPackedData();
    // UGeometryCollection::Modify invalidates its state GUID. Do not invalidate other
    // definitions sharing this collection when its actual data has not changed.
    if (Changed) Collection->Modify();
    if (HasExplodedPreview) Data.RemoveAttribute(TEXT("ExplodedVector"),FGeometryCollection::TransformGroup);
    if (!bUseAuthoredResponse)
    {
    Collection->bRemoveOnMaxSleep=bRemoveDebrisOnSleep;
    Collection->MaximumSleepTime=SleepSeconds;
    Collection->RemovalDuration=FVector2D(FadeSeconds,FadeSeconds);
    Collection->bSlowMovingAsSleeping=true;
    Collection->bAutomaticCrumblePartialClusters=false;
    Removal.DefineSchema();
    Removal.SetToAll(RemovalSettings);
    }
    Occluders.Reset();
    OccluderIndex.Reset();
    TArray<FTransform3f> Global;
    GeometryCollectionAlgo::GlobalMatrices(Data.Transform,Data.Parent,Global);
    TMap<int32,FRetakeSightPolygonIndex> ByTransform;
    for (int32 Face=0;Face<Data.Indices.Num();++Face)
    {
        if (!Data.Visible[Face] || Data.Internal[Face]) continue;
        const int32 MaterialIndex=Data.MaterialID[Face];
        UMaterialInterface* Material=Materials.IsValidIndex(MaterialIndex) && Materials[MaterialIndex]
            ? Materials[MaterialIndex].Get() : (Collection->Materials.IsValidIndex(MaterialIndex) ? Collection->Materials[MaterialIndex].Get() : nullptr);
        if (!Material || Material->GetBlendMode()!=BLEND_Opaque) continue;
        const UMaterial* Base=Material->GetMaterial();
        if (!Base || Base->HasVertexPositionOffsetConnected() || Base->HasPixelDepthOffsetConnected()) continue;
        const FIntVector Tri=Data.Indices[Face];
        const int32 Bone=Data.BoneMap[Tri.X];
        if (!Global.IsValidIndex(Bone) || Data.BoneMap[Tri.Y]!=Bone || Data.BoneMap[Tri.Z]!=Bone) continue;
        FRetakeSightPolygon P;
        for (int32 V:{Tri.X,Tri.Y,Tri.Z}) P.Vertices.Add(FVector(Global[Bone].TransformPosition(Data.Vertex[V])));
        // Unreal front faces use the reverse of the mathematical triangle normal (as in the static bake). Without this
        // swap every destructible face pointed into the solid, so one-sided faces never concealed anyone (2026-09-28).
        Swap(P.Vertices[1],P.Vertices[2]);
        P.Normal=FVector::CrossProduct(P.Vertices[1]-P.Vertices[0],P.Vertices[2]-P.Vertices[0]).GetSafeNormal();
        P.bTwoSided=Material->IsTwoSided();
        if (!P.Normal.IsNearlyZero()) ByTransform.FindOrAdd(Bone).LocalPolygons.Add(MoveTemp(P));
    }
    for (auto& Pair:ByTransform)
    {
        Pair.Value.MergeCoplanarPolygons();
        for (auto& Polygon:Pair.Value.LocalPolygons)
        {
            auto& Entry=Occluders.AddDefaulted_GetRef(); Entry.TransformIndex=Pair.Key; Entry.Polygon=MoveTemp(Polygon);
        }
    }
    auto Area=[](const FRetakeDestructionOccluder& Entry)
    {
        double Result=0;const auto& V=Entry.Polygon.Vertices;
        for (int32 I=1;I+1<V.Num();++I) Result+=FVector::CrossProduct(V[I]-V[0],V[I+1]-V[0]).Size()*.5;
        return Result;
    };
    // Group the faces by plane. A plane whose faces exactly tile their convex outline (Voronoi faces never overlap, so
    // equal areas mean no gap) gets one cover polygon; a gap, a dropped tiny face or a curved face leaves it per face.
    PlaneCovers.Reset();
    {
        // Collection vertices are single precision: across a 6 m face they drift by a few 0.01 mm, so faces are grouped
        // within 1 mm rather than hashed exactly (exact bins split one face into several "gapped" groups, 2026-09-28).
        struct FGroup { FVector Normal; double Offset; TArray<int32> Value; };
        TArray<FGroup> Groups;
        for (int32 I=0;I<Occluders.Num();++I)
        {
            const auto& P=Occluders[I].Polygon;
            const double Offset=FVector::DotProduct(P.Normal,P.Vertices[0]);
            FGroup* Group=Groups.FindByPredicate([&](const FGroup& G){return FVector::DotProduct(G.Normal,P.Normal)>1.-1.e-6 && FMath::Abs(G.Offset-Offset)<.1;});
            if (!Group) { Group=&Groups.AddDefaulted_GetRef(); Group->Normal=P.Normal; Group->Offset=Offset; }
            Group->Value.Add(I);
        }
        for (const FGroup& Group:Groups)
        {
            // Only large faces made of many pieces pay off; small coplanar patches on curved props would bloat every query.
            if (Group.Value.Num()<8) continue;
            const auto& First=Occluders[Group.Value[0]].Polygon;
            FVector U,V; First.Normal.FindBestAxisVectors(U,V);
            const double Offset=FVector::DotProduct(First.Normal,First.Vertices[0]);
            TArray<FVector2D> Points; double Sum=0; bool TwoSided=true,Flat=true;
            for (int32 I:Group.Value)
            {
                const auto& P=Occluders[I].Polygon;
                TwoSided&=P.bTwoSided;
                for (const FVector& X:P.Vertices) { Points.Add(FVector2D(FVector::DotProduct(X,U),FVector::DotProduct(X,V))); Flat&=FMath::Abs(FVector::DotProduct(X,First.Normal)-Offset)<.1; }
                for (int32 K=1;K+1<P.Vertices.Num();++K) Sum+=FVector::CrossProduct(P.Vertices[K]-P.Vertices[0],P.Vertices[K+1]-P.Vertices[0]).Size()*.5;
            }
            // Hull() takes at most 256 points; a 6 m face has hundreds. The hull of partial hulls is the same outline.
            while (Points.Num()>256)
            {
                TArray<FVector2D> Reduced;
                for (int32 Start=0;Start<Points.Num();Start+=200)
                {
                    RetakeVisibilityCoverage::FPolygon Part;
                    const TConstArrayView<FVector2D> Batch(Points.GetData()+Start,FMath::Min(200,Points.Num()-Start));
                    if (Batch.Num()>=3 && RetakeVisibilityCoverage::Hull(Batch,Part)) Reduced.Append(Part.GetData(),Part.Num());
                    else Reduced.Append(Batch.GetData(),Batch.Num());
                }
                if (Reduced.Num()>=Points.Num()) break;
                Points=MoveTemp(Reduced);
            }
            RetakeVisibilityCoverage::FPolygon Hull;
            if (!RetakeVisibilityCoverage::Hull(Points,Hull)) continue;
            const double HullArea=RetakeVisibilityCoverage::Area(Hull);
            UE_LOGF(LogRetakeDestruction,Verbose,"event=DestructionFaceGroup definition=%ls normal=%ls offset=%.3f faces=%d face_area=%.2f outline_area=%.2f flat=%d",
                *GetName(),*First.Normal.ToCompactString(),Offset,Group.Value.Num(),Sum,HullArea,Flat);
            if (!Flat) continue;
            if (HullArea<2500. || FMath::Abs(HullArea-Sum)>FMath::Max(.5,HullArea*1.e-4)) continue;
            auto& Cover=PlaneCovers.AddDefaulted_GetRef();
            Cover.Polygon.Normal=First.Normal; Cover.Polygon.bTwoSided=TwoSided;
            for (const FVector2D& H:Hull) Cover.Polygon.Vertices.Add(U*H.X+V*H.Y+First.Normal*Offset);
            // Keep the outline's winding consistent with the face normal.
            if (FVector::DotProduct(FVector::CrossProduct(Cover.Polygon.Vertices[1]-Cover.Polygon.Vertices[0],Cover.Polygon.Vertices[2]-Cover.Polygon.Vertices[0]),First.Normal)<0)
                Algo::Reverse(Cover.Polygon.Vertices);
            for (int32 I:Group.Value) { Cover.Transforms.AddUnique(Occluders[I].TransformIndex); Occluders[I].Cover=PlaneCovers.Num()-1; }
            // Grid over the outline's bounds; only cells whose corners are all inside the outline are kept as cover.
            {
                FBox2D Box(ForceInit); for (const FVector2D& H:Hull) Box+=H;
                const int32 Columns=FMath::Clamp(FMath::CeilToInt(Box.GetSize().X/30.),1,64),Rows=FMath::Clamp(FMath::CeilToInt(Box.GetSize().Y/30.),1,64);
                Cover.U=U; Cover.V=V; Cover.Offset=Offset; Cover.GridMin=Box.Min; Cover.GridMax=Box.Max; Cover.Columns=Columns;
                Cover.Cells.SetNum(Columns*Rows);
                const FVector2D Step((Box.Max.X-Box.Min.X)/Columns,(Box.Max.Y-Box.Min.Y)/Rows);
                auto InsideHull=[&](const FVector2D& P)
                {
                    for (int32 K=0;K<Hull.Num();++K)
                    {
                        const FVector2D E=Hull[(K+1)%Hull.Num()]-Hull[K];
                        if (FVector2D::CrossProduct(E,P-Hull[K])<-.01) return false;
                    }
                    return true;
                };
                for (int32 Row=0;Row<Rows;++Row) for (int32 Col=0;Col<Columns;++Col)
                {
                    const FVector2D Lo=Box.Min+Step*FVector2D(Col,Row),Hi=Lo+Step;
                    if (!InsideHull(Lo) || !InsideHull(Hi) || !InsideHull(FVector2D(Lo.X,Hi.Y)) || !InsideHull(FVector2D(Hi.X,Lo.Y))) Cover.Cells[Row*Columns+Col].Transforms.Add(INDEX_NONE);
                }
                for (int32 I:Group.Value)
                {
                    FBox2D Face(ForceInit);
                    for (const FVector& X:Occluders[I].Polygon.Vertices) Face+=FVector2D(FVector::DotProduct(X,U),FVector::DotProduct(X,V));
                    const int32 C0=FMath::Clamp(FMath::FloorToInt((Face.Min.X-Box.Min.X)/Step.X),0,Columns-1),C1=FMath::Clamp(FMath::FloorToInt((Face.Max.X-Box.Min.X)/Step.X),0,Columns-1);
                    const int32 R0=FMath::Clamp(FMath::FloorToInt((Face.Min.Y-Box.Min.Y)/Step.Y),0,Rows-1),R1=FMath::Clamp(FMath::FloorToInt((Face.Max.Y-Box.Min.Y)/Step.Y),0,Rows-1);
                    for (int32 Row=R0;Row<=R1;++Row) for (int32 Col=C0;Col<=C1;++Col) Cover.Cells[Row*Columns+Col].Transforms.AddUnique(Occluders[I].TransformIndex);
                }
            }
        }
    }
    if (PlaneCovers.Num()>32)
    {
        // Keep the 32 largest covers; faces of dropped covers fall back to per-face tests.
        TArray<int32> Order; for (int32 I=0;I<PlaneCovers.Num();++I) Order.Add(I);
        auto CoverArea=[&](int32 I){ const auto& V=PlaneCovers[I].Polygon.Vertices; double A=0; for (int32 K=1;K+1<V.Num();++K) A+=FVector::CrossProduct(V[K]-V[0],V[K+1]-V[0]).Size(); return A; };
        Order.Sort([&](int32 A,int32 B){return CoverArea(A)>CoverArea(B);});
        TArray<int32> Remap; Remap.Init(INDEX_NONE,PlaneCovers.Num());
        TArray<FRetakeDestructionPlaneCover> Kept;
        for (int32 I=0;I<32;++I) { Remap[Order[I]]=Kept.Num(); Kept.Add(MoveTemp(PlaneCovers[Order[I]])); }
        PlaneCovers=MoveTemp(Kept);
        for (auto& Entry:Occluders) if (Entry.Cover!=INDEX_NONE) Entry.Cover=Remap[Entry.Cover];
    }
    // After the covers: tiny faces are real opaque surface, dropped from the per-face list only for cost. Tiling is
    // judged on every face, so noisy cuts (many sliver triangles) still earn a whole-face cover.
    // Tiny curved/tessellated detail is expensive and cannot provide useful whole-body coverage.
    Occluders.RemoveAll([&](const auto& Entry){return Area(Entry)<1.;});
    if (Occluders.Num()>FMath::Clamp(MaximumOcclusionPolygons,0,8192))
    {
        Occluders.StableSort([&](const auto& A,const auto& B){return Area(A)>Area(B);});
        Occluders.SetNum(FMath::Clamp(MaximumOcclusionPolygons,0,8192));
        Occluders.Shrink();
    }
    if (Changed) Collection->MarkPackageDirty();
    PreparedCollectionGuid=Collection->GetStateGuid();
    PreparedTransformCount=Data.Transform.Num();
    MarkPackageDirty();
    return true;
#else
    return false;
#endif
}

const FRetakeSightPolygonIndex& URetakeDestructionDefinition::GetOccluderIndex() const
{
    if (!OccluderIndex)
    {
        OccluderIndex=MakeShared<FRetakeSightPolygonIndex>();
        for (const auto& Entry:Occluders) OccluderIndex->LocalPolygons.Add(Entry.Polygon);
        OccluderIndex->BuildIndex();
    }
    return *OccluderIndex;
}
void URetakeDestructionDefinition::FindOccluders(const FVector& Start,const FVector& End,FRetakeVisibilityCandidates& Out) const
{
    GetOccluderIndex().FindCandidates(Start,End,Out);
}
void URetakeDestructionDefinition::FindOccludersInBounds(const FBox& Bounds,FRetakeVisibilityCandidates& Out) const
{
    GetOccluderIndex().FindCandidatesInBounds(Bounds,Out);
}

FString URetakeDestructionDefinition::GetPreparationSummary() const
{
    int32 Anchored=0,Fixed=0,Leaves=0;
    if (Collection && Collection->GetGeometryCollection())
    {
        const auto& Data=*Collection->GetGeometryCollection();
        Chaos::Facades::FCollectionAnchoringFacade Anchors(Data);
        for (int32 I=0;I<Data.Transform.Num();++I)
        {
            if (Data.Children[I].IsEmpty()) ++Leaves;
            if (Anchors.HasAnchoredAttribute() && Anchors.IsAnchored(I)) ++Anchored;
            if (Anchors.HasInitialDynamicStateAttribute())
            {
                const auto State=Anchors.GetInitialDynamicState(I);
                if (State==Chaos::EObjectStateType::Static || State==Chaos::EObjectStateType::Kinematic) ++Fixed;
            }
        }
    }
    return FString::Printf(TEXT("transforms=%d leaves=%d anchored=%d fixed=%d frame=%s opaque_polygons=%d face_covers=%d current=%d"),
        PreparedTransformCount,Leaves,Anchored,Fixed,*GetNameSafe(FrameMesh),Occluders.Num(),PlaneCovers.Num(),
        Collection && PreparedCollectionGuid==Collection->GetStateGuid());
}

FString URetakeDestructionDefinition::GetRemovalSummary() const
{
    if (!Collection || !Collection->GetGeometryCollection()) return TEXT("missing");
    const auto& Data=*Collection->GetGeometryCollection();
    GeometryCollection::Facades::FCollectionRemoveOnBreakFacade Removal(Data);
    int32 Enabled=0,Crumbling=0;
    FString Ranges;
    TSet<FString> Seen;
    if (Removal.IsValid()) for (int32 I=0;I<Data.Transform.Num();++I)
    {
        const auto Entry=Removal.GetData(I);
        if (!Entry.IsEnabled()) continue;
        ++Enabled;if (Entry.GetClusterCrumbling()) ++Crumbling;
        const FVector2f Start=Entry.GetBreakTimer(),Fade=Entry.GetRemovalTimer();
        const FString Range=FString::Printf(TEXT("[%.3f,%.3f;%.3f,%.3f]"),Start.X,Start.Y,Fade.X,Fade.Y);
        if (!Seen.Contains(Range)) {Seen.Add(Range);Ranges+=Range;}
    }
    return FString::Printf(TEXT("schema=%d transforms=%d enabled=%d crumble=%d sleep=%d sleep_range=%s fade_range=%s slow_sleep=%d automatic_crumble=%d break_ranges=%s"),
        Removal.IsValid(),Data.Transform.Num(),Enabled,Crumbling,Collection->bRemoveOnMaxSleep,
        *Collection->MaximumSleepTime.ToString(),*Collection->RemovalDuration.ToString(),Collection->bSlowMovingAsSleeping,
        Collection->bAutomaticCrumblePartialClusters,*Ranges);
}

#if WITH_EDITOR
EDataValidationResult URetakeDestructionDefinition::IsDataValid(FDataValidationContext& Context) const
{
    const auto SuperResult=Super::IsDataValid(Context);
    bool Valid=SuperResult!=EDataValidationResult::Invalid;
    auto Fail=[&](const TCHAR* Text){Context.AddError(FText::FromString(Text));Valid=false;};
    if (!Collection || !Collection->GetGeometryCollection()) Fail(TEXT("A destruction collection is required."));
    else if (PreparedCollectionGuid!=Collection->GetStateGuid() || PreparedTransformCount!=Collection->GetGeometryCollection()->Transform.Num())
        Fail(TEXT("Collection changed: run Prepare Collection and save the definition and collection."));
    if (MaximumOcclusionPolygons<0 || MaximumOcclusionPolygons>8192 || Occluders.Num()>MaximumOcclusionPolygons)
        Fail(TEXT("Run Prepare Collection to apply the bounded visibility geometry budget."));
    if (!FMath::IsFinite(StrainPerDamage) || StrainPerDamage<=0 || !FMath::IsFinite(PointRadiusCm) || PointRadiusCm<=0)
        Fail(TEXT("Damage strain and radius must be finite and positive."));
    if (SleepSeconds.ContainsNaN() || !FMath::IsFinite(FadeSeconds) || !FMath::IsFinite(MaximumDebrisSeconds)
        || SleepSeconds.X<0 || SleepSeconds.Y<SleepSeconds.X || FadeSeconds<=0 || MaximumDebrisSeconds<=FadeSeconds)
        Fail(TEXT("Invalid debris cleanup settings."));
    if (!FMath::IsFinite(DebrisQuietSeconds) || DebrisQuietSeconds<=0 || !FMath::IsFinite(DebrisSleepSpeed) || DebrisSleepSpeed<1)
        Fail(TEXT("Invalid debris settling delay or speed."));
    if (!bUseAuthoredResponse && Collection && Collection->GetGeometryCollection())
    {
        GeometryCollection::Facades::FCollectionRemoveOnBreakFacade Removal(*Collection->GetGeometryCollection());
        const GeometryCollection::Facades::FRemoveOnBreakData Expected(bLimitDebrisLifetime,
            FVector2f(FMath::Max(.1f,MaximumDebrisSeconds-FadeSeconds)),false,FVector2f(FadeSeconds));
        if (Collection->bRemoveOnMaxSleep!=bRemoveDebrisOnSleep || !Removal.IsValid())
            Fail(TEXT("Run Prepare Collection to synchronize cleanup settings; use separate collections for different cleanup presets."));
        else for (int32 I=0;I<Collection->GetGeometryCollection()->Transform.Num();++I) if (Removal.GetData(I).GetPackedData()!=Expected.GetPackedData())
            {Fail(TEXT("Collection removal data differs from the definition. Prepare the collection, or duplicate it for this preset."));break;}
    }
    if (!FMath::IsFinite(RelevanceDistanceCm) || RelevanceDistanceCm<1000 || !FMath::IsFinite(ImpulseSpeed) || ImpulseSpeed<0)
        Fail(TEXT("Invalid relevancy distance or impulse speed."));
    for (float Value:{RadialImpulseSpeed,ExplosionImpulseSpeed,AngularImpulseSpeed,MinimumEffectSpeed,BreakSoundVolume})
        if (!FMath::IsFinite(Value) || Value<0) {Fail(TEXT("Impulse and presentation limits must be finite and nonnegative."));break;}
    if (!FMath::IsFinite(CollisionSoundWindowSeconds) || CollisionSoundWindowSeconds<0 || CollisionSoundWindowSeconds>30
        || !FMath::IsFinite(CollisionSoundMaxDurationSeconds) || CollisionSoundMaxDurationSeconds<.2f || CollisionSoundMaxDurationSeconds>10
        || !FMath::IsFinite(CollisionSoundMinimumImpactSpeed) || CollisionSoundMinimumImpactSpeed<1
        || !FMath::IsFinite(CollisionSoundVolume) || CollisionSoundVolume<0 || MaximumCollisionVoices<1 || MaximumCollisionVoices>4)
        Fail(TEXT("Invalid collision-audio window, duration, impact speed or volume."));
    for (const FVector2D Range:{BreakPitch,CollisionPitch,BreakEffectCooldownSeconds,ImpulseNoiseRange})
        if (Range.ContainsNaN() || Range.X<0 || Range.Y<Range.X) {Fail(TEXT("Presentation ranges must be finite, ordered, and nonnegative."));break;}
    for (const FVector2D Range:{BreakSoundFrequencyHz,CollisionSoundFrequencyHz})
        if (Range.ContainsNaN() || Range.X<0 || Range.Y<0) {Fail(TEXT("Sound frequency endpoints must be finite and nonnegative."));break;}
    for (const FBox& Box:AnchorBoxes) if (!Box.IsValid || Box.Min.ContainsNaN() || Box.Max.ContainsNaN())
        {Fail(TEXT("Anchor boxes must have finite, valid bounds."));break;}
    if (!FMath::IsFinite(BaseAnchorDepthCm) || BaseAnchorDepthCm<=0) Fail(TEXT("Base anchor depth must be finite and positive."));
    if (BreakMaterialIndices.Num()!=BreakMaterials.Num()) Fail(TEXT("Break material indices and materials must match."));
    if (Collection) for (int32 Index:BreakMaterialIndices) if (!Collection->Materials.IsValidIndex(Index))
        {Fail(TEXT("Break material index is outside the collection's material slots."));break;}
    for (float Threshold:DamageThresholds) if (!FMath::IsFinite(Threshold) || Threshold<=0) {Fail(TEXT("Damage thresholds must be positive."));break;}
    // A mod's effects: its own break effect, spawned under the shared effect budget, which tracks every effect until it ends.
    const FName Root=FPackageName::GetPackageMountPoint(GetPackage()->GetName(),false);
    if (Root!=FName(TEXT("/Game/")))
    {
        if (!WorldChaosEffects.IsEmpty()) Fail(TEXT("World Chaos Effects run for the whole level, outside the effect budget; a mod definition uses Break Effect."));
        if (BreakEffect)
        {
            if (FPackageName::GetPackageMountPoint(BreakEffect->GetPackage()->GetName(),false)!=Root)
                Fail(*FString::Printf(TEXT("Break Effect %s is outside this definition's package root %s."),*BreakEffect->GetPathName(),*Root.ToString()));
            else if (const FString Endless=NeverEndingReason(*BreakEffect); !Endless.IsEmpty())
                Fail(*FString::Printf(TEXT("Break Effect %s must end on its own: %s."),*BreakEffect->GetPathName(),*Endless));
        }
    }
    return Valid ? EDataValidationResult::Valid : EDataValidationResult::Invalid;
}
#endif
