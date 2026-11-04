// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeMapCompatibility.h"
#include "RetakeMapSDKManifest.h"
#include "RetakeMapSDKProfile.h"
#include "RetakeMapSDKVersion.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/AsyncLoading2.h"
#include "Serialization/CustomVersion.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/MemoryReader.h"
#include "UObject/ObjectVersion.h"
#include "UObject/Package.h"

namespace
{
    const FJsonObject* FindPair(const FJsonObject& Policy, const FString& Platform, const FString& Producer, const FString* MaterialPath)
    {
        const TArray<TSharedPtr<FJsonValue>>* Pairs = nullptr;
        if (!Policy.TryGetArrayField(TEXT("pairs"), Pairs)) return nullptr;
        bool bQualificationOnly = false;
        Policy.TryGetBoolField(TEXT("qualificationOnly"), bQualificationOnly);
        for (const auto& Value : *Pairs)
        {
            const TSharedPtr<FJsonObject>* Pair = nullptr;
            FString CookPlatform, Record, Material, Consumer;
            bool bReviewed = false;
            const TArray<TSharedPtr<FJsonValue>>* Evidence = nullptr;
            if (Value->TryGetObject(Pair) && (*Pair)->TryGetStringField(TEXT("platform"), CookPlatform) && CookPlatform == Platform
                && (*Pair)->TryGetStringField(TEXT("producerRecord"), Record) && Record == Producer
                && (*Pair)->TryGetStringField(TEXT("consumerRecord"), Consumer) && Consumer == Policy.GetStringField(TEXT("consumerRecord"))
                && (*Pair)->TryGetStringField(TEXT("materialPath"), Material) && (!MaterialPath || Material == *MaterialPath)
                && (*Pair)->TryGetBoolField(TEXT("reviewed"), bReviewed)
                && (bReviewed || (bQualificationOnly && Record == Consumer))
                && (*Pair)->TryGetArrayField(TEXT("evidence"), Evidence) && !Evidence->IsEmpty()) return Pair->Get();
        }
        return nullptr;
    }

    bool Contains(const FJsonObject& Object, const TCHAR* Field, const FString& Value)
    {
        const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
        return Object.TryGetArrayField(Field, Items) && Items->ContainsByPredicate([&Value](const auto& Item)
        { FString Text; return Item->TryGetString(Text) && Text == Value; });
    }
}

bool FRetakeMapCompatibility::IsHash(const FString& Value)
{
    if (Value.Len() != 64) return false;
    for (TCHAR C : Value) if (!((C >= '0' && C <= '9') || (C >= 'a' && C <= 'f'))) return false;
    return true;
}

bool FRetakeMapCompatibility::LoadPolicy(TSharedPtr<FJsonObject>& Out, FString& Error)
{
    // A missing/stale policy disables mod loading, not normal base-game startup. No mod may supply its own policy.
    static TSharedPtr<FJsonObject> Cached;
    if (Cached) { Out = Cached; return true; }
    const FString Path = FPaths::Combine(FPaths::ProjectDir(), TEXT("ModKit/compatibility.json"));
    FString Text;
    const int64 Size = IFileManager::Get().FileSize(*Path);
    if (Size <= 0 || Size > 16 * 1024 * 1024 || !FFileHelper::LoadFileToString(Text, *Path)
        || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Out) || !Out)
        { Error = TEXT("this game has no readable ModKit/compatibility.json qualification; install a qualified game build"); return false; }
    int32 Version = 0;
    FString Hash, Consumer;
    int64 ExecutableSize = 0;
    if (!Out->TryGetNumberField(TEXT("format"), Version) || Version != 1
        || !Out->TryGetStringField(TEXT("consumerRecord"), Consumer) || !IsHash(Consumer)
        || !Out->TryGetStringField(TEXT("executableHash"), Hash) || !IsHash(Hash)
        || FRetakeModManifest::HashFile(FPlatformProcess::ExecutablePath(), ExecutableSize) != Hash)
        { Error = TEXT("the mod qualification does not match this executable; reinstall the matching game package"); Out.Reset(); return false; }
    bool bQualificationOnly = false;
    Out->TryGetBoolField(TEXT("qualificationOnly"), bQualificationOnly);
    if (bQualificationOnly && (UE_BUILD_SHIPPING || UE_BUILD_TEST || !FParse::Param(FCommandLine::Get(), TEXT("RetakeModQualification"))))
        { Error = TEXT("this is an unpublished qualification package; the studio Development gate requires -RetakeModQualification"); Out.Reset(); return false; }
    Cached = Out;
    return true;
}

