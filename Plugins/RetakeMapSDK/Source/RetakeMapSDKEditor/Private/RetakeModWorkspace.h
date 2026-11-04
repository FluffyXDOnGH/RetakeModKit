// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

/** Ordinary creator workspaces. No game project, converted content or migration helpers are required. */
namespace RetakeModWorkspace
{
    struct FKind { const TCHAR* Id; const TCHAR* Name; const TCHAR* Description; };
    const TArray<FKind>& Kinds();
    FString Kind(const FString& ModId);
    bool CanCreateKind(const FString& Kind, FString& Reason);
    FString KindLabel(const FString& Kind);
    /** Current runtime delivery remains map-only; class availability only enables weapon draft authoring. */
    bool CanExportKind(const FString& Kind);
    /** Native data/resource validation only. It never certifies packaging or runtime admission. */
    bool CheckWeaponDraft(const FString& ModId, FString& ReportJson, FString& Error);
    bool Create(const FString& Name, bool bBasicLevel, FString& OutModId, FString& Error, const FString& Kind = TEXT("map"));
    bool Open(const FString& ModId, FString& Error);
    void Reveal(const FString& ModId);
    TArray<FString> List();
}
