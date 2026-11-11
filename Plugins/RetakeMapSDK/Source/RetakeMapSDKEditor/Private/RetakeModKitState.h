// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"

class FJsonObject;
class UWorld;
class ARetakeMinimapCapture;
class URetakeMinimapDefinition;

struct FRetakeCreatorCheck
{
    FString Title, Detail;
    int32 State = 0; // 0 unchecked, 1 passed, 2 attention, 3 blocked.
};

struct FRetakeCreatorElement
{
    const TCHAR* Name;
    const TCHAR* Group;
    const TCHAR* Class;
    int32 Preset = 0;
};

/** One editor-owned job service; closing/reopening the tab never loses a running operation. */
class FRetakeModKitState : public TSharedFromThis<FRetakeModKitState>
{
public:
    FRetakeModKitState();
    ~FRetakeModKitState();
    void Select(const FString& Mod);
    void SelectKind(const FString& Kind);
    bool IsMapWorkspace() const { return ActiveKind == TEXT("map"); }
    bool CanAuthorCurrentKind() const;
    FString KindDescription() const;
    void OpenContent();
    void CreateWeaponAsset(const FString& Role);
    void OpenWeaponCatalogue();
    void Refresh();
    void OpenMap();
    void OpenExample();
    void StopService();
    void Save();
    void SetupTools();
    void Run(const FString& Action);
    void Cancel();
    void Place(int32 Index);
    void SetupMinimap();
    void FitMinimap(bool SelectionOnly);
    void SelectMinimapCapture();
    void SetMinimapValue(const FString& Field, double Value);
    TOptional<double> MinimapValue(const FString& Field) const;
    void ChooseMinimapTexture(bool Import);
    void OpenMinimapPreview();
    bool CanAuthorMinimap() const;
    bool CanBakeMinimap() const;
    bool HasMinimapPreview() const;
    bool HasMinimapCapture() const;
    bool MinimapIsCurrent() const;
    void ChooseGame();
    void ChooseServer();
    void ChooseExport();
    void SelectExport(const FString& Path);
    void RefreshExports();
    void InvalidateTestCheck();
    bool HasExport() const;
    bool CanStartServer() const;
    bool TestCheckCurrent() const;
    bool IsTestingWorkflow() const;
    void DiscoverInstallations();
    void OpenOutput() const;
    void OpenLog() const;
    bool IsKit() const;
    bool Busy() const { return Process.IsValid(); }
    bool IsRunning(const TCHAR* Action) const { return Busy() && CurrentAction == Action; }
    bool FailedAction(const TCHAR* Action) const { return OperationFailed && CurrentAction == Action; }
    bool HasTools() const;
    bool HasUnsaved() const;
    bool CanExport() const;
    bool CanPrepare() const;
    void SelectMap(const FString& Map);
    FString DetectedModes() const;
    bool CanTest() const;
    bool ExportIsCurrent() const { return !LastExport.IsEmpty() && !Stale && ExportStamp == ValidationStamp; }
    FString StatusTitle() const;
    FString StatusDetail() const;
    static const TArray<FRetakeCreatorElement>& Elements();

    FString ActiveKind=TEXT("map");
    TMap<FString,FString> LastWorkspaceByKind;
    FString ModId, MapName, Modes = TEXT("auto"), Revision, GameExe, ServerPath, WslDistribution, WslUser;
    bool UseServer = false, TestLaunched = false;
    int32 TestPort = 17777;
    FString Notice, Phase, LastLog, LastExport, LastRecovery, Activity;
    TArray<TSharedPtr<FString>> Mods, Maps;
    TMap<FString, FString> MapPackages;
    TArray<FRetakeCreatorCheck> Checks, PreparationChecks;
    TMap<FString, FString> MapModes;
    TArray<TSharedPtr<FString>> TestModes;
    FString TestMode;
    TArray<TSharedPtr<FString>> Exports;
    TMap<FString, FString> ExportLabels, RememberedExports;
    FString ClientTestDetail = TEXT("Selected client compatibility has not been checked."), ServerTestDetail;
    FString ServerStatus = TEXT("Not checked"), ServerStatusDetail = TEXT("Refresh status to inspect the Kit-managed server.");
    bool TestReady = false, ClientAllowed = false, ServerAllowed = false;
    bool UseCustomMinimap = false;
    FString CustomMinimapTexture, MinimapMessage;
    bool PreparationCurrent = false, PreparationNeeded = false, PreparationBlocked = false;
    TArray<FString> Issues;
    bool Valid = false, Ready = false, Stale = true, Candidate = false, OperationFailed = false;
    int32 Errors = 0, Warnings = 0, ChangeSerial = 0;
    FSimpleMulticastDelegate Changed;
private:
    struct FCommand { FString Program, Arguments, Label; };
    TArray<FCommand> Queue;
    FProcHandle Process;
    void* ReadPipe = nullptr;
    void* WritePipe = nullptr;
    FTSTicker::FDelegateHandle Ticker;
    FString ExportStamp, TestCheckStamp, JobTestStamp;
    FString TestSelectionStamp() const;
    FString CurrentAction, ReportPath, ReturnMap, PreparationPlaceholder, JobDirectory, PendingOutput, ValidationStamp, JobStartStamp;
    double NextRefresh = 0;
    bool Tick(float);
    void StartNext();
    void Finished(int32 Code);
    bool ReleaseModPackages();
    void RestoreMap();
    void ReadReport();
    void ReadDraftReport();
    void RefreshTestModes();
    void RefreshMinimapState();
    UWorld* EditingMap() const;
    TWeakObjectPtr<ARetakeMinimapCapture> MinimapCapture;
    TWeakObjectPtr<URetakeMinimapDefinition> MinimapDefinition;
    FString MinimapChoiceForMap;
    void RecordOutput(const FString& Text);
    void SavePreferences() const;
    FString SourceStamp() const;
    FString Python() const;
    bool ToolsReady = false;
    FString Script() const;
    static FString Quote(const FString& Value);
};
