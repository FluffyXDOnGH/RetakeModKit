// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeModKitPanel.h"
#include "RetakeModKitState.h"
#include "RetakeModWorkspace.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/STextBlock.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Rendering/DrawElements.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Misc/Paths.h"
#include "HAL/PlatformApplicationMisc.h"

namespace
{
TSharedPtr<FRetakeModKitState> State;
const FLinearColor Background(.028f,.034f,.046f), Surface(.055f,.065f,.083f), Raised(.082f,.096f,.119f);
const FLinearColor Gold(.94f,.66f,.29f), Muted(.53f,.59f,.68f), White(.91f,.93f,.97f);
const FLinearColor Green(.30f,.75f,.54f), Amber(.94f,.66f,.29f), Red(.91f,.35f,.36f);
const FSlateRoundedBoxBrush Card(FLinearColor::White, 12.f), Pill(FLinearColor::White, 6.f);
FText Text(const FString& Value) { return FText::FromString(Value); }
FSlateFontInfo Font(int32 Size, bool Bold=false) { return FCoreStyle::GetDefaultFontStyle(Bold ? TEXT("Bold") : TEXT("Regular"), Size); }
FLinearColor CheckColor(int32 Value) { return Value == 1 ? Green : Value == 2 ? Amber : Value == 3 ? Red : Muted; }

/** A quiet map motif drawn as native vectors: no imported art or game content dependency. */
class SMapMotif : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SMapMotif) {} SLATE_END_ARGS()
    void Construct(const FArguments&) {}
    virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(280,100); }
    virtual int32 OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&, FSlateWindowElementList& Out,
        int32 Layer, const FWidgetStyle&, bool) const override
    {
        const FVector2f Size = Geometry.GetLocalSize();
        for (int32 I=0;I<6;++I)
        {
            TArray<FVector2f> Points;
            const float Y=14.f+I*14.f;
            Points = {FVector2f(Size.X*.10f,Y),FVector2f(Size.X*.38f,Y+7),FVector2f(Size.X*.58f,Y-7),FVector2f(Size.X*.88f,Y+4)};
            FSlateDrawElement::MakeLines(Out,Layer,Geometry.ToPaintGeometry(),Points,ESlateDrawEffect::None,FLinearColor(.45f,.55f,.69f,.10f),true,1.f);
        }
        TArray<FVector2f> Route = {FVector2f(Size.X*.25f,Size.Y*.78f),FVector2f(Size.X*.45f,Size.Y*.78f),
            FVector2f(Size.X*.45f,Size.Y*.35f),FVector2f(Size.X*.72f,Size.Y*.35f)};
        FSlateDrawElement::MakeLines(Out,Layer+1,Geometry.ToPaintGeometry(),Route,ESlateDrawEffect::None,FLinearColor(Gold.R,Gold.G,Gold.B,.24f),true,2.f);
        return Layer+1;
    }
};

class SRetakeModKitPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SRetakeModKitPanel) {} SLATE_EVENT(FSimpleDelegate, OnCreateMod) SLATE_END_ARGS()
    void Construct(const FArguments& Args)
    {
        if (!State) State = MakeShared<FRetakeModKitState>();
        Model = State; CreateMod = Args._OnCreateMod;
        Primary = FAppStyle::Get().GetWidgetStyle<FButtonStyle>(TEXT("Button"));
        Primary.SetNormal(FSlateRoundedBoxBrush(Gold,7.f)).SetHovered(FSlateRoundedBoxBrush(FLinearColor(1.f,.76f,.40f),7.f))
            .SetPressed(FSlateRoundedBoxBrush(FLinearColor(.76f,.48f,.18f),7.f)).SetDisabled(FSlateRoundedBoxBrush(Raised,7.f));
        Secondary = Primary; Secondary.SetNormal(FSlateRoundedBoxBrush(Raised,7.f)).SetHovered(FSlateRoundedBoxBrush(FLinearColor(.13f,.16f,.21f),7.f))
            .SetPressed(FSlateRoundedBoxBrush(Surface,7.f));
        ActionStyle = Primary;
        ActionStyle.SetNormal(FSlateRoundedBoxBrush(FLinearColor::White,7.f))
            .SetHovered(FSlateRoundedBoxBrush(FLinearColor(.86f,.86f,.86f),7.f))
            .SetPressed(FSlateRoundedBoxBrush(FLinearColor(.70f,.70f,.70f),7.f))
            .SetDisabled(FSlateRoundedBoxBrush(FLinearColor::White,7.f));
        WorkflowSteps = {
            StepCard(1, TEXT("Create / select"), TEXT("Choose the mod and the map you want to work on."),
                [this]{return Model->ModId.IsEmpty()?TEXT("Start here"):TEXT("Workspace selected");}, Selectors()),
            StepCard(2, TEXT("Build your map"), TEXT("Place geometry and SDK elements, choose game modes, then save."),
                [this]{return Model->HasUnsaved()?TEXT("Unsaved changes"):TEXT("Author in the level editor");}, BuildCard()),
            StepCard(3, TEXT("Check your map"), TEXT("Check saved content. Review errors and make deliberate choices for warnings."),
                [this]{return Model->Stale?TEXT("Check needed"):Model->Errors?TEXT("Review issues"):Model->Warnings?TEXT("Passed with warnings"):TEXT("Passed");}, CheckCard()),
            StepCard(4, TEXT("Prepare map data"), TEXT("Only missing or outdated preparation runs. Optional features stay optional."),
                [this]{return Model->Stale?TEXT("Check first"):Model->PreparationCurrent?TEXT("Up to date"):Model->PreparationBlocked?TEXT("Setup needed"):TEXT("Preparation needed");}, PrepareCard()),
            StepCard(5, TEXT("Export your mod"), TEXT("Build an immutable Windows / Linux bundle and a separate editable-source recovery archive."),
                [this]{return Model->ExportIsCurrent()?TEXT("Exported"):Model->CanExport()?TEXT("Ready to export"):TEXT("Complete earlier steps");}, ExportCard()),
            StepCard(6, TEXT("Test in Retake"), TEXT("Use your installed game and optional local server to try the exported revision."),
                [this]{return !Model->HasExport()?TEXT("Choose an export"):!Model->TestCheckCurrent()?TEXT("Check installations"):Model->TestReady?TEXT("Ready to launch"):TEXT("Installation needs attention");}, TestCard())
        };
        TSharedRef<SVerticalBox> Steps = SNew(SVerticalBox);
        for (const auto& Step : WorkflowSteps) Steps->AddSlot().AutoHeight().Padding(0,0,4,12)[Step.ToSharedRef()];
        Steps->AddSlot().AutoHeight().Padding(0,0,4,8)[ActivityCard()];
        ChildSlot
        [SNew(SBorder).BorderImage(FAppStyle::GetBrush(TEXT("WhiteBrush"))).BorderBackgroundColor(Background).Padding(18)
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[Header()]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[KindTabs()]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[Status()]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[AuthoringToolsCard()]
            +SVerticalBox::Slot().FillHeight(1)
            [SNew(SWidgetSwitcher).WidgetIndex_Lambda([this]{return Model->ActiveKind==TEXT("map")?0:Model->ActiveKind==TEXT("weapons")?1:2;})
                +SWidgetSwitcher::Slot()[SAssignNew(WorkflowScroll,SScrollBox)+SScrollBox::Slot()[Steps]]
                +SWidgetSwitcher::Slot()[SNew(SScrollBox)+SScrollBox::Slot()[WeaponsWorkspace()]]
                +SWidgetSwitcher::Slot()[GameModesWorkspace()]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,10,0,0)
            [SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1)[SNew(STextBlock).Text_Lambda([this]
                    {return Text(!Model->IsMapWorkspace()?TEXT("AUTHORING CAPABILITIES"):Model->Candidate?TEXT("LOCAL TEST KIT"):TEXT("RETAKE MAP WORKSHOP"));}).Font(Font(9,true)).ColorAndOpacity(Muted)]
                +SHorizontalBox::Slot().AutoWidth()[Button(TEXT("Tool setup"),[this]{Model->SetupTools();})]]]];
        LastSerial = Model->ChangeSerial;
        RebuildPalette(); RebuildIssues(); RebuildPreparation();
    }
    virtual void Tick(const FGeometry& Geometry,double Time,float Delta) override
    {
        SCompoundWidget::Tick(Geometry,Time,Delta);
        if (LastSerial != Model->ChangeSerial)
        {
            LastSerial = Model->ChangeSerial;
            for(const auto& Picker:ModPickers)if(Picker)Picker->RefreshOptions();
            if (MapCombo) MapCombo->RefreshOptions();
            if (TestModeCombo) TestModeCombo->RefreshOptions();
            if (ExportCombo) ExportCombo->RefreshOptions();
            RebuildIssues(); RebuildPreparation();
        }
    }