bool FRetakeMapCompatibility::Check(const FRetakeModManifest& Manifest, const FString& Platform, FString& Error)
{
    if (Manifest.SdkContractVersion < RetakeMapSDK::OldestReadableContract || Manifest.SdkContractVersion > RetakeMapSDK::ContractVersion)
        { Error = FString::Printf(TEXT("SDK %d is outside this game's readable range %d..%d; re-export or update Retake"),
            Manifest.SdkContractVersion, RetakeMapSDK::OldestReadableContract, RetakeMapSDK::ContractVersion); return false; }
    const FRetakeModPlatformCompatibility* Cook = Manifest.Compatibility.FindByPredicate([&](const auto& C) { return C.Platform == Platform; });
    if (!Cook) { Error = TEXT("the mod has no compatibility record for this platform"); return false; }
    TSharedPtr<FJsonObject> Policy;
    if (!LoadPolicy(Policy, Error)) return false;
    const FJsonObject* Pair = FindPair(*Policy, Platform, Cook->EngineRecordId, &Cook->MaterialPath);
    if (!Pair) { Error = FString::Printf(TEXT("engine cook record %s / %s / %s has not been qualified for this game; use a qualified engine or recook"),
        *Cook->EngineRecordId, *Platform, *Cook->MaterialPath); return false; }
    const TArray<FString> RuntimeProfile = FRetakeMapSDKProfile::StructuralEntries(*GConfig);
    for (const FString& Entry : Cook->StructuralProfile)
        if (!RuntimeProfile.Contains(Entry)) { Error = TEXT("a collision channel, profile or physical surface changed: ") + Entry; return false; }
    for (const FString& Package : Cook->ForeignPackages)
        if (!Contains(*Policy, TEXT("packages"), Package)) { Error = TEXT("this target does not ship required base package ") + Package; return false; }
    for (const FString& Class : Cook->NativeClasses)
    {
        FString Module, Name;
        if (!Class.StartsWith(TEXT("/Script/")) || !Class.Mid(8).Split(TEXT("."), &Module, &Name)
            || !Contains(*Policy, TEXT("scriptModules"), Module)) { Error = TEXT("this target does not ship required native class module: ") + Class; return false; }
    }
    return true;
}

