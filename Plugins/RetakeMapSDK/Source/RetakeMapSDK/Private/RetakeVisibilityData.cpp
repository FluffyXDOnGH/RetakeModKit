// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeVisibilityData.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "StaticMeshResources.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "RetakeDoor.h"
#include "Misc/SecureHash.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/DataValidation.h"
#include "Misc/ScopeExit.h"
#include "Misc/App.h"
#include "JsonObjectConverter.h"
#include "Algo/Reverse.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
void HashText(FMD5& Hash,const FString& Value)
{
    FTCHARToUTF8 Bytes(*Value); Hash.Update(reinterpret_cast<const uint8*>(Bytes.Get()),Bytes.Length());
    const uint8 Separator=0; Hash.Update(&Separator,1);
}
TSharedPtr<FJsonValue> VectorJSON(const FVector& V)
{
    return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(V.X),MakeShared<FJsonValueNumber>(V.Y),MakeShared<FJsonValueNumber>(V.Z)});
}
enum class ENaniteProof : uint8 { NotNanite, ExactFallback, Unsupported };
// Nanite meshes render their Nanite data, not their LOD resources. Fallback LOD 0 is the Nanite builder's own input,
// copied unchanged, only when the builder cannot reduce it: UE6 FMeshNaniteSettings::IsFallbackPotentiallyReduced() is
// false (fallback 100% of triangles or relative error 0; no keep/trim reduction; no assembly; Auto always counts as
// reduced) and Nanite is built from that same LOD 0 (no hi-res source model). Shape preservation and build-time
// displacement alter the rendered surface; an explicit precision coarser than the automatic 1/16 cm minimum moves it.
// A reduced fallback can be smaller or larger than the rendered surface, so it stays uncertain. Coarser fallback LODs
// remain recorded: every LOD must block, which also covers clients that render the fallback instead of Nanite.
// The settings are editor-only data and feed only the bake export. Live, unbaked captures (PIE and cooked builds) keep
// every Nanite mesh unsupported; certified runtime proof comes from the bake snapshot, and no runtime admission
// fingerprint reads these settings.
ENaniteProof NaniteProof(const UStaticMesh* Mesh,const TCHAR*& Reason)
{
    Reason=nullptr;
#if WITH_EDITORONLY_DATA
    // Authored intent, including force-enabled Nanite.
    if (!Mesh->IsNaniteEnabled()) return ENaniteProof::NotNanite;
    const FMeshNaniteSettings& Settings=Mesh->GetNaniteSettings();
    if (Mesh->IsNaniteAssembly() || Settings.IsFallbackPotentiallyReduced()) Reason=TEXT("nanite-reduced-fallback");
    else if (Mesh->IsHiResMeshDescriptionValid()) Reason=TEXT("nanite-hi-res-source");
    else if (Settings.ShapePreservation!=ENaniteShapePreservation::None || !Settings.DisplacementMaps.IsEmpty()) Reason=TEXT("nanite-altered-surface");
    else if (Settings.PositionPrecision!=MIN_int32 && Settings.PositionPrecision<4) Reason=TEXT("nanite-coarse-precision");
    else { Reason=TEXT("nanite-exact-fallback"); return ENaniteProof::ExactFallback; }
    return ENaniteProof::Unsupported;
#else
    // Cooked game builds cannot see the editor-only settings; exclude meshes that actually carry Nanite render data.
    if (!Mesh->HasValidNaniteData()) return ENaniteProof::NotNanite;
    Reason=TEXT("nanite-unverifiable"); return ENaniteProof::Unsupported;
#endif
}
}

FRetakeSightPolygon RetakeVisibilityScene::ToWorld(const FRetakeSightPolygon& Local,const FTransform& Transform)
{
    FRetakeSightPolygon Result=Local;
    for (FVector& V:Result.Vertices) V=Transform.TransformPosition(V);
    const FVector Scale=Transform.GetScale3D();
    if (Scale.X*Scale.Y*Scale.Z<0) Algo::Reverse(Result.Vertices);
    // Merged boundaries can start with several collinear vertices. Accumulate
    // the complete polygon area rather than assuming its first triangle has area.
    // Origin-relative edges also avoid cancellation at large world coordinates.
    FVector Area=FVector::ZeroVector;
    for (int32 I=1;I+1<Result.Vertices.Num();++I)
        Area+=FVector::CrossProduct(Result.Vertices[I]-Result.Vertices[0],Result.Vertices[I+1]-Result.Vertices[0]);
    Result.Normal=Area.GetSafeNormal();
    return Result;
}

