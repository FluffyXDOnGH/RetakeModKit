// Copyright 2026 Codnyx. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "RetakeModWorkspace.h"
#include "RetakeModKitPanel.h"
#include "ToolMenus.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Commands/UIAction.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "RetakeModKitEditor"

namespace
{
const FName WorkspaceTab(TEXT("RetakeModKitWorkspace"));
void ShowCreateMod()
{
    TSharedRef<SWindow> Window=SNew(SWindow).Title(LOCTEXT("CreateTitle","Create Retake Mod"))
        .ClientSize(FVector2D(680,400)).SupportsMaximize(false).SupportsMinimize(false);
    TSharedRef<FString> Kind=MakeShared<FString>(RetakeModKitPanel::SelectedKind());
    TSharedRef<bool> Basic=MakeShared<bool>(false);
    TSharedRef<SEditableTextBox> Name=SNew(SEditableTextBox).HintText_Lambda([Kind]
        {return FText::FromString(*Kind==TEXT("map")?TEXT("MyMap"):*Kind==TEXT("weapons")?TEXT("MyWeaponPack"):TEXT("MyGameMode"));});
    TSharedRef<STextBlock> Error=SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FLinearColor(.95f,.4f,.3f));
    TSharedRef<SHorizontalBox> Types=SNew(SHorizontalBox);
    for(const auto& Entry:RetakeModWorkspace::Kinds())
    {
        const FString Id(Entry.Id);
        Types->AddSlot().FillWidth(1).Padding(0,0,10,0)
        [SNew(SCheckBox).IsChecked_Lambda([Kind,Id]{return *Kind==Id?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
            .OnCheckStateChanged_Lambda([Kind,Id,Error](ECheckBoxState State){if(State==ECheckBoxState::Checked){*Kind=Id;Error->SetText(FText::GetEmpty());}})
            [SNew(STextBlock).Text(FText::FromString(Entry.Name)).Font(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"),11))]];
    }
    const TWeakPtr<SWindow> WeakWindow(Window);
    Window->SetContent(SNew(SBorder).Padding(22)
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(LOCTEXT("KindLabel","What kind of mod are you creating?"))]
            +SVerticalBox::Slot().AutoHeight().Padding(0,12)[Types]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[SNew(STextBlock).AutoWrapText(true).Text_Lambda([Kind]
                {for(const auto& Entry:RetakeModWorkspace::Kinds())if(*Kind==Entry.Id)return FText::FromString(Entry.Description);return FText::GetEmpty();})]
            +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(LOCTEXT("NameLabel","Mod name"))]
            +SVerticalBox::Slot().AutoHeight().Padding(0,7)[Name]
            +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("NameRules","Use letters, numbers and underscores. Each mod gets its own content-only plugin folder."))]
            +SVerticalBox::Slot().AutoHeight().Padding(0,12)
            [SNew(SHorizontalBox).Visibility_Lambda([Kind]{return *Kind==TEXT("map")?EVisibility::Visible:EVisibility::Collapsed;})
                +SHorizontalBox::Slot().AutoWidth().Padding(0,0,18,0)[SNew(SCheckBox).IsChecked_Lambda([Basic]{return *Basic?ECheckBoxState::Unchecked:ECheckBoxState::Checked;})
                    .OnCheckStateChanged_Lambda([Basic](ECheckBoxState State){if(State==ECheckBoxState::Checked)*Basic=false;})[SNew(STextBlock).Text(LOCTEXT("EmptyTemplate","Empty level"))]]
                +SHorizontalBox::Slot().AutoWidth()[SNew(SCheckBox).IsChecked_Lambda([Basic]{return *Basic?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                    .OnCheckStateChanged_Lambda([Basic](ECheckBoxState State){if(State==ECheckBoxState::Checked)*Basic=true;})[SNew(STextBlock).Text(LOCTEXT("BasicTemplate","Basic level"))]]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,8)[SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FLinearColor(.95f,.7f,.3f))
                .Text_Lambda([Kind]{FString Reason;RetakeModWorkspace::CanCreateKind(*Kind,Reason);return FText::FromString(Reason);})]
            +SVerticalBox::Slot().AutoHeight()[Error]
            +SVerticalBox::Slot().FillHeight(1)
            +SVerticalBox::Slot().AutoHeight()[SNew(SButton).HAlign(HAlign_Center)
                .Text_Lambda([Kind]{return FText::FromString(*Kind==TEXT("map")?TEXT("Create map mod"):*Kind==TEXT("weapons")?TEXT("Create weapon draft"):TEXT("Game-mode creation is planned"));})
                .IsEnabled_Lambda([Kind,Name]{FString Reason;return !Name->GetText().IsEmpty()&&RetakeModWorkspace::CanCreateKind(*Kind,Reason);})
                .OnClicked_Lambda([Kind,Basic,Name,Error,WeakWindow]
                {
                    FString ModId,Problem;
                    if(RetakeModWorkspace::Create(Name->GetText().ToString(),*Kind==TEXT("map")&&*Basic,ModId,Problem,*Kind))
                    {RetakeModKitPanel::SelectWorkspace(ModId);if(const auto Pinned=WeakWindow.Pin())Pinned->RequestDestroyWindow();}
                    else Error->SetText(FText::FromString(Problem));
                    return FReply::Handled();
                })]]);
    FSlateApplication::Get().AddWindow(Window);
}

TSharedRef<SDockTab> SpawnWorkspace(const FSpawnTabArgs&)
{
    return SNew(SDockTab).TabRole(ETabRole::NomadTab)
        [RetakeModKitPanel::Create(FSimpleDelegate::CreateStatic(&ShowCreateMod))];
}
}

class FRetakeMapSDKEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        if (IsRunningCommandlet() || FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) != TEXT("RetakeModKit")) return;
        FGlobalTabmanager::Get()->RegisterNomadTabSpawner(WorkspaceTab, FOnSpawnTab::CreateStatic(&SpawnWorkspace))
            .SetDisplayName(LOCTEXT("TabName", "Retake Mod Kit")).SetMenuType(ETabSpawnerMenuType::Hidden);
        UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FRetakeMapSDKEditorModule::RegisterMenus));
    }
    virtual void ShutdownModule() override
    {
        if (IsRunningCommandlet() || FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) != TEXT("RetakeModKit")) return;
        UToolMenus::UnRegisterStartupCallback(this);
        UToolMenus::UnregisterOwner(this);
        if (TSharedPtr<SDockTab> Tab = FGlobalTabmanager::Get()->FindExistingLiveTab(WorkspaceTab)) Tab->RequestCloseTab();
        RetakeModKitPanel::Shutdown();
        FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(WorkspaceTab);
    }
private:
    void RegisterMenus()
    {
        FToolMenuOwnerScoped Owner(this);
        UToolMenu* Bar = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu"));
        Bar->FindOrAddSection(TEXT("Retake"))
            .AddSubMenu(TEXT("Retake"), LOCTEXT("Menu", "Retake"), LOCTEXT("MenuTip", "Create and author Retake mods."), FNewToolMenuDelegate());
        UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Retake"));
        FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("ModKit"));
        Section.AddMenuEntry(TEXT("OpenWorkspace"), LOCTEXT("OpenWorkspace", "Mod Kit Workspace"),
            LOCTEXT("OpenWorkspaceTip", "Open your mod workspaces."), FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([] { FGlobalTabmanager::Get()->TryInvokeTab(WorkspaceTab); })));
        Section.AddMenuEntry(TEXT("CreateMod"), LOCTEXT("CreateMenu", "Create New Mod..."),
            LOCTEXT("CreateMenuTip", "Choose Map, Weapons or Game modes; available workflows depend on the installed SDK."), FSlateIcon(),
            FUIAction(FExecuteAction::CreateStatic(&ShowCreateMod)));
        FToolMenuSection& Workflow = Menu->FindOrAddSection(TEXT("Workflow"), LOCTEXT("Workflow", "Map workflow"));
        struct FEntry { const TCHAR* Action; const TCHAR* Title; };
        for (const FEntry& Entry : {FEntry{TEXT("save"),TEXT("Save Mod Content")}, FEntry{TEXT("status"),TEXT("Check Map Readiness")},
            FEntry{TEXT("prepare"),TEXT("Prepare Map")}, FEntry{TEXT("export"),TEXT("Export Mod")},
            FEntry{TEXT("test"),TEXT("Test in Retake")}, FEntry{TEXT("output"),TEXT("Open Export Folder")},
            FEntry{TEXT("setup"),TEXT("Set Up Authoring Tools")}})
        {
            const FString Action(Entry.Action);
            Workflow.AddMenuEntry(FName(Entry.Action), FText::FromString(Entry.Title), FText::FromString(Entry.Title), FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([Action]
                { FGlobalTabmanager::Get()->TryInvokeTab(WorkspaceTab); RetakeModKitPanel::Action(Action); }),
                FCanExecuteAction::CreateLambda([Action]{return Action==TEXT("save")||Action==TEXT("setup")||Action==TEXT("output")||RetakeModKitPanel::IsMapWorkflow();})));
        }

        FToolMenuSection& Weapons=Menu->FindOrAddSection(TEXT("Weapons"),LOCTEXT("WeaponsSection","Weapon authoring"));
        Weapons.AddMenuEntry(TEXT("CheckWeaponDraft"),LOCTEXT("CheckWeaponDraft","Check Weapon Draft"),
            LOCTEXT("CheckWeaponDraftTip","Check native weapon authoring data; playable export remains a separate gate."),FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([]{FGlobalTabmanager::Get()->TryInvokeTab(WorkspaceTab);RetakeModKitPanel::Action(TEXT("draft-status"));}),
                FCanExecuteAction::CreateLambda([]{FString Reason;return RetakeModKitPanel::SelectedKind()==TEXT("weapons")&&RetakeModWorkspace::CanCreateKind(TEXT("weapons"),Reason);})));
    }
};

IMPLEMENT_MODULE(FRetakeMapSDKEditorModule, RetakeMapSDKEditor)
#undef LOCTEXT_NAMESPACE
