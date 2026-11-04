// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeVisibilityData.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "StaticParameterSet.h"
#include "RetakeDoor.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
void Add(FMD5& Hash,const FString& Value)
{
    FTCHARToUTF8 Bytes(*Value); Hash.Update(reinterpret_cast<const uint8*>(Bytes.Get()),Bytes.Length());
    const uint8 Separator=0; Hash.Update(&Separator,1);
}
FString Finish(FMD5 Hash)
{
    uint8 Digest[16]; Hash.Final(Digest); return BytesToHex(Digest,16).ToLower();
}
bool MaterialRevision(const UMaterialInterface* Material,FString& Result)
{
    FMD5 Hash; Add(Hash,TEXT("RetakeMaterialSource:1"));
    TSet<const UMaterialInterface*> Visited;
    while (Material)
    {
        if (Visited.Contains(Material) || Visited.Num()>=32) return false;
        Visited.Add(Material); Add(Hash,Material->GetPathName());
        if (const auto* Instance=Cast<UMaterialInstance>(Material))
        {
            // Hash authored override values only. Cached inherited values differ on
            // headless processes and are not source data.
            const auto& Overrides=Instance->BasePropertyOverrides;
            const auto* Type=FMaterialInstanceBasePropertyOverrides::StaticStruct();
            for (TFieldIterator<FProperty> It(Type);It;++It)
            {
                const FString Name=It->GetName(); if (!Name.StartsWith(TEXT("bOverride_"))) continue;
                bool Enabled=false;
                if (const auto* Flag=CastField<FBoolProperty>(*It)) Enabled=Flag->GetPropertyValue_InContainer(&Overrides);
                else if (const auto* Mask=CastField<FNumericProperty>(*It))
                    Enabled=Mask->GetUnsignedIntPropertyValue(It->ContainerPtrToValuePtr<void>(&Overrides))!=0;
                Add(Hash,Name); Add(Hash,Enabled ? TEXT("1"):TEXT("0"));
                if (!Enabled) continue;
                FString Value; It->ExportText_InContainer(0,Value,&Overrides,nullptr,nullptr,PPF_None); Add(Hash,Value);
                if (const auto* Property=FindFProperty<FProperty>(Type,FName(*Name.RightChop(10))))
                { Value.Reset(); Property->ExportText_InContainer(0,Value,&Overrides,nullptr,nullptr,PPF_None); Add(Hash,Value); }
            }
            const auto Parameters=Instance->GetStaticParameters();
            FString StaticValues;
            FStaticParameterSetRuntimeData::StaticStruct()->ExportText(StaticValues,&Parameters.GetRuntime(),nullptr,nullptr,PPF_None,nullptr);
            Add(Hash,StaticValues);
            Material=Instance->Parent;
        }
        else if (const auto* Base=Cast<UMaterial>(Material))
        {
            if (!Base->StateId.IsValid()) return false;
            Add(Hash,Base->StateId.ToString());
            Result=Finish(Hash); return true;
        }
        else return false;
    }
    return false;
}
// Runtime admission runs in cooked servers and clients (WITH_EDITORONLY_DATA=0): GetLightingGuid() is a null GUID there
// and dedicated servers strip render buffers. ExtendedBounds and bAllowCPUAccess are serialized UPROPERTYs that the
// cooker writes as the editor holds them, so both processes hash the same bits. Never use editor-only data here.
// Geometry inside unchanged bounds is covered at bake/cook time by GeometryFingerprint (every exported proof vertex).
FString MeshIdentity(const UStaticMesh* Mesh)
{
    const FBoxSphereBounds& Bounds=Mesh->GetExtendedBounds();
    // Exact IEEE-754 bits: decimal text could differ between platform runtimes. Signed zero is canonicalized on the bits.
    const double Values[7]={Bounds.Origin.X,Bounds.Origin.Y,Bounds.Origin.Z,Bounds.BoxExtent.X,Bounds.BoxExtent.Y,Bounds.BoxExtent.Z,Bounds.SphereRadius};
    return RetakeVisibilityScene::CanonicalDoubleBits(Values)+(Mesh->bAllowCPUAccess ? TEXT(":cpu"):TEXT(":gpu"));
}
// Placement as the exact bits of the serialized relative transforms of the component and each attach ancestor. Never
// hash GetComponentTransform().ToString(): it re-derives a rotator from the world quaternion through asin/atan2 over
// rounding-sensitive products, and the sign of a near-zero angle differs by platform. Cooked Linux printed "-0.000000"
// where the Win64 editor printed "0.000000" (alpha.18 Stalingrad, 4 of 2,420 components), refusing the whole bake.
// Gathered non-door components are Static, and Static cannot attach to a non-static parent, so these UPROPERTYs fully
// determine placement. Parent socket offsets are asset data, covered at bake/cook time by GeometryFingerprint.
// Saved values can be -0.0: Stalingrad's StaticMeshActor_85 rotation is (1e-06, -0.0, -270.000001). The editor and the
// Linux server folded it with `Value + 0.0`, but /fp:fast compiles that addition away in the Win64 game, so every Win64
// standalone or listen load of Stalingrad called the bake stale (Sentry: alpha.23 to alpha.31). CanonicalDoubleBits decides
// on the bits.
FString Placement(const USceneComponent* Component,const UWorld* World)
{
    FString Result;
    for (const USceneComponent* It=Component;It;It=It->GetAttachParent())
    {
        const FVector L=It->GetRelativeLocation(),S=It->GetRelativeScale3D(); const FRotator R=It->GetRelativeRotation();
        const double Values[9]={L.X,L.Y,L.Z,R.Pitch,R.Yaw,R.Roll,S.X,S.Y,S.Z};
        const int32 Absolute=(It->IsUsingAbsoluteLocation() ? 1:0)|(It->IsUsingAbsoluteRotation() ? 2:0)|(It->IsUsingAbsoluteScale() ? 4:0);
        if (It!=Component) Result+=TEXT("<")+It->GetPathName(World)+TEXT(">");
        Result+=RetakeVisibilityScene::CanonicalDoubleBits(Values);
        Result+=FString::Printf(TEXT(":%d:%s;"),Absolute,*It->GetAttachSocketName().ToString());
    }
    return Result;
}
}