bool RetakeVisibilityScene::Capture(UWorld* World,TArray<FRetakeOcclusionMesh>& Meshes,FString& Fingerprint,FString& Error,FString* OutDiagnostics,bool bBakeExport)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(RetakeVisibility_Capture);
    Meshes.Reset(); Fingerprint.Reset(); Error.Reset();
    if (OutDiagnostics) OutDiagnostics->Reset();
    if (!World) { Error=TEXT("No world for visibility geometry."); return false; }
    if (World->GetNetMode()==NM_DedicatedServer || !FApp::CanEverRenderOrProduceRenderData())
    { Error=TEXT("Headless visibility requires a validated runtime geometry snapshot; renderer defaults cannot certify walls."); return false; }
    TArray<UStaticMeshComponent*> Components;
#if WITH_EDITOR
    TSet<UMaterial*> PreparedMaterials;
#endif
    RetakeVisibilityScene::GatherComponents(World,Components);
    FMD5 Hash;
    TArray<TSharedPtr<FJsonValue>> Checkpoints;
    HashText(Hash,TEXT("RetakeVisibilityGeometry:4-cpu-consistent-all-LODs"));
    for (auto* C:Components)
    {
        const UStaticMesh* Mesh=C->GetStaticMesh();
        const bool Dynamic=C->GetOwner()->IsA<ARetakeDoor>();
        const TCHAR* NaniteReason=nullptr;
        ENaniteProof Nanite=NaniteProof(Mesh,NaniteReason);
        // Exact fallbacks are dense (every source triangle on every fallback LOD). Only the bake export pays for them;
        // live, unbaked captures (PIE on an unbaked map) keep Nanite unsupported as before, which is only more visible.
        if (Nanite==ENaniteProof::ExactFallback && !bBakeExport) { Nanite=ENaniteProof::Unsupported; NaniteReason=TEXT("nanite-live-capture"); }
        // Supported render geometry whose triangles the exporter reads; packaging requires CPU access only for these.
        bool bProofCandidate=false;
        TArray<TSharedPtr<FJsonValue>> LODDiagnostics;
        ON_SCOPE_EXIT
        {
            if (OutDiagnostics)
            {
                // Finalize a copy: checkpoints must never change the actual bake hash.
                FMD5 Checkpoint=Hash; uint8 Digest[16]; Checkpoint.Final(Digest);
                auto Item=MakeShared<FJsonObject>();
                Item->SetStringField(TEXT("component"),C->GetPathName(World));
                Item->SetStringField(TEXT("mesh"),Mesh->GetPathName());
                Item->SetStringField(TEXT("hash_after"),BytesToHex(Digest,16).ToLower());
                Item->SetStringField(TEXT("transform"),Dynamic ? TEXT("dynamic-door") : C->GetComponentTransform().ToString());
                Item->SetBoolField(TEXT("cpu_access"),Mesh->bAllowCPUAccess);
                Item->SetBoolField(TEXT("nanite"),Nanite!=ENaniteProof::NotNanite);
                if (NaniteReason) Item->SetStringField(TEXT("nanite_proof"),NaniteReason);
                Item->SetBoolField(TEXT("proof_candidate"),bProofCandidate);
                Item->SetNumberField(TEXT("minimum_draw_distance"),C->MinDrawDistance);
                Item->SetNumberField(TEXT("authored_maximum_draw_distance"),C->LDMaxDrawDistance);
                Item->SetNumberField(TEXT("cached_maximum_draw_distance"),C->CachedMaxDrawDistance);
                Item->SetArrayField(TEXT("lods"),LODDiagnostics);
                const auto* Render=Mesh->GetRenderData();
                Item->SetNumberField(TEXT("lod_count"),Render ? Render->LODResources.Num():0);
                TArray<TSharedPtr<FJsonValue>> Materials;
                for (int32 I=0;I<C->GetNumMaterials();++I)
                {
                    const auto* Material=C->GetMaterial(I);
                    Materials.Add(MakeShared<FJsonValueString>(Material ? Material->GetPathName():TEXT("no-material")));
                }
                Item->SetArrayField(TEXT("materials"),Materials);
                Checkpoints.Add(MakeShared<FJsonValueObject>(Item));
            }
        };
        HashText(Hash,C->GetPathName(World)); HashText(Hash,Mesh->GetPathName());
        HashText(Hash,Dynamic ? TEXT("dynamic-door") : C->GetComponentTransform().ToString());
        // A component transform is not a faithful render mesh for instances,
        // spline deformation or a reduced Nanite fallback (see NaniteProof).
        // Uncertain surfaces cannot provide a concealment certificate.
        if (C->IsA<UInstancedStaticMeshComponent>() || C->IsA<USplineMeshComponent>() || Nanite==ENaniteProof::Unsupported
            || C->MinDrawDistance>0 || C->LDMaxDrawDistance>0 || C->CachedMaxDrawDistance>0)
        { HashText(Hash,TEXT("unsupported-render-geometry")); continue; }
        bProofCandidate=true;
        // Non-Nanite hashes are unchanged; an exact Nanite fallback is marked so a later settings change re-exports.
        if (Nanite==ENaniteProof::ExactFallback) HashText(Hash,TEXT("nanite-exact-fallback"));
        // CPU access is retained by the bake authoring script for release geometry.
        // Omitted geometry is not a blocker, but still participates in the fingerprint.
        const FStaticMeshRenderData* Data=Mesh->GetRenderData();
        // Use the same eligibility in editor exports and game/server worlds. Editor
        // render buffers alone must not certify geometry absent from a cooked server.
        if (!Data || Data->LODResources.IsEmpty() || !Mesh->bAllowCPUAccess)
        { HashText(Hash,TEXT("unavailable-geometry")); continue; }
        FRetakeOcclusionMesh Entry; Entry.Component=C; Entry.bDynamic=Dynamic;
        Entry.NumLODs=Data->LODResources.Num();
        HashText(Hash,FString::FromInt(Entry.NumLODs));
        Entry.SourceMesh=C->GetStaticMesh(); Entry.SourceTransform=C->GetComponentTransform();
        for (int32 MaterialIndex=0;MaterialIndex<C->GetNumMaterials();++MaterialIndex) Entry.SourceMaterials.Add(C->GetMaterial(MaterialIndex));
        for (int32 LODIndex=0;LODIndex<Entry.NumLODs;++LODIndex)
        {
        const auto& LOD=Data->LODResources[LODIndex];
        const auto Indices=LOD.IndexBuffer.GetArrayView();
        TArray<TSharedPtr<FJsonValue>> SectionDiagnostics;
        ON_SCOPE_EXIT
        {
            if (OutDiagnostics)
            {
                FMD5 Checkpoint=Hash; uint8 Digest[16]; Checkpoint.Final(Digest);
                auto Item=MakeShared<FJsonObject>();
                Item->SetNumberField(TEXT("lod"),LODIndex);
                Item->SetNumberField(TEXT("vertices"),LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices());
                Item->SetNumberField(TEXT("indices"),Indices.Num());
                Item->SetStringField(TEXT("hash_after"),BytesToHex(Digest,16).ToLower());
                Item->SetArrayField(TEXT("sections"),SectionDiagnostics);
                LODDiagnostics.Add(MakeShared<FJsonValueObject>(Item));
            }
        };
        HashText(Hash,FString::FromInt(LODIndex));
        if (Indices.Num()==0) { HashText(Hash,TEXT("empty-LOD")); continue; }
        for (const auto& Section:LOD.Sections)
        {
            UMaterialInterface* Material=C->GetMaterial(Section.MaterialIndex);
            HashText(Hash,Material ? Material->GetPathName() : TEXT("no-material"));
            UMaterial* BaseMaterial=Material ? Material->GetMaterial():nullptr;
#if WITH_EDITOR
            // Uncooked headless worlds may not have the renderer's expression cache.
            // Analyze the source graph once per material before asking whether WPO/PDO
            // is connected. Cooked builds use the serialized expression data instead.
            if (BaseMaterial && !BaseMaterial->GetOutermost()->bIsCookedForEditor && !PreparedMaterials.Contains(BaseMaterial))
            {
                PreparedMaterials.Add(BaseMaterial);
                BaseMaterial->UpdateCachedExpressionData();
            }
#endif
            // Nanite tessellation displaces the rendered surface away from the fallback triangles.
            const bool Displaced=Nanite==ENaniteProof::ExactFallback && BaseMaterial
                && (Material->IsTessellationEnabled() || BaseMaterial->HasDisplacementConnected());
            const bool Opaque=BaseMaterial && Material->GetBlendMode()==BLEND_Opaque
                && !BaseMaterial->HasVertexPositionOffsetConnected() && !BaseMaterial->HasPixelDepthOffsetConnected() && !Displaced;
            if (OutDiagnostics)
            {
                auto Item=MakeShared<FJsonObject>();
                Item->SetNumberField(TEXT("first_index"),Section.FirstIndex);
                Item->SetNumberField(TEXT("triangles"),Section.NumTriangles);
                Item->SetStringField(TEXT("material"),Material ? Material->GetPathName():TEXT("no-material"));
                Item->SetNumberField(TEXT("blend_mode"),Material ? static_cast<int32>(Material->GetBlendMode()):-1);
                Item->SetBoolField(TEXT("opaque_certificate"),Opaque);
                Item->SetBoolField(TEXT("two_sided"),Material && Material->IsTwoSided());
                Item->SetBoolField(TEXT("vertex_offset"),BaseMaterial && BaseMaterial->HasVertexPositionOffsetConnected());
                Item->SetBoolField(TEXT("depth_offset"),BaseMaterial && BaseMaterial->HasPixelDepthOffsetConnected());
                if (Nanite==ENaniteProof::ExactFallback) Item->SetBoolField(TEXT("nanite_displacement"),Displaced);
                SectionDiagnostics.Add(MakeShared<FJsonValueObject>(Item));
            }
            HashText(Hash,Opaque ? TEXT("opaque") : TEXT("transparent-or-masked"));
            if (!Opaque) continue;
            for (uint32 Triangle=0;Triangle<Section.NumTriangles;++Triangle)
            {
                FRetakeSightPolygon P; P.bTwoSided=Material->IsTwoSided(); P.LOD=LODIndex;
                for (uint32 Corner=0;Corner<3;++Corner)
                {
                    const uint32 Index=Indices[Section.FirstIndex+Triangle*3+Corner];
                    const FVector V(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Index));
                    P.Vertices.Add(V); HashText(Hash,FString::Printf(TEXT("%.6f,%.6f,%.6f"),V.X,V.Y,V.Z));
                }
                // Unreal front faces use the reverse of the mathematical triangle normal.
                Swap(P.Vertices[1],P.Vertices[2]);
                P.Normal=FVector::CrossProduct(P.Vertices[1]-P.Vertices[0],P.Vertices[2]-P.Vertices[0]).GetSafeNormal();
                HashText(Hash,P.bTwoSided ? TEXT("two-sided") : TEXT("one-sided"));
                if (!P.Normal.IsNearlyZero()) Entry.LocalPolygons.Add(MoveTemp(P));
            }
        }
        }
        if (!Entry.LocalPolygons.IsEmpty()) { Entry.MergeCoplanarPolygons(); Entry.BuildIndex(); Meshes.Add(MoveTemp(Entry)); }
    }
    uint8 Digest[16]; Hash.Final(Digest); Fingerprint=BytesToHex(Digest,16).ToLower();
    if (OutDiagnostics)
    {
        auto Report=MakeShared<FJsonObject>();
        Report->SetStringField(TEXT("map"),UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()));
        Report->SetStringField(TEXT("fingerprint"),Fingerprint);
        Report->SetArrayField(TEXT("components"),Checkpoints);
        FJsonSerializer::Serialize(Report,TJsonWriterFactory<>::Create(OutDiagnostics));
    }
    return true;
}

