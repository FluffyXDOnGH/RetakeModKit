// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeMapSDKCompatibilityCommandlets.h"
#include "RetakeMapSDKIds.h"
#include "RetakeMapSDKModeRequirements.h"
#include "RetakeMapSDKProfile.h"
#include "RetakeMapSDKVersion.h"
#include "RetakeMapSDKLog.h"
#include "CoreGlobals.h"
#include "AssetRegistry/AssetRegistryState.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Hash/Blake3.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "Serialization/CustomVersion.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Class.h"
#include "UObject/ObjectVersion.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace
{
    bool IsSdk(const UObject* Object)
    {
        if (!Object) return false;
        const FString Name = Object->GetOutermost()->GetName();
        return Name == TEXT("/Script/RetakeMapSDK") || Name == TEXT("/Script/RetakeDestruction");
    }

    bool Save(const FString& Params, TSharedRef<FJsonObject> Object)
    {
        FString Path, Json;
        if (!FParse::Value(*Params, TEXT("Out="), Path) || Path.IsEmpty()) return false;
        return FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Json))
            && FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }

    void Properties(TMap<FString, FString>& Out, const FString& Prefix, UStruct* Type, const void* Defaults, bool bOnlySdkOwner)
    {
        for (TFieldIterator<FProperty> It(Type); It; ++It)
        {
            const FProperty* Property = *It;
            if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_EditorOnly | CPF_SkipSerialization)
                || (bOnlySdkOwner && !IsSdk(Property->GetOwnerStruct()))) continue;
            const FString Key = Prefix + TEXT("/property/") + Property->GetName();
            Out.Add(Key + TEXT("/type"), Property->GetCPPType());
            Out.Add(Key + TEXT("/array"), FString::FromInt(Property->ArrayDim));
            Out.Add(Key + TEXT("/flags"), LexToString(static_cast<uint64>(Property->PropertyFlags & (CPF_Net | CPF_RepNotify | CPF_InstancedReference | CPF_ContainsInstancedReference | CPF_SaveGame))));
            if (Defaults)
            {
                FString Value;
                Property->ExportText_InContainer(0, Value, Defaults, nullptr, nullptr, PPF_None);
                Out.Add(Key + TEXT("/default"), Value);
            }
        }
    }
}

URetakeMapSDKContractCommandlet::URetakeMapSDKContractCommandlet()
{ IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true; }