FString RetakeVisibilityScene::CanonicalDoubleBits(TConstArrayView<double> Values)
{
    static_assert(PLATFORM_LITTLE_ENDIAN,"The visibility source manifest hashes little-endian double bits.");
    // Integer work only, so no floating-point mode can change the result: +0.0 and -0.0 are the only values whose bits
    // are all clear apart from the sign. Every other value, denormals and NaN payloads included, keeps its exact bits.
    TArray<uint64,TInlineAllocator<16>> Bits;
    Bits.Reserve(Values.Num());
    for (const double Value:Values)
    {
        uint64 Raw=0; FMemory::Memcpy(&Raw,&Value,sizeof(Raw));
        Bits.Add((Raw<<1)==0 ? 0:Raw);
    }
    return BytesToHex(reinterpret_cast<const uint8*>(Bits.GetData()),Bits.Num()*static_cast<int32>(sizeof(uint64))).ToLower();
}

void RetakeVisibilityScene::GatherComponents(UWorld* World,TArray<UStaticMeshComponent*>& Out)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(RetakeVisibility_GatherComponents);
    Out.Reset(); if (!World) return;
    for (TActorIterator<AActor> It(World);It;++It)
    {
        if (It->IsEditorOnly() || It->IsHidden() || It->ActorHasTag(TEXT("Retake.Visibility.Ignore"))) continue;
        TInlineComponentArray<UStaticMeshComponent*> Found(*It);
        for (auto* Component:Found)
            if (Component && !Component->IsEditorOnly() && Component->GetStaticMesh() && Component->IsVisible() && !Component->bHiddenInGame
                && !Component->ComponentHasTag(TEXT("Retake.Visibility.Ignore"))
                && (Component->Mobility==EComponentMobility::Static || It->IsA<ARetakeDoor>())) Out.Add(Component);
    }
    Out.Sort([World](const auto& A,const auto& B){return A.GetPathName(World)<B.GetPathName(World);});
}