FString URetakeVisibilityData::AutomaticAssetPath(const FString& MapPackage, const FString& ResourceFolder)
{
    const FString Name=TEXT("DA_Visibility_")+FPaths::GetCleanFilename(MapPackage)+TEXT("_")+FMD5::HashAnsiString(*MapPackage).Left(12);
    return (ResourceFolder / Name)+TEXT(".")+Name;
}

bool URetakeVisibilityData::ValidateIdentity(const FString& Map,const FString& Fingerprint,FString& Error) const
{
    if (Version!=CurrentVersion || MapPackage!=Map || GeometryFingerprint!=Fingerprint || Cells.IsEmpty() || Cells.Num()>4096
        || SourceFingerprint.Len()!=32 || RuntimeMeshes.IsEmpty()
        || !FPackageName::IsValidLongPackageName(Map) || Fingerprint.Len()!=32)
    { Error=TEXT("Visibility bake missing, incompatible or stale. Export and bake the current map before competitive admission."); return false; }
    Error.Reset(); return true;
}

bool URetakeVisibilityData::Validate(const FString& Map,const FString& Fingerprint,FString& Error) const
{
    TRACE_CPUPROFILER_EVENT_SCOPE(RetakeVisibility_ValidateBake);
    if (!ValidateIdentity(Map,Fingerprint,Error)) return false;
    TSet<FString> Components;
    for (const auto& Mesh:RuntimeMeshes)
    {
        if (Mesh.ComponentPath.IsEmpty() || Components.Contains(Mesh.ComponentPath) || Mesh.NumLODs<=0 || Mesh.NumLODs>32 || Mesh.LocalPolygons.IsEmpty())
        { Error=TEXT("Invalid visibility runtime mesh."); return false; }
        Components.Add(Mesh.ComponentPath);
        for (const auto& Polygon:Mesh.LocalPolygons)
        {
            if (Polygon.LOD<0 || Polygon.LOD>=Mesh.NumLODs || Polygon.Vertices.Num()<3 || !Polygon.Normal.IsNormalized())
            { Error=TEXT("Invalid visibility proof polygon."); return false; }
            for (const FVector& Vertex:Polygon.Vertices)
                if (Vertex.ContainsNaN() || FMath::Abs(FVector::DotProduct(Vertex-Polygon.Vertices[0],Polygon.Normal))>.01)
                { Error=TEXT("Non-finite or nonplanar visibility proof polygon."); return false; }
            for (int32 I=0;I<Polygon.Vertices.Num();++I)
                if (FVector::DotProduct(FVector::CrossProduct(Polygon.Vertices[(I+1)%Polygon.Vertices.Num()]-Polygon.Vertices[I],
                    Polygon.Vertices[(I+2)%Polygon.Vertices.Num()]-Polygon.Vertices[(I+1)%Polygon.Vertices.Num()]),Polygon.Normal)<-1.e-6)
                { Error=TEXT("Non-convex visibility proof polygon."); return false; }
        }
    }
    for (const auto& Cell:Cells)
    {
        if (!Cell.Bounds.IsValid || Cell.Bounds.Min.ContainsNaN() || Cell.Bounds.Max.ContainsNaN() || Cell.HiddenRuns.Num()%2)
        { Error=TEXT("Invalid visibility cell."); return false; }
        int32 End=0;
        for (int32 I=0;I<Cell.HiddenRuns.Num();I+=2)
        {
            const int32 Start=Cell.HiddenRuns[I],Count=Cell.HiddenRuns[I+1];
            if (Start<End || Count<=0 || Start>=Cells.Num() || Count>Cells.Num()-Start)
            { Error=TEXT("Invalid visibility run."); return false; }
            End=Start+Count;
        }
    }
    Error.Reset(); return true;
}