int32 URetakeMapSDKContractCommandlet::Main(const FString& Params)
{
    if (!FModuleManager::Get().LoadModule(TEXT("RetakeDestruction"))) return 1;
    TMap<FString, FString> Entries;
    for (TObjectIterator<UStruct> It; It; ++It)
    {
        UStruct* Type = *It;
        if (!IsSdk(Type) || Type->IsA<UFunction>()) continue;
        const FString Key = Type->GetPathName();
        UClass* Class = Cast<UClass>(Type);
        if (Class && !Class->HasAnyClassFlags(CLASS_Native)) continue;
        Entries.Add(Key + TEXT("/kind"), Class ? TEXT("class") : TEXT("struct"));
        Entries.Add(Key + TEXT("/base"), Type->GetSuperStruct() ? Type->GetSuperStruct()->GetPathName() : FString());
        UObject* CDO = Class ? Class->GetDefaultObject() : nullptr;
        TArray<uint8> StructMemory;
        UScriptStruct* Struct = Cast<UScriptStruct>(Type);
        if (Struct) { StructMemory.SetNumZeroed(Struct->GetStructureSize()); Struct->InitializeStruct(StructMemory.GetData()); }
        Properties(Entries, Key, Type, CDO ? static_cast<void*>(CDO) : StructMemory.GetData(), !Class);
        if (Struct) Struct->DestroyStruct(StructMemory.GetData());
        if (CDO)
        {
            TArray<UObject*> Subobjects;
            CDO->GetDefaultSubobjects(Subobjects);
            for (UObject* Subobject : Subobjects)
            {
                const FString SubKey = Key + TEXT("/subobject/") + Subobject->GetPathName(CDO);
                Entries.Add(SubKey + TEXT("/class"), Subobject->GetClass()->GetPathName());
                Properties(Entries, SubKey, Subobject->GetClass(), Subobject, false);
            }
        }
    }
    for (TObjectIterator<UEnum> It; It; ++It)
    {
        if (!IsSdk(*It)) continue;
        for (int32 Index = 0; Index < It->NumEnums(); ++Index)
        {
            const FString Name = It->GetNameStringByIndex(Index);
            if (!Name.EndsWith(TEXT("_MAX"))) Entries.Add(It->GetPathName() + TEXT("/enum/") + Name, LexToString(It->GetValueByIndex(Index)));
        }
    }
    const auto Ids = [&Entries](const TCHAR* Family, const TArray<FName>& Values)
    { for (FName Value : Values) Entries.Add(FString(TEXT("ids/")) + Family + TEXT("/") + Value.ToString(), TEXT("available")); };
    Ids(TEXT("devices"), RetakeMapSDKIds::DevicePresentations());
    Ids(TEXT("weapons"), RetakeMapSDKIds::Weapons());
    Ids(TEXT("boost"), RetakeMapSDKIds::BoostProfiles());
    for (const auto& Mode : RetakeMapSDK::ModeRequirements)
        Entries.Add(FString(TEXT("modes/")) + Mode.ModeId, FString::Printf(TEXT("%d,%d,%d,%d,%d,%d,%d,%d"),
            static_cast<int32>(Mode.Starts), Mode.MinStarts, Mode.AdvisedStarts, Mode.MinBombSites, Mode.MinControlPoints,
            Mode.bCaptureTheFlag, Mode.MinRelays, Mode.bRequireSharedStart));
    Entries.Add(TEXT("manifest/encoding"), FString::FromInt(RetakeMapSDK::ManifestSchemaVersion));
    Entries.Add(TEXT("manifest/weapon-minimum-sdk"),FString::FromInt(RetakeMapSDK::MinimumWeaponContract));
    TArray<FString> Keys; Entries.GetKeys(Keys); Keys.Sort();
    auto Surface = MakeShared<FJsonObject>();
    for (const FString& Key : Keys) Surface->SetStringField(Key, Entries[Key]);
    auto Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("format"), 1);
    Root->SetNumberField(TEXT("contractVersion"), RetakeMapSDK::ContractVersion);
    Root->SetNumberField(TEXT("oldestReadableContract"), RetakeMapSDK::OldestReadableContract);
    Root->SetObjectField(TEXT("surface"), Surface);
    UE_LOGF(LogRetakeMapSDK, Display, "event=MapContractCaptured entries=%d contract=%d oldest=%d", Entries.Num(), RetakeMapSDK::ContractVersion, RetakeMapSDK::OldestReadableContract);
    return Save(Params, Root) ? 0 : 1;
}

URetakeMapSDKRecordCommandlet::URetakeMapSDKRecordCommandlet()
{ IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true; }

int32 URetakeMapSDKRecordCommandlet::Main(const FString& Params)
{
    FString Platform;
    FParse::Value(*Params, TEXT("Platform="), Platform);
    if (Platform != TEXT("Windows") && Platform != TEXT("LinuxServer")) return 1;
    const FString IniPlatform = Platform == TEXT("Windows") ? TEXT("Windows") : TEXT("Linux");
    FConfigCacheIni* Config = FConfigCacheIni::ForPlatform(FName(*IniPlatform));
    if (!Config) return 1;
    auto Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("format"), 1);
    Root->SetStringField(TEXT("platform"), Platform);
    Root->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
#if defined(_MSC_FULL_VER)
    Root->SetStringField(TEXT("compilerIdentity"), FString::Printf(TEXT("MSVC:%d"), _MSC_FULL_VER));
#elif defined(__clang__)
    Root->SetStringField(TEXT("compilerIdentity"), FString::Printf(TEXT("Clang:%d.%d.%d"), __clang_major__, __clang_minor__, __clang_patchlevel__));
#else
    Root->SetStringField(TEXT("compilerIdentity"), TEXT("unqualified-compiler"));