bool RetakeVisibilityScene::SourceFingerprint(UWorld* World,FString& Fingerprint,FString& Error,FString* Diagnostics)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(RetakeVisibility_SourceFingerprint);
    Fingerprint.Reset(); Error.Reset(); if (Diagnostics) Diagnostics->Reset();
    if (!World) { Error=TEXT("No world for visibility source manifest."); return false; }
    // Version 2: cooked-stable mesh identity instead of the editor-only lighting GUID. Version 3: serialized placement
    // bits instead of transform text. Older bakes report stale until packaging refreshes their source fingerprint.
    // Still version 3: canonicalizing signed zero on the bits (2026-10-26) reproduces the Linux server, which folded every
    // value. The fast-math Win64 game and server folded none, and the editor's MeshIdentity code skipped four of the seven
    // bounds values; no Stalingrad mesh has a -0.0 bound, so its bake keeps its source fingerprint.
    FMD5 Hash; Add(Hash,TEXT("RetakeVisibilitySources:3"));
    TArray<UStaticMeshComponent*> Components; GatherComponents(World,Components);
    TMap<const UMaterialInterface*,FString> Materials;
    TArray<TSharedPtr<FJsonValue>> Rows;
    for (auto* Component:Components)
    {
        const UStaticMesh* Mesh=Component->GetStaticMesh();
#if WITH_EDITORONLY_DATA
        // Authoring guard only, never hashed: a mesh without a lighting GUID was never saved, so the cook would not see
        // the content exported now.
        if (!Mesh->GetLightingGuid().IsValid()) { Error=TEXT("Save the mesh before baking: ")+Mesh->GetPathName(); return false; }
#endif
        const FString Path=Component->GetPathName(World),Class=Component->GetClass()->GetPathName(),Identity=MeshIdentity(Mesh);
        const FString Where=Component->GetOwner()->IsA<ARetakeDoor>() ? FString(TEXT("dynamic-door")):Placement(Component,World);
        const FString Draw=FString::Printf(TEXT("%d:%g:%g"),static_cast<int32>(Component->Mobility),Component->MinDrawDistance,Component->LDMaxDrawDistance);
        Add(Hash,Path); Add(Hash,Class); Add(Hash,Mesh->GetPathName()); Add(Hash,Identity); Add(Hash,Where); Add(Hash,Draw);
        Add(Hash,FString::FromInt(Component->GetNumMaterials()));
        TArray<TSharedPtr<FJsonValue>> MaterialRows;
        for (int32 I=0;I<Component->GetNumMaterials();++I)
        {
            const auto* Material=Component->GetMaterial(I);
            FString Revision=TEXT("no-material");
            if (Material)
            {
                if (const auto* Found=Materials.Find(Material)) Revision=*Found;
                else
                {
                    if (!MaterialRevision(Material,Revision)) { Error=TEXT("Unverifiable material source: ")+Material->GetPathName(); return false; }
                    Materials.Add(Material,Revision);
                }
            }
            Add(Hash,Revision);
            if (Diagnostics) MaterialRows.Add(MakeShared<FJsonValueString>((Material ? Material->GetPathName():TEXT("none"))+TEXT(":")+Revision));
        }
        if (Diagnostics)
        {
            auto Row=MakeShared<FJsonObject>(); Row->SetStringField(TEXT("component"),Path);
            Row->SetStringField(TEXT("class"),Class); Row->SetStringField(TEXT("mesh"),Mesh->GetPathName());
            Row->SetStringField(TEXT("mesh_identity"),Identity); Row->SetStringField(TEXT("placement"),Where); Row->SetStringField(TEXT("draw"),Draw);
            Row->SetStringField(TEXT("hash_after"),Finish(Hash)); Row->SetArrayField(TEXT("materials"),MaterialRows);
            Rows.Add(MakeShared<FJsonValueObject>(Row));
        }
    }
    Fingerprint=Finish(Hash);
    if (Diagnostics)
    {
        auto Root=MakeShared<FJsonObject>(); Root->SetStringField(TEXT("map"),UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()));
        Root->SetStringField(TEXT("fingerprint"),Fingerprint); Root->SetStringField(TEXT("kind"),TEXT("source-manifest-v3"));
        Root->SetArrayField(TEXT("components"),Rows); FJsonSerializer::Serialize(Root,TJsonWriterFactory<>::Create(Diagnostics));
    }
    return true;
}

bool URetakeVisibilityData::RestoreWorld(UWorld* World,TArray<FRetakeOcclusionMesh>& Out,FString& Error,FString* Diagnostics) const
{
    TRACE_CPUPROFILER_EVENT_SCOPE(RetakeVisibility_RestoreWorld);
    Out.Reset();
    if (!World) return false;
    // Identity and source manifest first, so a stale bake is rejected without Validate's walk over every proof polygon.
    const FString Map=UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());
    if (!ValidateIdentity(Map,GeometryFingerprint,Error)) return false;
    FString Current;
    if (!RetakeVisibilityScene::SourceFingerprint(World,Current,Error,Diagnostics)) return false;
    if (Current!=SourceFingerprint) { Error=TEXT("Visibility source manifest is stale. Export current sources and validate the bake before admission."); return false; }
    if (!Validate(Map,GeometryFingerprint,Error)) return false;
    TArray<UStaticMeshComponent*> Components; RetakeVisibilityScene::GatherComponents(World,Components);
    TMap<FString,UStaticMeshComponent*> ByPath;
    for (auto* Component:Components) ByPath.Add(Component->GetPathName(World),Component);
    for (const auto& Saved:RuntimeMeshes)
    {
        auto* const* Found=ByPath.Find(Saved.ComponentPath);
        if (!Found || Saved.bDynamic!=(*Found)->GetOwner()->IsA<ARetakeDoor>())
        { Out.Reset(); Error=TEXT("Visibility runtime component does not match the bake: ")+Saved.ComponentPath; return false; }
        FRetakeOcclusionMesh Entry; Entry.Component=*Found; Entry.SourceMesh=(*Found)->GetStaticMesh();
        Entry.SourceTransform=(*Found)->GetComponentTransform(); Entry.bDynamic=Saved.bDynamic;
        Entry.NumLODs=Saved.NumLODs; Entry.LocalPolygons=Saved.LocalPolygons;
        for (int32 I=0;I<(*Found)->GetNumMaterials();++I) Entry.SourceMaterials.Add((*Found)->GetMaterial(I));
        Entry.BuildIndex(); Out.Add(MoveTemp(Entry));
    }
    return true;
}