#if WITH_EDITOR
EDataValidationResult URetakeVisibilityData::IsDataValid(FDataValidationContext& Context) const
{
    FString Error;
    if (!Validate(MapPackage,GeometryFingerprint,Error)) { Context.AddError(FText::FromString(Error)); return EDataValidationResult::Invalid; }
    return EDataValidationResult::Valid;
}
#endif

bool URetakeVisibilityData::CertifiesHidden(const FBox& Observer,const FBox& Subject) const
{
    int32 A=INDEX_NONE,B=INDEX_NONE;
    for (int32 I=0;I<Cells.Num();++I)
    {
        if (Cells[I].Bounds.IsInsideOrOn(Observer.Min) && Cells[I].Bounds.IsInsideOrOn(Observer.Max)) A=I;
        if (Cells[I].Bounds.IsInsideOrOn(Subject.Min) && Cells[I].Bounds.IsInsideOrOn(Subject.Max)) B=I;
    }
    if (A==INDEX_NONE || B==INDEX_NONE) return false;
    const auto& Runs=Cells[A].HiddenRuns;
    for (int32 I=0;I+1<Runs.Num();I+=2) if (B>=Runs[I] && B<Runs[I]+Runs[I+1]) return true;
    return false;
}

FString URetakeVisibilityData::ExportScene(const UObject* Context)
{
    UWorld* World=GEngine->GetWorldFromContextObject(Context,EGetWorldErrorMode::ReturnNull);
    TArray<FRetakeOcclusionMesh> Meshes; FString Fingerprint,Error,Diagnostics,SourceHash;
    auto Root=MakeShared<FJsonObject>();
    if (!RetakeVisibilityScene::Capture(World,Meshes,Fingerprint,Error,&Diagnostics,true)
        || !RetakeVisibilityScene::SourceFingerprint(World,SourceHash,Error)) Root->SetStringField(TEXT("error"),Error);
    else
    {
        Root->SetNumberField(TEXT("version"),CurrentVersion);
        Root->SetStringField(TEXT("map"),UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()));
        Root->SetStringField(TEXT("fingerprint"),Fingerprint);
        Root->SetStringField(TEXT("source_fingerprint"),SourceHash);
        TArray<TSharedPtr<FJsonValue>> Runtime;
        for (const auto& Entry:Meshes)
        {
            FRetakeVisibilityRuntimeMesh Saved; Saved.ComponentPath=Entry.Component->GetPathName(World);
            Saved.NumLODs=Entry.NumLODs; Saved.bDynamic=Entry.bDynamic; Saved.LocalPolygons=Entry.LocalPolygons;
            Runtime.Add(MakeShared<FJsonValueObject>(FJsonObjectConverter::UStructToJsonObject(Saved)));
        }
        Root->SetArrayField(TEXT("runtime_meshes"),Runtime);
        TSharedPtr<FJsonObject> Report;
        if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Diagnostics),Report)) Root->SetObjectField(TEXT("diagnostics"),Report);
        TArray<TSharedPtr<FJsonValue>> Polygons;
        for (const auto& Entry:Meshes) if (!Entry.bDynamic && Entry.Component.IsValid())
            for (const auto& Local:Entry.LocalPolygons)
            {
                const auto P=RetakeVisibilityScene::ToWorld(Local,Entry.Component->GetComponentTransform());
                auto J=MakeShared<FJsonObject>(); TArray<TSharedPtr<FJsonValue>> Vertices;
                for (const auto& V:P.Vertices) Vertices.Add(VectorJSON(V));
                J->SetArrayField(TEXT("vertices"),Vertices); J->SetField(TEXT("normal"),VectorJSON(P.Normal));
                J->SetStringField(TEXT("mesh"),Entry.Component->GetPathName(World));
                J->SetNumberField(TEXT("lod"),P.LOD); J->SetNumberField(TEXT("lod_count"),Entry.NumLODs);
                J->SetBoolField(TEXT("two_sided"),P.bTwoSided); Polygons.Add(MakeShared<FJsonValueObject>(J));
            }
        Root->SetArrayField(TEXT("polygons"),Polygons);
    }
    FString JSON; FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&JSON)); return JSON;
}