private:
    TSharedPtr<FRetakeModKitState> Model;
    FSimpleDelegate CreateMod;
    FButtonStyle Primary,Secondary,ActionStyle;
    enum class EActionTone { Unavailable, Needed, Complete, Running, Blocked };
    TSharedPtr<SComboBox<TSharedPtr<FString>>> MapCombo,TestModeCombo,ExportCombo;
    TArray<TSharedPtr<SComboBox<TSharedPtr<FString>>>> ModPickers;
    TSharedPtr<SWrapBox> Palette;
    TArray<TSharedPtr<SVerticalBox>> IssueLists;
    TSharedPtr<SVerticalBox> PreparationList;
    TSharedPtr<SScrollBox> WorkflowScroll;
    TArray<TSharedPtr<SWidget>> WorkflowSteps;
    FString Search, CopyFeedback;
    int32 LastSerial=0;

    TSharedRef<SWidget> Button(const FString& Caption,TFunction<void()> Click,bool GoldButton=false,TFunction<bool()> Enabled={})
    {
        return SNew(SButton).ButtonStyle(GoldButton ? &Primary : &Secondary).ContentPadding(FMargin(12,9))
            .IsEnabled_Lambda([this,Enabled]{return Enabled ? Enabled() : Model->IsKit()&&!Model->Busy();})
            .OnClicked_Lambda([Click]{Click();return FReply::Handled();})
            [SNew(STextBlock).Text(Text(Caption)).Font(Font(10,true)).ColorAndOpacity(GoldButton ? FLinearColor(.075f,.06f,.035f) : White)];
    }
    EActionTone ActionTone(const FString& Action) const
    {
        if(Model->IsRunning(*Action)) return EActionTone::Running;
        if(Model->Busy() || !Model->IsKit() || Model->ModId.IsEmpty()) return EActionTone::Unavailable;
        if(Model->FailedAction(*Action)) return EActionTone::Blocked;
        if(Action==TEXT("save")) return Model->HasUnsaved()?EActionTone::Needed:EActionTone::Complete;
        if(Action==TEXT("draft-status"))return !Model->CanAuthorCurrentKind()||!Model->HasTools()||Model->HasUnsaved()?EActionTone::Unavailable:
            Model->Stale?EActionTone::Needed:Model->Errors?EActionTone::Blocked:EActionTone::Complete;
        if(!Model->HasTools()) return EActionTone::Unavailable;
        if(Action==TEXT("weapon-basis"))return Model->HasUnsaved()?EActionTone::Unavailable:EActionTone::Needed;
        if(Action==TEXT("prepare-weapons"))return !Model->CanPrepare()?EActionTone::Unavailable:Model->PreparationCurrent?EActionTone::Complete:EActionTone::Needed;
        if(Action==TEXT("status"))
            return Model->HasUnsaved()?EActionTone::Unavailable:Model->Stale?EActionTone::Needed:Model->Errors?EActionTone::Blocked:EActionTone::Complete;
        if(Action==TEXT("prepare"))
            return Model->Stale||Model->HasUnsaved()?EActionTone::Unavailable:Model->PreparationBlocked?EActionTone::Blocked:
                Model->PreparationCurrent?EActionTone::Complete:Model->PreparationNeeded?EActionTone::Needed:EActionTone::Unavailable;
        if(Action==TEXT("export"))
            return Model->ExportIsCurrent()?EActionTone::Complete:Model->CanExport()?EActionTone::Needed:
                !Model->Stale&&Model->Errors?EActionTone::Blocked:EActionTone::Unavailable;
        if(Action==TEXT("minimap")) return Model->MinimapIsCurrent()?EActionTone::Complete:!Model->CanBakeMinimap()?EActionTone::Unavailable:EActionTone::Needed;
        if(Action==TEXT("test-info"))return !Model->HasExport()?EActionTone::Unavailable:!Model->TestCheckCurrent()?EActionTone::Needed:Model->TestReady?EActionTone::Complete:EActionTone::Blocked;
        if(Action==TEXT("start-test-server"))return Model->ServerStatus==TEXT("running")?EActionTone::Complete:Model->CanStartServer()?EActionTone::Needed:
            Model->TestCheckCurrent()&&!Model->ServerAllowed?EActionTone::Blocked:EActionTone::Unavailable;
        if(Action==TEXT("test")) return Model->CanTest()?(Model->TestLaunched?EActionTone::Complete:EActionTone::Needed):
            Model->HasExport()&&Model->TestCheckCurrent()?EActionTone::Blocked:EActionTone::Unavailable;
        return EActionTone::Unavailable;
    }
    FString ActionCaption(const FString& Action,EActionTone Tone) const
    {
        if(Action==TEXT("weapon-basis"))return Tone==EActionTone::Running?TEXT("Creating references..."):TEXT("Create authoring references");
        if(Action==TEXT("prepare-weapons"))return Tone==EActionTone::Running?TEXT("Preparing weapons..."):Tone==EActionTone::Complete?TEXT("Weapons prepared - prepare again"):TEXT("Prepare weapon profiles");
        if(Tone==EActionTone::Running) return (Action==TEXT("status")||Action==TEXT("draft-status"))?TEXT("Checking..."):Action==TEXT("prepare")?TEXT("Preparing..."):
            Action==TEXT("export")?TEXT("Exporting..."):Action==TEXT("test-info")?TEXT("Checking installations..."):Action==TEXT("minimap")?TEXT("Creating minimap..."):TEXT("Launching...");
        if(Action==TEXT("draft-status"))return Tone==EActionTone::Complete?TEXT("Draft checked - check again"):TEXT("Check weapon draft");
        if(Action==TEXT("minimap"))return Tone==EActionTone::Complete?(Model->UseCustomMinimap?TEXT("Custom image aligned - apply again"):TEXT("Minimap generated - regenerate")):Model->UseCustomMinimap?TEXT("Apply custom image & alignment"):TEXT("Generate minimap");
        if(Action==TEXT("test-info"))return Tone==EActionTone::Complete?TEXT("Installations checked - check again"):TEXT("Check selected installations");
        if(Action==TEXT("start-test-server"))return Tone==EActionTone::Complete?TEXT("Server running - restart"):TEXT("Start server only");
        if(Action==TEXT("save")) return Tone==EActionTone::Complete?TEXT("Saved"):TEXT("Save mod content");
        if(Action==TEXT("status")) return Tone==EActionTone::Complete?TEXT("Checked - check again"):
            Tone==EActionTone::Blocked?TEXT("Review issues, then check again"):Model->HasUnsaved()?TEXT("Save before checking"):TEXT("Check map");
        if(Action==TEXT("prepare")) return Tone==EActionTone::Complete?TEXT("Preparation up to date"):
            Tone==EActionTone::Blocked?TEXT("Fix setup, then prepare"):Tone==EActionTone::Unavailable?TEXT("Check map before preparing"):TEXT("Prepare map data");
        if(Action==TEXT("export")) return Tone==EActionTone::Complete?TEXT("Exported - export again"):
            Tone==EActionTone::Blocked?TEXT("Resolve issues before export"):Tone==EActionTone::Unavailable?TEXT("Complete Check / Prepare first"):TEXT("Export mod");
        return Tone==EActionTone::Complete?TEXT("Launched - launch again"):Tone==EActionTone::Unavailable?(!Model->HasExport()?TEXT("Choose an export first"):TEXT("Check installations first")):
            Tone==EActionTone::Blocked?TEXT("Resolve installation issues"):Model->UseServer?TEXT("Start server & client"):TEXT("Launch client");
    }
    TSharedRef<SWidget> ActionButton(const FString& Action)
    {
        return SNew(SBox).HAlign(HAlign_Left)
        [SNew(SButton).ButtonStyle(&ActionStyle).ContentPadding(FMargin(14,10))
            .ButtonColorAndOpacity_Lambda([this,Action]
            {
                const EActionTone Tone=ActionTone(Action);
                return FSlateColor(Tone==EActionTone::Needed?Gold:Tone==EActionTone::Complete?FLinearColor(.065f,.27f,.17f):
                    Tone==EActionTone::Running?FLinearColor(.055f,.22f,.50f):Tone==EActionTone::Blocked?FLinearColor(.38f,.095f,.10f):Raised);
            })
            .IsEnabled_Lambda([this,Action]
            {
                if(!Model->IsKit()||Model->Busy()||Model->ModId.IsEmpty())return false;
                if(Action==TEXT("save"))return Model->HasUnsaved();
                if(Action==TEXT("status"))return Model->HasTools()&&!Model->HasUnsaved();
                if(Action==TEXT("draft-status"))return Model->ActiveKind==TEXT("weapons")&&Model->CanAuthorCurrentKind()&&Model->HasTools()&&!Model->HasUnsaved();
                if(Action==TEXT("prepare"))return Model->CanPrepare();
                if(Action==TEXT("prepare-weapons"))return Model->ActiveKind==TEXT("weapons")&&Model->CanPrepare();
                if(Action==TEXT("weapon-basis"))return Model->ActiveKind==TEXT("weapons")&&Model->HasTools()&&!Model->HasUnsaved()&&Model->CanAuthorCurrentKind();
                if(Action==TEXT("export"))return Model->CanExport();
                if(Action==TEXT("minimap"))return Model->CanBakeMinimap();
                if(Action==TEXT("test-info"))return Model->HasExport()&&Model->HasTools();
                if(Action==TEXT("start-test-server"))return Model->CanStartServer();
                return Model->CanTest();
            })
            .OnClicked_Lambda([this,Action]{if(Action==TEXT("save"))Model->Save();else Model->Run(Action);return FReply::Handled();})
            .ToolTipText_Lambda([this,Action]
            {
                const EActionTone Tone=ActionTone(Action);
                return Text(Tone==EActionTone::Complete?TEXT("This step is complete. Recheck or re-export only when you need to."):
                    Tone==EActionTone::Running?TEXT("This operation is running. Progress is shown above."):
                    Tone==EActionTone::Blocked?TEXT("Review this step's issues and instructions before retrying."):
                    Tone==EActionTone::Unavailable?TEXT("Complete the prerequisite steps shown in this card."):TEXT("This action is needed for the current saved content."));
            })
            [SNew(STextBlock).Text_Lambda([this,Action]{return Text(ActionCaption(Action,ActionTone(Action)));}).Font(Font(10,true))
                .ColorAndOpacity_Lambda([this,Action]{return ActionTone(Action)==EActionTone::Needed?FLinearColor(.075f,.06f,.035f):White;})]];
    }
    TSharedRef<SWidget> Header()
    {
        return SNew(SOverlay)
        +SOverlay::Slot().HAlign(HAlign_Right)[SNew(SMapMotif).Visibility(EVisibility::HitTestInvisible)]
        +SOverlay::Slot()[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1)
            [SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(Text(TEXT("RETAKE"))).Font(Font(25,true)).ColorAndOpacity(White)]
                +SVerticalBox::Slot().AutoHeight().Padding(0,3,0,8)[SNew(STextBlock).Text(Text(TEXT("M O D   K I T"))).Font(Font(10,true)).ColorAndOpacity(Gold)]
                +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(Text(TEXT("Create content for Retake. Choose a mod type to begin."))).Font(Font(10)).ColorAndOpacity(Muted)]]
            +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[Button(TEXT("+  New mod"),[this]{CreateMod.ExecuteIfBound();},true)]];
    }
    TSharedRef<SWidget> KindTabs()
    {
        TSharedRef<SHorizontalBox> Tabs=SNew(SHorizontalBox);
        for(const auto& Kind:RetakeModWorkspace::Kinds())
        {
            const FString Id(Kind.Id);
            Tabs->AddSlot().FillWidth(1).Padding(0,0,6,0)
            [SNew(SButton).ButtonStyle(&ActionStyle).ContentPadding(FMargin(12,10))
                .ButtonColorAndOpacity_Lambda([this,Id]{return FSlateColor(Model->ActiveKind==Id?Gold:Raised);})
                .IsEnabled_Lambda([this]{return !Model->Busy();})
                .OnClicked_Lambda([this,Id]{Model->SelectKind(Id);return FReply::Handled();})
                [SNew(STextBlock).Text(Text(Kind.Name)).Font(Font(11,true)).Justification(ETextJustify::Center)
                    .ColorAndOpacity_Lambda([this,Id]{return Model->ActiveKind==Id?FLinearColor(.075f,.06f,.035f):White;})]];
        }
        return Tabs;
    }
    TSharedRef<SWidget> Selectors(bool MapView=true)
    {
        TSharedPtr<SComboBox<TSharedPtr<FString>>> Picker;
        TSharedRef<SVerticalBox> Rows=SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[SNew(STextBlock).Text(Text(TEXT("YOUR WORKSPACE"))).Font(Font(9,true)).ColorAndOpacity(Muted)]
            +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,8,0)
                [SAssignNew(Picker,SComboBox<TSharedPtr<FString>>).OptionsSource(&Model->Mods).IsEnabled_Lambda([this]{return !Model->Busy();})
                    .OnGenerateWidget_Lambda([](TSharedPtr<FString> Item){return SNew(STextBlock).Text(Text(Item->RightChop(10))).Font(Font(11));})
                    .OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item,ESelectInfo::Type){if(Item)Model->Select(*Item);})
                    [SNew(STextBlock).Text_Lambda([this]{return Text(Model->ModId.IsEmpty()?TEXT("Choose or create a mod"):Model->ModId.RightChop(10));}).Font(Font(11,true)).Margin(FMargin(6))]]
                +SHorizontalBox::Slot().AutoWidth()[Button(TEXT("Refresh"),[this]{Model->Refresh();})]];
        ModPickers.Add(Picker);
        if(MapView)
        {
            Rows->AddSlot().AutoHeight().Padding(0,8,0,0)[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,8,0)
                [SAssignNew(MapCombo,SComboBox<TSharedPtr<FString>>).OptionsSource(&Model->Maps).IsEnabled_Lambda([this]{return !Model->Busy();})
                    .OnGenerateWidget_Lambda([](TSharedPtr<FString> Item){return SNew(STextBlock).Text(Text(*Item)).Font(Font(10));})
                    .OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item,ESelectInfo::Type){if(Item)Model->SelectMap(*Item);})
                    [SNew(STextBlock).Text_Lambda([this]{return Text(Model->MapName.IsEmpty()?TEXT("No saved map"):Model->MapName);}).Margin(FMargin(6))]]
                +SHorizontalBox::Slot().AutoWidth()[Button(TEXT("Open map"),[this]{Model->OpenMap();})]];
            Rows->AddSlot().AutoHeight().Padding(0,8,0,0)[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1)[SNew(STextBlock).Text(Text(TEXT("Start empty, use Basic, or explore the Example mod."))).AutoWrapText(true).Font(Font(9)).ColorAndOpacity(Muted)]
                +SHorizontalBox::Slot().AutoWidth()[Button(TEXT("Example"),[this]{Model->OpenExample();})]];
        }
        else Rows->AddSlot().AutoHeight().Padding(0,10,0,0)[Button(TEXT("Open mod content"),[this]{Model->OpenContent();},false,[this]{return !Model->Busy()&&!Model->ModId.IsEmpty()&&Model->CanAuthorCurrentKind();})];
        return SNew(SBorder).BorderImage(&Card).BorderBackgroundColor(Surface).Padding(12)[Rows];
    }
    TSharedRef<SWidget> WeaponsWorkspace()
    {
        TSharedRef<SWrapBox> Assets=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(8,8));
        Assets->AddSlot()[Button(TEXT("Open weapon catalogue"),[this]{Model->OpenWeaponCatalogue();},false,[this]{return !Model->Busy()&&!Model->ModId.IsEmpty()&&Model->CanAuthorCurrentKind();})];
        for(const auto& Entry:TArray<TPair<FString,FString>>{{TEXT("specification"),TEXT("Add weapon specification")},{TEXT("ammo"),TEXT("Add ammunition data")},
            {TEXT("animation"),TEXT("Add animation profile")},{TEXT("tracer"),TEXT("Add tracer profile")}})
        {const FString Role=Entry.Key;Assets->AddSlot()[Button(Entry.Value,[this,Role]{Model->CreateWeaponAsset(Role);},false,[this]{return !Model->Busy()&&!Model->ModId.IsEmpty()&&Model->CanAuthorCurrentKind();})];}
        return SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,4,12)[StepCard(1,TEXT("Create / select weapon pack"),TEXT("A weapon pack owns data and resources in its own Content folder. It does not need a level."),
                [this]{return Model->CanAuthorCurrentKind()?TEXT("Workspace selected"):TEXT("SDK update required");},SNew(SVerticalBox)
                    +SVerticalBox::Slot().AutoHeight()[Selectors(false)]
                    +SVerticalBox::Slot().AutoHeight().Padding(0,8)[Button(TEXT("Open / create weapon example"),[this]{Model->OpenExample();},false,[this]{return !Model->Busy()&&Model->HasTools();})])]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,4,12)[StepCard(2,TEXT("Author weapons and profiles"),TEXT("Open DA_WeaponPack in Data. Add weapon identities and profiles, then bind their public specifications. Author presentation resources inside this mod."),
                []{return FString(TEXT("Data authoring"));},SNew(SVerticalBox)
                    +SVerticalBox::Slot().AutoHeight()[Assets]
                    +SVerticalBox::Slot().AutoHeight().Padding(0,10)[ActionButton(TEXT("weapon-basis"))]
                    +SVerticalBox::Slot().AutoHeight().Padding(0,12,0,0)[ActionButton(TEXT("save"))])]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,4,12)[StepCard(3,TEXT("Check authoring data"),TEXT("Native asset and ownership checks help author the draft. Export, preparation and runtime acceptance are separate gates."),
                [this]{return Model->Stale?TEXT("Check needed"):Model->Errors?TEXT("Needs attention"):TEXT("Draft check complete");},SNew(SVerticalBox)
                    +SVerticalBox::Slot().AutoHeight()[ActionButton(TEXT("draft-status"))]
                    +SVerticalBox::Slot().AutoHeight().Padding(0,10,0,0)[IssuesCard()])]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,4,12)[StepCard(4,TEXT("Prepare weapon profiles"),TEXT("Build the catalogue's pose contributions from the saved weapon specifications and authoring references. Current contributions are reused."),
                [this]{return Model->PreparationCurrent?TEXT("Prepared"):TEXT("Check saved content first");},ActionButton(TEXT("prepare-weapons")))]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,4,12)[StepCard(5,TEXT("Export your weapon pack"),TEXT("Export Windows and Linux content plus editable-source recovery. Compatibility and content validation still apply."),
                [this]{return Model->ExportIsCurrent()?TEXT("Exported"):Model->CanExport()?TEXT("Ready to export"):TEXT("Complete earlier steps and review compatibility");},ExportCard())]
            +SVerticalBox::Slot().AutoHeight()[ActivityCard()];
    }
    TSharedRef<SWidget> GameModesWorkspace()
    {
        return StepCard(1,TEXT("Game-mode mods"),TEXT("New creator-authored match rules need a supported execution, replication and server-authority contract."),
            []{return FString(TEXT("Planned"));},SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(Text(TEXT("This mod type cannot be created or exported yet. Maps can already declare support for Retake's existing modes such as FFA, TDM and OBJ."))).Font(Font(11)).ColorAndOpacity(Muted).AutoWrapText(true)]
                +SVerticalBox::Slot().AutoHeight().Padding(0,14,0,0)[Button(TEXT("Go to map authoring"),[this]{Model->SelectKind(TEXT("map"));})]);
    }
    TSharedRef<SWidget> AuthoringToolsCard()
    {
        return SNew(SBorder).BorderImage(&Card).BorderBackgroundColor(Surface).Padding(14)
            .Visibility_Lambda([this]{return Model->HasTools()?EVisibility::Collapsed:EVisibility::Visible;})
            [SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(Text(TEXT("Enable checking, preparation and export using this editor's bundled Python. The first setup downloads the authoring dependencies; progress stays in this window."))).AutoWrapText(true).ColorAndOpacity(White)]
                +SVerticalBox::Slot().AutoHeight().Padding(0,10,0,0)[Button(TEXT("Enable authoring tools"),[this]{Model->SetupTools();},true)]];
    }
    TSharedRef<SWidget> StepCard(int32 Number,const FString& Title,const FString& Description,TFunction<FString()> StatusText,TSharedRef<SWidget> Body)
    {
        return SNew(SBorder).BorderImage(&Card).BorderBackgroundColor(Surface).Padding(16)
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()
            [SNew(SHorizontalBox)
                +SHorizontalBox::Slot().AutoWidth().Padding(0,0,12,0)
                [SNew(SBorder).BorderImage(&Pill).BorderBackgroundColor(Raised).Padding(FMargin(11,6))
                    [SNew(STextBlock).Text(Text(FString::FromInt(Number))).Font(Font(14,true)).ColorAndOpacity(Gold)]]
                +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[SNew(STextBlock).Text(Text(Title)).Font(Font(14,true)).ColorAndOpacity(White)]
                +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(STextBlock).Text_Lambda([StatusText]{return Text(StatusText());}).Font(Font(9,true)).ColorAndOpacity(Gold)]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,8,0,10)[SNew(STextBlock).Text(Text(Description)).Font(Font(10)).ColorAndOpacity(Muted).AutoWrapText(true)]
            +SVerticalBox::Slot().AutoHeight()[Body]];
    }
    void GoToNextStep()
    {
        if(!Model->IsMapWorkspace())return;
        const int32 Index = Model->IsTestingWorkflow()?5:Model->ModId.IsEmpty()?0:Model->Stale?2:Model->PreparationBlocked?3:
            Model->PreparationNeeded?3:Model->Errors?2:Model->ExportIsCurrent()?5:4;
        if(WorkflowScroll && WorkflowSteps.IsValidIndex(Index)) WorkflowScroll->ScrollDescendantIntoView(WorkflowSteps[Index],true,EDescendantScrollDestination::TopOrLeft);
    }
    TSharedRef<SWidget> BuildCard()
    {
        return SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[ActionButton(TEXT("save"))]
            +SVerticalBox::Slot().AutoHeight().Padding(0,12,0,0)
            [SNew(SExpandableArea).InitiallyCollapsed(true)
                .HeaderContent()[SNew(STextBlock).Text(Text(TEXT("Elements and game modes"))).Font(Font(10,true)).ColorAndOpacity(White)]
                .BodyContent()[SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()[ModesCard()]+SVerticalBox::Slot().AutoHeight()[ElementsCard()]]];
    }
    TSharedRef<SWidget> CheckCard()
    {
        return SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[ActionButton(TEXT("status"))]
            +SVerticalBox::Slot().AutoHeight().Padding(0,8)[SNew(STextBlock).Text_Lambda([this]
                {return Text(Model->DetectedModes().IsEmpty()?TEXT("Supported modes appear after checking the map."):TEXT("Detected modes: ")+Model->DetectedModes());})
                .Font(Font(10)).ColorAndOpacity(Muted)]
            +SVerticalBox::Slot().AutoHeight()[Checklist()]
            +SVerticalBox::Slot().AutoHeight()[IssuesCard()];
    }
    TSharedRef<SWidget> PrepareCard()
    {
        return SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[SAssignNew(PreparationList,SVerticalBox)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,8)[SNew(STextBlock).Text_Lambda([this]
                {return Text(Model->Stale?TEXT("Check the saved map first to identify the work needed."):
                    Model->PreparationCurrent?TEXT("Everything is current. Skip this step and continue to Export."):
                    Model->PreparationBlocked?TEXT("Fix the setup listed above, then Check map again."):TEXT("Prepare builds only the missing or outdated map data."));})
                .Font(Font(10)).ColorAndOpacity(Muted).AutoWrapText(true)]
            +SVerticalBox::Slot().AutoHeight()[ActionButton(TEXT("prepare"))]
            +SVerticalBox::Slot().AutoHeight().Padding(0,9,0,0)[SNew(STextBlock).Text(Text(TEXT("Surface choices, missing elements and content errors need manual authoring. Preparation does not fix those warnings.")))
                .Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,16,0,0)[MinimapGuide()];
    }
    void RebuildPreparation()
    {
        if(!PreparationList)return;PreparationList->ClearChildren();
        if(Model->PreparationChecks.IsEmpty())
            PreparationList->AddSlot().AutoHeight()[SNew(STextBlock).Text(Text(TEXT("Acoustics, optional minimap, geometry and visibility have not been checked."))).Font(Font(10)).ColorAndOpacity(Muted).AutoWrapText(true)];
        for(const auto& Check:Model->PreparationChecks)
        {
            const int32 Value=Model->Stale?0:Check.State;
            const FString StateText=Value==1?TEXT("Up to date"):Value==2?TEXT("Needs preparation"):Value==3?TEXT("Setup needed"):Value==4?TEXT("Not used"):TEXT("Check needed");
            PreparationList->AddSlot().AutoHeight().Padding(0,0,0,9)
            [SNew(SHorizontalBox)
                +SHorizontalBox::Slot().AutoWidth().Padding(0,2,10,0)[SNew(SImage).Image(FAppStyle::GetBrush(Value==1?TEXT("Icons.Check"):Value==3?TEXT("Icons.Error"):Value==2?TEXT("Icons.Warning"):TEXT("Icons.Info"))).ColorAndOpacity(CheckColor(Value))]
                +SHorizontalBox::Slot().FillWidth(1)[SNew(SVerticalBox)
                    +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(Text(Check.Title)).Font(Font(10,true)).ColorAndOpacity(White)]
                    +SVerticalBox::Slot().AutoHeight().Padding(0,3,0,0)[SNew(STextBlock).Text(Text(Model->Stale?TEXT("Check again after saved changes."):Check.Detail)).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)]]
                +SHorizontalBox::Slot().AutoWidth().Padding(10,0,0,0)[SNew(STextBlock).Text(Text(StateText)).Font(Font(9)).ColorAndOpacity(CheckColor(Value))]];
        }
    }
    TSharedRef<SWidget> Status()
    {
        return SNew(SBorder).BorderImage(&Card).BorderBackgroundColor(Surface).Padding(16)
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()
            [SNew(SHorizontalBox)
                +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0,0,12,0)
                [SNew(SImage).Image_Lambda([this]{return FAppStyle::GetBrush(Model->Ready&&!Model->Stale?TEXT("Icons.Success.Large"):(Model->OperationFailed||(Model->Errors&&!Model->Stale))?TEXT("Icons.Error.Large"):TEXT("Icons.Info.Large"));})
                    .ColorAndOpacity_Lambda([this]{return Model->Ready&&!Model->Stale?Green:(Model->OperationFailed||(Model->Errors&&!Model->Stale))?Red:Gold;})]
                +SHorizontalBox::Slot().FillWidth(1)
                [SNew(SVerticalBox)
                    +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text_Lambda([this]{return Text(Model->StatusTitle());}).Font(Font(16,true)).ColorAndOpacity(White).AutoWrapText(true)]
                    +SVerticalBox::Slot().AutoHeight().Padding(0,5,0,0)[SNew(STextBlock).Text_Lambda([this]{return Text(Model->StatusDetail());}).Font(Font(10)).ColorAndOpacity(Muted).AutoWrapText(true)]]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,14,0,0)
            [SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(8,8))
                +SWrapBox::Slot()[SNew(SBox).Visibility_Lambda([this]{return Model->IsMapWorkspace()?EVisibility::Visible:EVisibility::Collapsed;})[Button(TEXT("Go to next step"),[this]{GoToNextStep();},true)]]
                +SWrapBox::Slot()[Button(TEXT("Stop"),[this]{Model->Cancel();},false,[this]{return Model->Busy();})]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,12,0,0)
            [SNew(SBox).HeightOverride(4).Visibility_Lambda([this]{return Model->Busy()?EVisibility::Visible:EVisibility::Collapsed;})
                [SNew(SProgressBar).Percent(TOptional<float>()).FillColorAndOpacity(Gold)]]];
    }
    TSharedRef<SWidget> MinimapGuide()
    {
        TSharedRef<SWrapBox> Fields=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(14,8));
        for(const auto& Field:TArray<TPair<FString,FString>>{{TEXT("size"),TEXT("Coverage width (m)")},{TEXT("height"),TEXT("Height cutoff (m)")},{TEXT("datum"),TEXT("Datum height (m)")},{TEXT("yaw"),TEXT("North rotation (degrees)")}})
        {
            const FString Id=Field.Key;
            Fields->AddSlot()[SNew(SBox).WidthOverride(175)[SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,4)[SNew(STextBlock).Text(Text(Field.Value)).Font(Font(9)).ColorAndOpacity(Muted)]
                +SVerticalBox::Slot().AutoHeight()[SNew(SNumericEntryBox<double>).AllowSpin(false).MinDesiredValueWidth(120)
                    .Value_Lambda([this,Id]{return Model->MinimapValue(Id);})
                    .IsEnabled_Lambda([this]{return Model->CanAuthorMinimap()&&Model->HasMinimapCapture();})
                    .OnValueCommitted_Lambda([this,Id](double Value,ETextCommit::Type){Model->SetMinimapValue(Id,Value);})]]];
        }
        return SNew(SBorder).BorderImage(&Card).BorderBackgroundColor(Raised).Padding(14)
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(Text(TEXT("MINIMAP  |  Generate or use your own image"))).Font(Font(11,true)).ColorAndOpacity(White)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,6,0,10)[SNew(STextBlock).Text_Lambda([this]{return Text(Model->MinimapMessage);}).Font(Font(10)).ColorAndOpacity(Muted).AutoWrapText(true)]
            +SVerticalBox::Slot().AutoHeight()[SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(7,7))
                +SWrapBox::Slot()[Button(TEXT("Open selected map"),[this]{Model->OpenMap();})]
                +SWrapBox::Slot()[Button(TEXT("Set up capture"),[this]{Model->SetupMinimap();},false,[this]{return Model->CanAuthorMinimap();})]
                +SWrapBox::Slot()[Button(TEXT("Select capture / Details"),[this]{Model->SelectMinimapCapture();},false,[this]{return Model->CanAuthorMinimap()&&Model->HasMinimapCapture();})]
                +SWrapBox::Slot()[Button(TEXT("Fit to level"),[this]{Model->FitMinimap(false);},false,[this]{return Model->CanAuthorMinimap()&&Model->HasMinimapCapture();})]
                +SWrapBox::Slot()[Button(TEXT("Fit to selected geometry"),[this]{Model->FitMinimap(true);},false,[this]{return Model->CanAuthorMinimap()&&Model->HasMinimapCapture();})]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,10)[Fields]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[SNew(STextBlock).Text(Text(TEXT("Generation draws upward static-mesh faces. Fit to level uses visible, collidable meshes; review bounds and cutoff to exclude roofs or sky. Use custom artwork for unsupported geometry. Floor bands and MinimapExclude are in capture Details."))).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)]
            +SVerticalBox::Slot().AutoHeight()[SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(16,8))
                +SWrapBox::Slot()[SNew(SCheckBox).IsEnabled_Lambda([this]{return !Model->Busy();}).IsChecked_Lambda([this]{return Model->UseCustomMinimap?ECheckBoxState::Unchecked:ECheckBoxState::Checked;})
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState Checked){if(Checked==ECheckBoxState::Checked)Model->UseCustomMinimap=false;})
                    [SNew(STextBlock).Text(Text(TEXT("Generate from geometry"))).Font(Font(10)).ColorAndOpacity(White)]]
                +SWrapBox::Slot()[SNew(SCheckBox).IsEnabled_Lambda([this]{return !Model->Busy();}).IsChecked_Lambda([this]{return Model->UseCustomMinimap?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState Checked){if(Checked==ECheckBoxState::Checked)Model->UseCustomMinimap=true;})
                    [SNew(STextBlock).Text(Text(TEXT("Custom image"))).Font(Font(10)).ColorAndOpacity(White)]]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,10)
            [SNew(SVerticalBox).Visibility_Lambda([this]{return Model->UseCustomMinimap?EVisibility::Visible:EVisibility::Collapsed;})
                +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(Text(TEXT("Use a 1024 x 1024 image. Align its coverage and north direction with the capture. Applying it records your review; automatic Prepare never redraws custom artwork."))).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)]
                +SVerticalBox::Slot().AutoHeight().Padding(0,8)[SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(7,7))
                    +SWrapBox::Slot()[Button(TEXT("Import image..."),[this]{Model->ChooseMinimapTexture(true);},false,[this]{return Model->CanAuthorMinimap();})]
                    +SWrapBox::Slot()[Button(TEXT("Use selected texture"),[this]{Model->ChooseMinimapTexture(false);},false,[this]{return Model->CanAuthorMinimap();})]]
                +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text_Lambda([this]{return Text(Model->CustomMinimapTexture.IsEmpty()?TEXT("No custom image selected"):Model->CustomMinimapTexture);}).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)]]
            +SVerticalBox::Slot().AutoHeight()[SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(7,7))
                +SWrapBox::Slot()[ActionButton(TEXT("save"))]
                +SWrapBox::Slot()[ActionButton(TEXT("minimap"))]
                +SWrapBox::Slot()[Button(TEXT("Open minimap preview"),[this]{Model->OpenMinimapPreview();},false,[this]{return !Model->Busy()&&Model->HasMinimapPreview();})]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,8,0,0)[SNew(STextBlock).Text(Text(TEXT("Save the capture first. Generate replaces the active floorplan while keeping imported source textures. Review the preview before exporting."))).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)]];
    }
    TSharedRef<SWidget> ModesCard()
    {
        TSharedRef<SWrapBox> Modes=SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(12,8));
        for(const FString& Id : {FString(TEXT("FFA")),FString(TEXT("TDM")),FString(TEXT("OBJ")),FString(TEXT("CTF")),FString(TEXT("DOM")),FString(TEXT("FT"))})
            Modes->AddSlot()[SNew(SCheckBox).IsEnabled_Lambda([this]{return !Model->Busy()&&Model->Modes!=TEXT("auto");})
                .IsChecked_Lambda([this,Id]{TArray<FString> Selected;(Model->Modes==TEXT("auto")?Model->DetectedModes():Model->Modes).ParseIntoArray(Selected,TEXT(","));return Selected.Contains(Id)?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                .OnCheckStateChanged_Lambda([this,Id](ECheckBoxState StateValue){TArray<FString> Selected;Model->Modes.ParseIntoArray(Selected,TEXT(","));if(StateValue==ECheckBoxState::Checked)Selected.AddUnique(Id);else Selected.Remove(Id);Model->Modes=FString::Join(Selected,TEXT(","));Model->Stale=true;Model->Ready=false;})
                [SNew(STextBlock).Text(Text(Id)).Font(Font(10)).ColorAndOpacity(White)]];
        return SNew(SBorder).BorderImage(&Card).BorderBackgroundColor(Surface).Padding(16)
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[SNew(STextBlock).Text(Text(TEXT("GAME MODES"))).Font(Font(10,true)).ColorAndOpacity(White)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[SNew(SCheckBox).IsEnabled_Lambda([this]{return !Model->Busy();})
                .IsChecked_Lambda([this]{return Model->Modes==TEXT("auto")?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                .OnCheckStateChanged_Lambda([this](ECheckBoxState Checked){Model->Modes=Checked==ECheckBoxState::Checked?TEXT("auto"):TEXT("FFA");Model->Stale=true;Model->Ready=false;})
                [SNew(STextBlock).Text(Text(TEXT("Detect the modes this map supports"))).Font(Font(10)).ColorAndOpacity(Muted)]]
            +SVerticalBox::Slot().AutoHeight()[Modes]];
    }
    TSharedRef<SWidget> Checklist()
    {
        TSharedRef<SVerticalBox> Rows=SNew(SVerticalBox);
        Rows->AddSlot().AutoHeight().Padding(0,0,0,10)[SNew(STextBlock).Text(Text(TEXT("MAP READINESS"))).Font(Font(10,true)).ColorAndOpacity(White)];
        for(int32 I=0;I<Model->Checks.Num()-1;++I)
            Rows->AddSlot().AutoHeight().Padding(0,4)
            [SNew(SHorizontalBox)
                +SHorizontalBox::Slot().AutoWidth().Padding(0,0,10,0)[SNew(SImage).Image_Lambda([this,I]
                    {int32 S=Model->Stale?0:Model->Checks[I].State;return FAppStyle::GetBrush(S==1?TEXT("Icons.Check"):S==3?TEXT("Icons.Error"):S==2?TEXT("Icons.Warning"):TEXT("Icons.Help"));})
                    .ColorAndOpacity_Lambda([this,I]{return CheckColor(Model->Stale?0:Model->Checks[I].State);})]
                +SHorizontalBox::Slot().FillWidth(1)[SNew(STextBlock).Text(Text(Model->Checks[I].Title)).Font(Font(10)).ColorAndOpacity(White)]
                +SHorizontalBox::Slot().AutoWidth().MaxWidth(200)[SNew(STextBlock).Text_Lambda([this,I]{return Text(Model->Stale?TEXT("Check needed"):Model->Checks[I].Detail);})
                    .Font(Font(9)).ColorAndOpacity_Lambda([this,I]{return CheckColor(Model->Stale?0:Model->Checks[I].State);}).AutoWrapText(true)]];
        return SNew(SBorder).BorderImage(&Card).BorderBackgroundColor(Surface).Padding(16)[Rows];
    }
    void CopyIssues(const TArray<FString>& Issues, const FString& Summary)
    {
        if(Issues.IsEmpty())return;
        FString Payload = TEXT("Retake Mod Kit - ") + Summary + TEXT("\nMod: ") + Model->ModId
            + (Model->IsMapWorkspace()?TEXT("\nScope: saved maps in this mod\n"):TEXT("\nScope: saved authoring assets in this mod\n"));
        if(Model->Stale) Payload += TEXT("This is the previous check; run the workspace check for current results.\n");
        Payload += TEXT("\n") + FString::Join(Issues,TEXT("\n\n"));
        FPlatformApplicationMisc::ClipboardCopy(*Payload);
        CopyFeedback = FString::Printf(TEXT("Copied %d %s to clipboard"),Issues.Num(),Issues.Num()==1?TEXT("issue"):TEXT("issues"));
    }
    TSharedRef<SWidget> IssuesCard()
    {
        TSharedPtr<SVerticalBox> List;
        TSharedRef<SWidget> Result= SNew(SBorder).BorderImage(&Card).BorderBackgroundColor(Surface).Padding(14)
            .Visibility_Lambda([this]{return Model->Issues.IsEmpty()?EVisibility::Collapsed:EVisibility::Visible;})
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)
            [SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(8,6))
                +SWrapBox::Slot()[Button(TEXT("Copy warnings"),[this]
                    {CopyIssues(Model->Issues.FilterByPredicate([](const FString& Issue){return Issue.StartsWith(TEXT("Note:"));}),TEXT("Warnings"));},false,
                    [this]{return Model->Warnings>0;})]
                +SWrapBox::Slot()[Button(TEXT("Copy all issues"),[this]{CopyIssues(Model->Issues,TEXT("Validation issues"));},false,
                    [this]{return !Model->Issues.IsEmpty();})]
                +SWrapBox::Slot()[SNew(STextBlock).Text_Lambda([this]{return Text(CopyFeedback);}).Font(Font(9)).ColorAndOpacity(Green)]]
            +SVerticalBox::Slot().AutoHeight()
            [SNew(SExpandableArea).InitiallyCollapsed(true)
                .HeaderContent()[SNew(STextBlock).Text_Lambda([this]{return Text(FString::Printf(TEXT("THINGS TO REVIEW  |  %d errors  /  %d warnings"),Model->Errors,Model->Warnings));}).Font(Font(10,true)).ColorAndOpacity(Amber)]
                .BodyContent()[SNew(SBox).MaxDesiredHeight(220).Padding(0,10,0,0)[SNew(SScrollBox)+SScrollBox::Slot()[SAssignNew(List,SVerticalBox)]]]]];
        IssueLists.Add(List);return Result;
    }
    void RebuildIssues()
    {
        CopyFeedback.Reset();
        for(const auto& IssueList:IssueLists)
        {
        if(!IssueList)continue;IssueList->ClearChildren();
        for(const FString& Issue:Model->Issues)
            IssueList->AddSlot().AutoHeight().Padding(0,0,0,9)
                [SNew(SHorizontalBox)
                    +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,10,0)[SNew(STextBlock).Text(Text(Issue)).Font(Font(10)).ColorAndOpacity(Issue.StartsWith(TEXT("Fix:"))?Red:Muted).AutoWrapText(true)]
                    +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top)[Button(TEXT("Copy"),[this,Issue]{CopyIssues({Issue},TEXT("Validation issue"));},false,[]{return true;})]];
        }
    }
    TSharedRef<SWidget> ElementsCard()
    {
        return SNew(SBorder).BorderImage(&Card).BorderBackgroundColor(Surface).Padding(16)
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(Text(TEXT("BUILD WITH RETAKE"))).Font(Font(10,true)).ColorAndOpacity(White)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,5,0,10)[SNew(STextBlock).Text(Text(TEXT("Place an element, then configure it in Details."))).Font(Font(10)).ColorAndOpacity(Muted)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)
                [SNew(SSearchBox).HintText(Text(TEXT("Find an element or tool..."))).OnTextChanged_Lambda([this](const FText& Value){Search=Value.ToString();RebuildPalette();})]
            +SVerticalBox::Slot().AutoHeight()[SNew(SBox).MaxDesiredHeight(260)
                [SNew(SScrollBox)+SScrollBox::Slot()[SAssignNew(Palette,SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(6,6))]]]];
    }
    void RebuildPalette()
    {
        if(!Palette)return;Palette->ClearChildren();
        const auto& Elements=FRetakeModKitState::Elements();
        for(int32 I=0;I<Elements.Num();++I)
        {
            const auto& E=Elements[I];if(!Search.IsEmpty()&&!FString(E.Name).Contains(Search)&&!FString(E.Group).Contains(Search))continue;
            Palette->AddSlot().FillEmptySpace(true)
            [SNew(SBox).MinDesiredWidth(150)
                [SNew(SButton).ButtonStyle(&Secondary).ContentPadding(FMargin(10,9)).IsEnabled_Lambda([this]{return Model->IsKit()&&!Model->Busy()&&!Model->ModId.IsEmpty();})
                    .OnClicked_Lambda([this,I]{Model->Place(I);return FReply::Handled();})
                    [SNew(SVerticalBox)
                        +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(Text(E.Name)).Font(Font(10,true)).ColorAndOpacity(White)]
                        +SVerticalBox::Slot().AutoHeight().Padding(0,3,0,0)[SNew(STextBlock).Text(Text(E.Group)).Font(Font(8)).ColorAndOpacity(Muted)]]]];
        }
    }
    TSharedRef<SWidget> InstallationsCard()
    {
        return SNew(SBorder).BorderImage(&Card).BorderBackgroundColor(Surface).Padding(16)
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(Text(TEXT("TEST INSTALLATIONS"))).Font(Font(10,true)).ColorAndOpacity(White)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,5,0,12)[SNew(STextBlock).Text(Text(TEXT("Use your existing client and server. Only mod files are installed; test profiles stay separate."))).Font(Font(10)).ColorAndOpacity(Muted).AutoWrapText(true)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)
            [SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,8,0)[SNew(STextBlock).Text_Lambda([this]{return Text(Model->GameExe.IsEmpty()?TEXT("Client  |  Not detected"):TEXT("Client  |  ")+Model->GameExe);}).Font(Font(9)).ColorAndOpacity_Lambda([this]{return Model->GameExe.IsEmpty()?Amber:!Model->TestCheckCurrent()?Muted:Model->ClientAllowed?Green:Red;}).AutoWrapText(true)]
                +SHorizontalBox::Slot().AutoWidth()[Button(TEXT("Browse"),[this]{Model->ChooseGame();})]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)
            [SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,8,0)[SNew(STextBlock).Text_Lambda([this]{return Text(Model->ServerPath.IsEmpty()?TEXT("Linux server  |  Optional"):TEXT("Linux server  |  ")+Model->ServerPath);}).Font(Font(9)).ColorAndOpacity_Lambda([this]{return Model->ServerPath.IsEmpty()||!Model->TestCheckCurrent()?Muted:Model->ServerAllowed?Green:Red;}).AutoWrapText(true)]
                +SHorizontalBox::Slot().AutoWidth()[Button(TEXT("Browse"),[this]{Model->ChooseServer();})]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[SNew(SCheckBox).IsEnabled_Lambda([this]{return !Model->Busy()&&!Model->ServerPath.IsEmpty();})
                .IsChecked_Lambda([this]{return Model->UseServer?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                .OnCheckStateChanged_Lambda([this](ECheckBoxState Checked){Model->UseServer=Checked==ECheckBoxState::Checked;Model->InvalidateTestCheck();})
                [SNew(STextBlock).Text(Text(TEXT("Launch a local dedicated server through WSL"))).Font(Font(10)).ColorAndOpacity(White)]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(8,8))
                +SWrapBox::Slot()[Button(TEXT("Find installations"),[this]{Model->DiscoverInstallations();})]
                +SWrapBox::Slot()[Button(TEXT("Refresh server status"),[this]{Model->Run(TEXT("server-status"));})]
                +SWrapBox::Slot()[ActionButton(TEXT("start-test-server"))]
                +SWrapBox::Slot()[Button(TEXT("Stop test server"),[this]{Model->Run(TEXT("stop-test-server"));},false,[this]{return !Model->Busy()&&Model->ServerStatus==TEXT("running");})]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,4,0,10)[SNew(STextBlock).Text_Lambda([this]{return Text(TEXT("Test server: ")+Model->ServerStatusDetail);}).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)]
            +SVerticalBox::Slot().AutoHeight()[SNew(SExpandableArea).InitiallyCollapsed(true)
                .HeaderContent()[SNew(STextBlock).Text(Text(TEXT("Advanced server settings"))).Font(Font(9)).ColorAndOpacity(Muted)]
                .BodyContent()[SNew(SVerticalBox)
                    +SVerticalBox::Slot().AutoHeight().Padding(0,6)[SNew(SEditableTextBox).HintText(Text(TEXT("WSL distribution (default when empty)"))).Text_Lambda([this]{return Text(Model->WslDistribution);}).OnTextCommitted_Lambda([this](const FText& T,ETextCommit::Type){Model->WslDistribution=T.ToString();Model->InvalidateTestCheck();})]
                    +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[SNew(SEditableTextBox).HintText(Text(TEXT("WSL user (default when empty)"))).Text_Lambda([this]{return Text(Model->WslUser);}).OnTextCommitted_Lambda([this](const FText& T,ETextCommit::Type){Model->WslUser=T.ToString();Model->InvalidateTestCheck();})]
                    +SVerticalBox::Slot().AutoHeight()[SNew(SEditableTextBox).HintText(Text(TEXT("Local test port"))).Text_Lambda([this]{return Text(FString::FromInt(Model->TestPort));}).OnTextCommitted_Lambda([this](const FText& T,ETextCommit::Type){Model->TestPort=FCString::Atoi(*T.ToString());Model->InvalidateTestCheck();})]]]];
    }
    TSharedRef<SWidget> ExportCard()
    {
        return SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text_Lambda([this]
                {return Text(Model->ExportIsCurrent()?TEXT("This saved source has been exported. Open the export folder to find the bundle."):
                    Model->CanExport()?TEXT("Your mod is ready. Choose a new revision and export."):TEXT("Resolve the checks above before exporting."));})
                .Font(Font(10)).ColorAndOpacity(Muted).AutoWrapText(true)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,12,0,0)
            [SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,8,0)[SNew(SEditableTextBox).Text_Lambda([this]{return Text(Model->Revision);}).HintText(Text(TEXT("Revision: letters and numbers")))
                    .IsEnabled_Lambda([this]{return !Model->Busy();}).OnTextCommitted_Lambda([this](const FText& Value,ETextCommit::Type){Model->Revision=Value.ToString();})]
                +SHorizontalBox::Slot().AutoWidth()[ActionButton(TEXT("export"))]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,10,0,0)[Button(TEXT("Open export folder"),[this]{Model->OpenOutput();},false,[this]{return !Model->LastExport.IsEmpty();})]
            +SVerticalBox::Slot().AutoHeight().Padding(0,9,0,0)[SNew(STextBlock).Text_Lambda([this]
                {return Text(Model->Candidate?TEXT("Kit export target: local test bundles. Public compatibility qualification is pending."):TEXT("Kit export target: ")+Model->Checks.Last().Detail);})
                .Font(Font(9)).ColorAndOpacity_Lambda([this]{return Model->Candidate?Amber:Muted;}).AutoWrapText(true)];
    }
    TSharedRef<SWidget> TestCard()
    {
        return SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(Text(TEXT("1. Choose an exported revision"))).Font(Font(11,true)).ColorAndOpacity(White)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,5,0,9)[SNew(STextBlock).Text(Text(TEXT("Existing bundles are found automatically and remembered between editor sessions. Editing a map does not update its older export."))).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)]
            +SVerticalBox::Slot().AutoHeight()
            [SAssignNew(ExportCombo,SComboBox<TSharedPtr<FString>>).OptionsSource(&Model->Exports).IsEnabled_Lambda([this]{return !Model->Busy();})
                .OnGenerateWidget_Lambda([this](TSharedPtr<FString> Item){return SNew(STextBlock).Text(Text(Model->ExportLabels.FindRef(*Item)));})
                .OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item,ESelectInfo::Type){if(Item)Model->SelectExport(*Item);})
                [SNew(STextBlock).Text_Lambda([this]{return Text(Model->HasExport()?Model->ExportLabels.FindRef(Model->LastExport):TEXT("No export selected - create one or choose an existing folder"));}).Margin(FMargin(6))]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,8,0,14)[SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(7,7))
                +SWrapBox::Slot()[Button(TEXT("Go to Export step"),[this]{if(WorkflowScroll)WorkflowScroll->ScrollDescendantIntoView(WorkflowSteps[4],true,EDescendantScrollDestination::TopOrLeft);},!Model->HasExport())]
                +SWrapBox::Slot()[Button(TEXT("Choose export folder..."),[this]{Model->ChooseExport();})]
                +SWrapBox::Slot()[Button(TEXT("Find existing exports"),[this]{Model->RefreshExports();++Model->ChangeSerial;Model->Changed.Broadcast();})]]
            +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(Text(TEXT("2. Check the installed client and server"))).Font(Font(11,true)).ColorAndOpacity(White)]
            +SVerticalBox::Slot().AutoHeight()[InstallationsCard()]
            +SVerticalBox::Slot().AutoHeight().Padding(0,8)[ActionButton(TEXT("test-info"))]
            +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text_Lambda([this]{return Text(TEXT("Client: ")+Model->ClientTestDetail);}).Font(Font(10)).ColorAndOpacity_Lambda([this]{return !Model->TestCheckCurrent()?Muted:Model->ClientAllowed?Green:Amber;}).AutoWrapText(true)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,5,0,14)[SNew(STextBlock).Visibility_Lambda([this]{return Model->UseServer?EVisibility::Visible:EVisibility::Collapsed;})
                .Text_Lambda([this]{return Text(TEXT("Server: ")+Model->ServerTestDetail);}).Font(Font(10)).ColorAndOpacity_Lambda([this]{return !Model->TestCheckCurrent()?Muted:Model->ServerAllowed?Green:Amber;}).AutoWrapText(true)]
            +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(Text(TEXT("3. Choose a mode and launch"))).Font(Font(11,true)).ColorAndOpacity(White)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,8)[SNew(STextBlock).Text_Lambda([this]
                {return Text(!Model->HasExport()?TEXT("Step 1 needs an exported revision. Use Go to Export step above."):
                    !Model->TestCheckCurrent()?TEXT("Check the selected installations before launching."):
                    !Model->TestReady?TEXT("Resolve the installation compatibility messages above. No game or server has been launched."):
                    Model->UseServer?TEXT("Start server & client will launch a local WSL server, wait for readiness, then connect the client."):TEXT("Launch client opens the selected exported map in your existing game."));})
                .Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)]
            +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,8,0)
                [SAssignNew(TestModeCombo,SComboBox<TSharedPtr<FString>>).OptionsSource(&Model->TestModes).IsEnabled_Lambda([this]{return !Model->Busy()&&Model->HasExport()&&!Model->TestModes.IsEmpty();})
                    .OnGenerateWidget_Lambda([](TSharedPtr<FString> Item){return SNew(STextBlock).Text(Text(*Item));})
                    .OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item,ESelectInfo::Type){if(Item)Model->TestMode=*Item;})
                    [SNew(STextBlock).Text_Lambda([this]{return Text(Model->TestMode.IsEmpty()?TEXT("Choose an export to list its modes"):TEXT("Game mode: ")+Model->TestMode);}).Margin(FMargin(6))]]
                +SHorizontalBox::Slot().AutoWidth()[ActionButton(TEXT("test"))]];
    }
    TSharedRef<SWidget> ActivityCard()
    {
        return SNew(SBorder).BorderImage(&Card).BorderBackgroundColor(Surface).Padding(14)
        [SNew(SExpandableArea).InitiallyCollapsed(true)
            .HeaderContent()[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1)[SNew(STextBlock).Text(Text(TEXT("ACTIVITY & DETAILS"))).Font(Font(9,true)).ColorAndOpacity(Muted)]
                +SHorizontalBox::Slot().AutoWidth()[Button(TEXT("Open log"),[this]{Model->OpenLog();},false,[this]{return !Model->LastLog.IsEmpty();})]]
            .BodyContent()[SNew(SBox).MaxDesiredHeight(220).Padding(0,10,0,0)[SNew(SScrollBox)+SScrollBox::Slot()
                [SNew(STextBlock).Text_Lambda([this]{return Text(Model->Activity.IsEmpty()?TEXT("Checks and export progress will appear here."):Model->Activity);}).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)]]]];
    }
};
}

TSharedRef<SWidget> RetakeModKitPanel::Create(FSimpleDelegate CreateMod) { return SNew(SRetakeModKitPanel).OnCreateMod(CreateMod); }
void RetakeModKitPanel::SelectWorkspace(const FString& ModId) { if(State)State->Select(ModId); }
void RetakeModKitPanel::Action(const FString& Name)
{
    if(!State)State=MakeShared<FRetakeModKitState>();
    if(Name==TEXT("discover"))State->DiscoverInstallations();else if(Name==TEXT("save"))State->Save();else if(Name==TEXT("setup"))State->SetupTools();else if(Name==TEXT("output"))State->OpenOutput();else State->Run(Name);
}
FString RetakeModKitPanel::SelectedKind(){return State?State->ActiveKind:FString(TEXT("map"));}
bool RetakeModKitPanel::IsMapWorkflow(){return !State||State->IsMapWorkspace();}
void RetakeModKitPanel::Shutdown() { if(State)State->StopService(); State.Reset(); }
