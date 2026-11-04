// Copyright 2026 Codnyx. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "RetakeDestructionConversionMenu.h"

/** Adds the level-editor conversion menu for as long as the module is loaded. */
class FRetakeDestructionEditorModule : public IModuleInterface
{
    virtual void StartupModule() override { RetakeDestructionConversionMenu::Startup(); }
    virtual void ShutdownModule() override { RetakeDestructionConversionMenu::Shutdown(); }
};

IMPLEMENT_MODULE(FRetakeDestructionEditorModule, RetakeDestructionEditor)