bool URetakeVisibilityData::ImportBakedJSON(const FString& JSON,FString& Error)
{
#if WITH_EDITOR
    TSharedPtr<FJsonObject> Root;
    if (!IsInGameThread() || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JSON),Root) || !Root)
    { Error=TEXT("Invalid bake JSON."); return false; }
    auto* Candidate=NewObject<URetakeVisibilityData>();
    const TArray<TSharedPtr<FJsonValue>>* Meshes=nullptr;
    const TArray<TSharedPtr<FJsonValue>>* CellValues=nullptr;
    if (!Root->TryGetNumberField(TEXT("version"),Candidate->Version) || Candidate->Version!=CurrentVersion
        || !Root->TryGetStringField(TEXT("map"),Candidate->MapPackage)
        || !Root->TryGetStringField(TEXT("fingerprint"),Candidate->GeometryFingerprint)
        || !Root->TryGetStringField(TEXT("source_fingerprint"),Candidate->SourceFingerprint)
        || !Root->TryGetArrayField(TEXT("runtime_meshes"),Meshes) || !Root->TryGetArrayField(TEXT("cells"),CellValues))
    { Error=TEXT("Bake JSON is missing its source manifest or runtime geometry."); return false; }
    for (const auto& Value:*Meshes)
    {
        const TSharedPtr<FJsonObject>* Object=nullptr;
        if (!Value->TryGetObject(Object) || !FJsonObjectConverter::JsonObjectToUStruct(Object->ToSharedRef(),&Candidate->RuntimeMeshes.AddDefaulted_GetRef(),0,0))
        { Error=TEXT("Invalid runtime mesh JSON."); return false; }
    }
    auto Vector=[](const TSharedPtr<FJsonValue>& Value,FVector& Out)
    {
        const TArray<TSharedPtr<FJsonValue>>* Axes=nullptr;
        if (!Value->TryGetArray(Axes) || Axes->Num()!=3) return false;
        for (int32 I=0;I<3;++I) if (!(*Axes)[I]->TryGetNumber(Out[I]) || !FMath::IsFinite(Out[I])) return false;
        return true;
    };
    for (const auto& Value:*CellValues)
    {
        const TSharedPtr<FJsonObject>* Object=nullptr;
        const TArray<TSharedPtr<FJsonValue>>* Bounds=nullptr;
        const TArray<TSharedPtr<FJsonValue>>* Runs=nullptr;
        FVector Min,Max;
        if (!Value->TryGetObject(Object) || !(*Object)->TryGetArrayField(TEXT("bounds"),Bounds) || Bounds->Num()!=2
            || !Vector((*Bounds)[0],Min) || !Vector((*Bounds)[1],Max) || (Max-Min).GetMin()<=0
            || !(*Object)->TryGetArrayField(TEXT("hidden_runs"),Runs))
        { Error=TEXT("Invalid visibility cell JSON."); return false; }
        auto& Cell=Candidate->Cells.AddDefaulted_GetRef(); Cell.Bounds=FBox(Min,Max);
        for (const auto& Run:*Runs)
        {
            double Number=0;
            if (!Run->TryGetNumber(Number) || Number<0 || Number>MAX_int32 || Number!=FMath::FloorToDouble(Number))
            { Error=TEXT("Invalid visibility run JSON."); return false; }
            Cell.HiddenRuns.Add(static_cast<int32>(Number));
        }
    }
    if (!Candidate->Validate(Candidate->MapPackage,Candidate->GeometryFingerprint,Error)) return false;
    Modify(); Version=Candidate->Version; MapPackage=Candidate->MapPackage;
    GeometryFingerprint=Candidate->GeometryFingerprint; SourceFingerprint=Candidate->SourceFingerprint;
    Cells=MoveTemp(Candidate->Cells); RuntimeMeshes=MoveTemp(Candidate->RuntimeMeshes);
    MarkPackageDirty(); return true;
#else
    Error=TEXT("Bake import is available only in editor builds."); return false;
#endif
}
