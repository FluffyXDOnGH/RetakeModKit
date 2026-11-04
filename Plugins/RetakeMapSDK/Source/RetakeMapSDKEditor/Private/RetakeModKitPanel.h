// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
class SWidget;

namespace RetakeModKitPanel
{
    TSharedRef<SWidget> Create(FSimpleDelegate CreateMod);
    void SelectWorkspace(const FString& ModId);
    void Action(const FString& Name);
    FString SelectedKind();
    bool IsMapWorkflow();
    void Shutdown();
}
