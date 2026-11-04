// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UWorld;
class ARetakeMinimapCapture;
class URetakeMinimapDefinition;
class UTexture2D;

/**
 * Native minimap bake for Map Kit mods (-run=RetakeMinimapBake): the upward faces of LOD0 static meshes inside the map's Retake
 * Minimap Capture, drawn from above in the Retake palette with an outline at every floor band change, coverage edge and height
 * step. Works with -NullRHI. Built-in maps keep Scripts/Minimap; the result is the same URetakeMinimapDefinition either way.
 */
namespace RetakeMinimapBaker
{
    /** Changes whenever the same geometry would bake differently; part of every fingerprint, so older bakes read as stale. */
    inline constexpr int32 Version = 1;

    /** Every Retake Minimap Capture in World's persistent level. */
    TArray<const ARetakeMinimapCapture*> FindCaptures(const UWorld& World);
    bool IsCustom(const URetakeMinimapDefinition& Definition);
    FString CustomFingerprint(const UWorld& World, const ARetakeMinimapCapture& Capture, const UTexture2D& Texture);
    /** Applies a creator-owned 1024px image with the capture calibration; never rasterizes or overwrites its source art. */
    bool ApplyCustom(const UWorld& World, const ARetakeMinimapCapture& Capture, UTexture2D& Texture, const FString& ModRoot,
        const FText& DisplayName, FString& OutDefinition, FString& Error);
    /** BLAKE3 over Version, the capture's parameters and the quantized triangles the bake draws. Never rasterizes. */
    FString ComputeFingerprint(const UWorld& World, const ARetakeMinimapCapture& Capture);
    /**
     * Writes and saves <ModRoot>Minimap/T_Minimap_<Map> (1024 px, UI texture settings) and DA_Minimap_<Map> (calibration, DisplayName
     * and fingerprint), replacing an earlier bake in place, and a preview PNG under Saved/RetakeMinimap. OutDefinition is its object path.
     */
    bool Bake(const UWorld& World, const ARetakeMinimapCapture& Capture, const FString& ModRoot, const FText& DisplayName,
        FString& OutDefinition, FString& Error, bool bReplaceCustom = false);
}