#endif
    Root->SetNumberField(TEXT("packageUE4"), GPackageFileUEVersion.FileVersionUE4);
    Root->SetNumberField(TEXT("packageUE5"), GPackageFileUEVersion.FileVersionUE5);
    Root->SetNumberField(TEXT("licenseeVersion"), GPackageFileLicenseeUEVersion);
    Root->SetStringField(TEXT("cookProfile"), FRetakeMapSDKProfile::BuildCanonicalText(*Config, IniPlatform));
    TArray<TSharedPtr<FJsonValue>> Structural;
    for (const FString& Entry : FRetakeMapSDKProfile::StructuralEntries(*Config)) Structural.Add(MakeShared<FJsonValueString>(Entry));
    Root->SetArrayField(TEXT("structuralProfile"), Structural);
    auto Versions = MakeShared<FJsonObject>();
    for (const FCustomVersion& Version : FCurrentCustomVersions::GetAll().GetAllVersions())
        Versions->SetNumberField(Version.Key.ToString(EGuidFormats::DigitsWithHyphensLower), Version.Version);
    Root->SetObjectField(TEXT("customVersions"), Versions);
    // These are diagnostic evidence, not an automatic compatibility verdict. Qualification reviews the complete source record.
    auto Schemas = MakeShared<FJsonObject>();
    for (TObjectIterator<UClass> It; It; ++It)
    {
        if (!It->HasAnyClassFlags(CLASS_Native) || IsSdk(*It)) continue;
        FBlake3 Hasher;
        FAppendToClassSchemaContext Context(&Hasher);
        It->CallAppendToClassSchema(Context);
        Schemas->SetStringField(It->GetPathName(), LexToString(Hasher.Finalize()).ToLower());
    }
    Root->SetObjectField(TEXT("classSchemaEvidence"), Schemas);
    return Save(Params, Root) ? 0 : 1;
}


URetakeMapSDKBaseRegistryCommandlet::URetakeMapSDKBaseRegistryCommandlet()
{ IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true; }

int32 URetakeMapSDKBaseRegistryCommandlet::Main(const FString& Params)
{
    FString Input, Base, Output, Json;
    if (!FParse::Value(*Params, TEXT("In="), Input) || !FParse::Value(*Params, TEXT("Base="), Base)
        || !FParse::Value(*Params, TEXT("Out="), Output) || !FFileHelper::LoadFileToString(Json, *Base)) return 1;
    TSharedPtr<FJsonObject> Manifest;
    const TArray<TSharedPtr<FJsonValue>>* Packages = nullptr;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Manifest) || !Manifest
        || !Manifest->TryGetArrayField(TEXT("packages"), Packages) || Packages->IsEmpty()) return 1;
    TSet<FName> Keep;
    for (const auto& Value : *Packages)
    {
        FString Name;
        if (!Value->TryGetString(Name)) return 1;
        bool bAllowed = false;
        for (const TCHAR* Root : RetakeMapSDK::AllowedForeignRoots) bAllowed |= Name.StartsWith(Root, ESearchCase::CaseSensitive);
        if (!bAllowed) return 1;
        Keep.Add(FName(*Name));
    }
    FAssetRegistryState Source, Filtered;
    FAssetRegistryLoadOptions LoadOptions;
    if (!FAssetRegistryState::LoadFromDisk(*Input, LoadOptions, Source)) return 1;
    FAssetRegistrySerializationOptions Options;
    Options.bSerializeAssetRegistry = true;
    Options.bSerializePackageData = true;
    Options.bSerializeDependencies = false;
    Options.bSerializeSearchableNameDependencies = false;
    Options.bSerializeManageDependencies = false;
    Options.bUseAssetRegistryTagsAllowListInsteadOfDenyList = true;
    Options.CookFilterlistTagsByClass.Reset();
    Options.CookFilterlistTagPrefixesByClass.Reset();
    Options.bFilterAssetDataWithNoTags = false;
    Options.bKeepDevelopmentAssetRegistryTags = false;
    Filtered.InitializeFromExistingAndPrune(Source, Keep, {}, {}, Options);
    Filtered.FilterTags(Options);
    for (FName Package : Keep)
        if (!Filtered.GetAssetPackageData(Package))
        { UE_LOGF(LogRetakeMapSDK, Error, "Base cook registry lacks required package %ls", *Package.ToString()); return 1; }
    TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*Output));
    if (!Writer || !Filtered.Save(*Writer, Options) || Writer->IsError() || !Writer->Close()) return 1;
    UE_LOGF(LogRetakeMapSDK, Display, "event=ModBaseRegistryFiltered packages=%d", Keep.Num());
    return 0;
}
