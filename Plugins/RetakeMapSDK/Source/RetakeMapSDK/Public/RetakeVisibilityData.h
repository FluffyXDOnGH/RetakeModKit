// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "RetakeVisibilityGeometry.h"
#include "RetakeVisibilityData.generated.h"

class UStaticMeshComponent;
class UStaticMesh;
class UMaterialInterface;
struct FRetakeOcclusionMesh;

/** Cached server diagnostics; not replicated or reflected into client state. */
struct FRetakeFloorSupportStats
{
    int32 CandidateTriangles=0,PatchTriangles=0,OpenEdges=0;
    FBox Footprint=FBox(ForceInit);
};

/** Cookable proof geometry. Dedicated servers do not need render buffers or shader graphs. */
USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeVisibilityRuntimeMesh
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, Category="Retake|Network") FString ComponentPath;
    UPROPERTY(EditAnywhere, Category="Retake|Network") int32 NumLODs=0;
    UPROPERTY(EditAnywhere, Category="Retake|Network") bool bDynamic=false;
    UPROPERTY(EditAnywhere, Category="Retake|Network") TArray<FRetakeSightPolygon> LocalPolygons;
};

USTRUCT(BlueprintType)
struct RETAKEMAPSDK_API FRetakeVisibilityCell
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, Category="Retake|Network") FBox Bounds = FBox(ForceInit);
    /** Start/count pairs of certified hidden destination cell indices. Everything else is potentially visible. */
    UPROPERTY(EditAnywhere, Category="Retake|Network") TArray<int32> HiddenRuns;
};

/** Versioned scene proof. Generated data is never accepted just because its filename matches. */
UCLASS(BlueprintType)
class RETAKEMAPSDK_API URetakeVisibilityData : public UDataAsset
{
    GENERATED_BODY()
public:
    static constexpr int32 CurrentVersion = 3;
    UPROPERTY(EditAnywhere, Category="Retake|Network") int32 Version = CurrentVersion;
    UPROPERTY(EditAnywhere, Category="Retake|Network", AssetRegistrySearchable) FString MapPackage;
    UPROPERTY(EditAnywhere, Category="Retake|Network") FString GeometryFingerprint;
    /** Mesh/material authoring revisions and component state; contains no renderer cache state. */
    UPROPERTY(EditAnywhere, Category="Retake|Network") FString SourceFingerprint;
    /** Authoring cache identity; never used as a substitute for runtime source validation. */
    UPROPERTY(VisibleAnywhere, Category="Retake|Network") FString BakeInputFingerprint;
    /** Stable authoring destination in a caller-owned resource folder; the SDK never assumes a private game path. */
    static FString AutomaticAssetPath(const FString& MapPackage, const FString& ResourceFolder);
    UPROPERTY(EditAnywhere, Category="Retake|Network") TArray<FRetakeVisibilityCell> Cells;
    UPROPERTY(EditAnywhere, Category="Retake|Network") TArray<FRetakeVisibilityRuntimeMesh> RuntimeMeshes;
    bool Validate(const FString& Map, const FString& Fingerprint, FString& Error) const;
    /** Validate's cheap identity part (version, map, fingerprints, non-empty data), without the walk over every proof polygon. */
    bool ValidateIdentity(const FString& Map, const FString& Fingerprint, FString& Error) const;
    bool RestoreWorld(UWorld* World,TArray<FRetakeOcclusionMesh>& OutMeshes,FString& Error,FString* Diagnostics=nullptr) const;
    bool CertifiesHidden(const FBox& Observer, const FBox& Subject) const;
    /** Native geometry read; used through MCP by the offline bake exporter. Does not modify the world. */
    UFUNCTION(BlueprintCallable, Category="Retake|Network", meta=(WorldContext="WorldContext"))
    static FString ExportScene(const UObject* WorldContext);
    /** Editor authoring only; rejects invalid input before replacing any asset fields. */
    UFUNCTION(BlueprintCallable, Category="Retake|Network")
    bool ImportBakedJSON(const FString& JSON,FString& Error);
#if WITH_EDITOR
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};

/** One static mesh component's proof polygons (component space) and the state they were captured from. */
struct RETAKEMAPSDK_API FRetakeOcclusionMesh : FRetakeSightPolygonIndex
{
    TWeakObjectPtr<UStaticMeshComponent> Component;
    TWeakObjectPtr<UStaticMesh> SourceMesh;
    TArray<TWeakObjectPtr<UMaterialInterface>> SourceMaterials;
    /** The component's override slots when its materials last matched SourceMaterials (bOverridesCaptured). While they and
     *  the mesh are unchanged, GetMaterial returns SourceMaterials, so validation skips the per-slot resolution. */
    TArray<TWeakObjectPtr<UMaterialInterface>> SourceOverrides;
    bool bOverridesCaptured = false;
    FTransform SourceTransform=FTransform::Identity;
    /** LocalPolygons placed by SourceTransform, built once for static meshes. Valid only while the component's transform
     *  equals SourceTransform exactly (then RetakeVisibilityScene::ToWorld would produce the same values); empty for doors. */
    TArray<FRetakeSightPolygon> WorldPolygons;
    bool bDynamic = false;
    bool bTrusted = true;
    int32 NumLODs=0;
};

namespace RetakeVisibilityScene
{
    RETAKEMAPSDK_API void GatherComponents(UWorld* World,TArray<UStaticMeshComponent*>& Out);
    RETAKEMAPSDK_API bool SourceFingerprint(UWorld* World,FString& Fingerprint,FString& Error,FString* Diagnostics=nullptr);
    /** The source manifest's text for serialized doubles: each value's exact little-endian IEEE-754 bits as lowercase hex,
     *  with -0.0 written as +0.0. Signed zero is decided on the integer bits, because `Value + 0.0` is compiled away by the
     *  fast floating-point math of the Win64 game and server targets (/fp:fast) and so never folded there (alpha.31). */
    RETAKEMAPSDK_API FString CanonicalDoubleBits(TConstArrayView<double> Values);
    /** Retains only geometry whose actual opaque triangles can provide a visibility proof.
     *  bBakeExport also admits Nanite meshes with an exact (unreduced) fallback; live, unbaked captures keep them out. */
    RETAKEMAPSDK_API bool Capture(UWorld* World, TArray<FRetakeOcclusionMesh>& OutMeshes, FString& OutFingerprint, FString& Error, FString* OutDiagnostics = nullptr, bool bBakeExport = false);
    RETAKEMAPSDK_API FRetakeSightPolygon ToWorld(const FRetakeSightPolygon& Local, const FTransform& Transform);
}
