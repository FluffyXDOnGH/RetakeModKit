// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeDestructionConversionMenu.h"
#include "RetakeDestructionConversion.h"
#include "RetakeDestructionDefinition.h"
#include "RetakeDoor.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/StaticMeshActor.h"
#include "Framework/Application/SlateApplication.h"
#include "IStructureDetailsView.h"
#include "Misc/MessageDialog.h"
#include "PropertyEditorModule.h"
#include "ToolMenus.h"
#include "UObject/StructOnScope.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace RetakeDestructionConversionMenu
{
namespace
{
FDelegateHandle StartupHandle;
const FName OwnerName(TEXT("RetakeDestructionConversion"));
TWeakPtr<SWindow> Dialog;
AStaticMeshActor* SelectedSource()
{
    if (!GEditor || GEditor->GetSelectedActorCount()!=1) return nullptr;
    return Cast<AStaticMeshActor>(GEditor->GetSelectedActors()->GetTop(AActor::StaticClass()));
}
ARetakeDoor* SelectedDoor()
{
    if (!GEditor || GEditor->GetSelectedActorCount()!=1) return nullptr;
    return Cast<ARetakeDoor>(GEditor->GetSelectedActors()->GetTop(AActor::StaticClass()));
}
bool CanOpen()
{
    FString Reason;return URetakeDestructionConversion::CanConvertActor(SelectedSource(),Reason);
}
bool CanOpenDoor()
{
    FString Reason;return URetakeDestructionConversion::CanConvertDoorLeaf(SelectedDoor(),Reason);
}
using FConvert=TFunction<ARetakeDestructiblePlacement*(const FRetakeDestructionConversionOptions&,FString&)>;
void OpenDialog(const FString& Title,const FString& Intro,TSharedRef<FStructOnScope> Data,FConvert Convert);
void Open()
{
    if (auto Existing=Dialog.Pin()) {Existing->BringToFront();return;}
    AStaticMeshActor* Source=SelectedSource();FString Reason;
    if (!URetakeDestructionConversion::CanConvertActor(Source,Reason)) {FMessageDialog::Open(EAppMsgType::Ok,FText::FromString(Reason));return;}
    const TWeakObjectPtr<AStaticMeshActor> WeakSource(Source);
    OpenDialog(TEXT("Convert to Retake destructible"),TEXT("Create an editable fracture graph, definition and placement from ")+Source->GetActorLabel()+
        TEXT(". Source assets are preserved. Scale is baked into the collection; the level replacement can be undone."),
        MakeShared<FStructOnScope>(FRetakeDestructionConversionOptions::StaticStruct()),
        [WeakSource](const FRetakeDestructionConversionOptions& Options,FString& Message)
        {return URetakeDestructionConversion::ConvertActor(WeakSource.Get(),Options,Message);});
}
void OpenDoor()
{
    if (auto Existing=Dialog.Pin()) {Existing->BringToFront();return;}
    ARetakeDoor* Door=SelectedDoor();FString Reason;
    if (!URetakeDestructionConversion::CanConvertDoorLeaf(Door,Reason)) {FMessageDialog::Open(EAppMsgType::Ok,FText::FromString(Reason));return;}
    auto Data=MakeShared<FStructOnScope>(FRetakeDestructionConversionOptions::StaticStruct());
    // Wooden doors break like the wooden walls (Stalingrad's cover), with planks a bullet can chip out.
    auto& Defaults=*reinterpret_cast<FRetakeDestructionConversionOptions*>(Data->GetStructMemory());
    // The conversion anchors the leaf's hinge strip itself, so the leaf hangs from the first physics frame.
    Defaults.AssetName=TEXT("WoodDoor");Defaults.Fracture.FragmentCount=24;Defaults.bOpenFractureEditor=false;
    Defaults.Preset=LoadObject<URetakeDestructionDefinition>(nullptr,TEXT("/Game/Retake/Destruction/Definitions/DA_Destruction_Wall_Wood.DA_Destruction_Wall_Wood"));
    const TWeakObjectPtr<ARetakeDoor> WeakDoor(Door);
    OpenDialog(TEXT("Convert door leaf to Retake destructible"),TEXT("Make ")+Door->GetActorLabel()+
        TEXT("'s leaf destructible: holes can be shot through it, it swings with the door, and enough damage drops it."),
        Data,[WeakDoor](const FRetakeDestructionConversionOptions& Options,FString& Message)
        {return URetakeDestructionConversion::ConvertDoorLeaf(WeakDoor.Get(),Options,Message);});
}
void OpenDialog(const FString& Title,const FString& Intro,TSharedRef<FStructOnScope> Data,FConvert Convert)
{
    FDetailsViewArgs ViewArgs;ViewArgs.bAllowSearch=false;ViewArgs.bHideSelectionTip=true;ViewArgs.bShowScrollBar=true;
    FStructureDetailsViewArgs StructArgs;StructArgs.bShowObjects=true;
    auto Details=FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor")).CreateStructureDetailView(ViewArgs,StructArgs,Data);
    auto Window=SNew(SWindow).Title(FText::FromString(Title)).ClientSize(FVector2D(600,640)).SupportsMaximize(false).SupportsMinimize(false);
    Dialog=Window;
    const TWeakPtr<SWindow> WeakWindow=Window;
    Window->SetContent(SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(16)[SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(Intro))]
        +SVerticalBox::Slot().FillHeight(1).Padding(12)[Details->GetWidget().ToSharedRef()]
        +SVerticalBox::Slot().AutoHeight().Padding(16)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1)[SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(TEXT("Assets: Destruction in the level's content root (/Game/Retake/Destruction in Retake).\n32 fragments is a starting point; adjust in the generated graph.")))]
            +SHorizontalBox::Slot().AutoWidth().Padding(8,0)[SNew(SButton).Text(FText::FromString(TEXT("Cancel"))).OnClicked_Lambda([WeakWindow](){if (auto W=WeakWindow.Pin()) W->RequestDestroyWindow();return FReply::Handled();})]
            +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("Convert"))).OnClicked_Lambda([Data,Convert,WeakWindow]()
            {
                FString Message;
                const auto& Options=*reinterpret_cast<const FRetakeDestructionConversionOptions*>(Data->GetStructMemory());
                auto* Placement=Convert(Options,Message);
                FMessageDialog::Open(EAppMsgType::Ok,FText::FromString(Message));
                if (Placement) if (auto W=WeakWindow.Pin()) W->RequestDestroyWindow();
                return FReply::Handled();
            })]]);
    // Keep struct and details customization alive for the dialog's lifetime without a permanent UObject.
    Window->SetOnWindowClosed(FOnWindowClosed::CreateLambda([Data,Details](const TSharedRef<SWindow>&){Dialog.Reset();}));
    FSlateApplication::Get().AddWindow(Window);
}
void Register()
{
    FToolMenuOwnerScoped Owner(OwnerName);
    for (const FName MenuName:{FName(TEXT("LevelEditor.ActorContextMenu")),FName(TEXT("LevelEditor.MainMenu.Tools"))})
    {
        UToolMenu* Menu=UToolMenus::Get()->ExtendMenu(MenuName);
        auto& Section=Menu->FindOrAddSection(TEXT("RetakeDestruction"),FText::FromString(TEXT("Retake Destruction")));
        Section.AddMenuEntry(TEXT("RetakeConvertSelectedMesh"),FText::FromString(TEXT("Convert selected mesh to Retake destructible")),
            FText::FromString(TEXT("Select one plain static-mesh actor, then create a replicated Retake destructible with an editable fracture graph.")),
            FSlateIcon(),FUIAction(FExecuteAction::CreateStatic(&Open),FCanExecuteAction::CreateStatic(&CanOpen)));
        Section.AddMenuEntry(TEXT("RetakeConvertDoorLeaf"),FText::FromString(TEXT("Convert selected door's leaf to Retake destructible")),
            FText::FromString(TEXT("Select one Retake door, then make its leaf a destructible collection that swings with it (wood by default).")),
            FSlateIcon(),FUIAction(FExecuteAction::CreateStatic(&OpenDoor),FCanExecuteAction::CreateStatic(&CanOpenDoor)));
    }
}
}
void Startup()
{
    if (!IsRunningCommandlet()) StartupHandle=UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateStatic(&Register));
}
void Shutdown()
{
    if (auto Window=Dialog.Pin()) Window->RequestDestroyWindow();
    UToolMenus::UnRegisterStartupCallback(StartupHandle);
    if (UToolMenus::IsToolMenuUIEnabled()) UToolMenus::UnregisterOwner(OwnerName);
}
}
