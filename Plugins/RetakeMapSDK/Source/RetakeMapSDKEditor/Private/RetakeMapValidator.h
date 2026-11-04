// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "RetakeMapSDKModeRequirements.h"

class UWorld;
struct FRetakeModMap;

struct FRetakeMapIssue
{
    bool bError = true;
    FString Code;
    FString Actor;
    FString Detail;
};

/**
 * Map Kit validation without the game (MKD-036): the level shape, every SDK element's own checks (IsDataValid) and the content
 * each mode needs (RetakeMapSDK::ModeRequirements). Walks the persistent level of an editor world; it never needs play.
 */
namespace RetakeMapValidator
{
    /** Level shape and element checks shared by every mode. */
    void ValidateWorld(const UWorld& World, TArray<FRetakeMapIssue>& Out);
    /** Content Mode needs; true when it adds no error. */
    bool ValidateMode(const UWorld& World, const RetakeMapSDK::FModeRequirement& Mode, TArray<FRetakeMapIssue>& Out);
    /** Part of ValidateWorld: ambient sounds and ambience zones (Audio), collision surfaces (Surface), pain and Kill Z (Volume), and
     *  minefields over starts or objective zones (Minefield). */
    void ValidateEnvironment(const UWorld& World, TArray<FRetakeMapIssue>& Out);
    /**
     * A mod map's presentation (manifest schema 2): its Retake Map Settings display name, description, loading art, preview and
     * default ambience, its baked minimap, and the mod's physical materials. Fills OutMap's DisplayName, Description, LoadingArt,
     * Preview, Minimap and Ambience; ModRoot is "/<ModId>/". True when it adds no error. Validation and export both call it.
     */
    bool ValidatePresentation(const UWorld& World, const FString& ModRoot, FRetakeModMap& OutMap, TArray<FRetakeMapIssue>& Out);
}