bool FRetakeMapCompatibility::InspectPackage(TConstArrayView<uint8> Data, FRetakeModPackageVersions& Out, FString& Error)
{
    static_assert(static_cast<uint32>(EZenPackageVersion::Latest) == 3, "Review the bounded mod package reader for the new Zen header.");
    constexpr int32 MaxCustomVersions = 4096;
    Out = {};
    if (Data.Num() < sizeof(FZenPackageSummary) + 20) { Error = TEXT("truncated mod package header"); return false; }
    FZenPackageSummary Summary;
    FMemory::Memcpy(&Summary, Data.GetData(), sizeof(Summary));
    if (Summary.bHasVersioningInfo != 1 || (Summary.PackageFlags & PKG_UnversionedProperties) != 0)
        { Error = TEXT("unversioned mod package refused; export with tagged/versioned cooking"); return false; }
    if (Summary.HeaderSize > static_cast<uint32>(Data.Num()) || Summary.HeaderSize < sizeof(Summary) + 20)
        { Error = TEXT("invalid mod package header size"); return false; }
    FMemoryReaderView Ar(Data);
    Ar.Seek(sizeof(Summary));
    uint32 Zen = 0;
    FPackageFileVersion PackageVersion;
    int32 Count = 0;
    Ar << Zen << PackageVersion << Out.Licensee << Count;
    if (Ar.IsError() || Zen > static_cast<uint32>(EZenPackageVersion::Latest) || !GPackageFileUEVersion.IsCompatible(PackageVersion)
        || PackageVersion.FileVersionUE4 < 0 || PackageVersion.FileVersionUE5 < 0
        || Out.Licensee < 0 || Out.Licensee > GPackageFileLicenseeUEVersion || Count < 0 || Count > MaxCustomVersions
        || Ar.Tell() + static_cast<int64>(Count) * 20 > Summary.HeaderSize)
        { Error = TEXT("newer or malformed package/custom version header; use a qualified engine"); return false; }
    Out.UE4 = PackageVersion.FileVersionUE4; Out.UE5 = PackageVersion.FileVersionUE5;
    for (int32 I = 0; I < Count; ++I)
    {
        FGuid Key; int32 Value = 0;
        Ar << Key << Value;
        const FString Name = Key.ToString(EGuidFormats::DigitsWithHyphensLower);
        const auto Registered = FCurrentCustomVersions::Get(Key);
        if (Ar.IsError() || Out.CustomVersions.Contains(Name) || !Registered || Value < 0 || Value > Registered->Version)
            { Error = FString::Printf(TEXT("unknown, duplicate or newer custom version %s=%d"), *Name, Value); return false; }
        Out.CustomVersions.Add(Name, Value);
    }
    return true;
}

bool FRetakeMapCompatibility::CheckPackage(TConstArrayView<uint8> Data, const FRetakeModPlatformCompatibility& Compatibility, FString& Error)
{
    FRetakeModPackageVersions Versions;
    if (!InspectPackage(Data, Versions, Error)) return false;
    TSharedPtr<FJsonObject> Policy;
    if (!LoadPolicy(Policy, Error)) return false;
    const FJsonObject* Pair = FindPair(*Policy, Compatibility.Platform, Compatibility.EngineRecordId, &Compatibility.MaterialPath);
    const TSharedPtr<FJsonObject>* Ranges = nullptr;
    if (!Pair || !Pair->TryGetObjectField(TEXT("customVersions"), Ranges))
        { Error = TEXT("no reviewed custom-version ranges for this engine pair"); return false; }
    int32 ExpectedUE4 = -1, ExpectedUE5 = -1, ExpectedLicensee = -1;
    if (!Pair->TryGetNumberField(TEXT("packageUE4"), ExpectedUE4) || !Pair->TryGetNumberField(TEXT("packageUE5"), ExpectedUE5)
        || !Pair->TryGetNumberField(TEXT("licenseeVersion"), ExpectedLicensee) || Versions.UE4 != ExpectedUE4
        || Versions.UE5 != ExpectedUE5 || Versions.Licensee != ExpectedLicensee)
        { Error = TEXT("package versions do not match the declared engine cook record"); return false; }
    for (const auto& Version : Versions.CustomVersions)
    {
        const int32* DeclaredVersion = Compatibility.CustomVersions.Find(Version.Key);
        if (!DeclaredVersion || *DeclaredVersion != Version.Value)
            { Error = FString::Printf(TEXT("package custom version %s=%d differs from its manifest"), *Version.Key, Version.Value); return false; }
        const TSharedPtr<FJsonObject>* Range = nullptr;
        int32 Minimum = 0, Maximum = 0;
        if (!(*Ranges)->TryGetObjectField(Version.Key, Range)
            || !(*Range)->TryGetNumberField(TEXT("min"), Minimum) || !(*Range)->TryGetNumberField(TEXT("max"), Maximum)
            || Version.Value < Minimum || Version.Value > Maximum)
            { Error = FString::Printf(TEXT("custom version %s=%d is outside its reviewed readable range"), *Version.Key, Version.Value); return false; }
    }
    return true;
}
