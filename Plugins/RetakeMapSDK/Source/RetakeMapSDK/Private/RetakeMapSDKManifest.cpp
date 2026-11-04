// Copyright 2026 Codnyx. All Rights Reserved.

#include "RetakeMapSDKManifest.h"
#include "RetakeMapSDKVersion.h"
#include "Dom/JsonObject.h"
#include "Hash/Blake3.h"
#include "HAL/FileManager.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Serialization/Archive.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    bool IsTokenChar(TCHAR C, bool bAllowUnderscore)
    {
        return FChar::IsAlnum(C) || (bAllowUnderscore && C == TEXT('_'));
    }

    bool IsToken(const FString& Value, int32 MaxLength, bool bAllowUnderscore)
    {
        if (Value.IsEmpty() || Value.Len() > MaxLength) return false;
        for (TCHAR C : Value)
            if (C > 127 || !IsTokenChar(C, bAllowUnderscore)) return false;
        return true;
    }

    /** Display text reaches logs, refusal messages and the replicated catalogue: bounded, single-line, no quotes. */
    bool IsSafeDisplayText(const FString& Value, int32 MaxLength)
    {
        if (Value.Len() > MaxLength) return false;
        for (TCHAR C : Value)
            if (C < 32 || C == 127 || C == TEXT('"') || C == TEXT('\\') || C == TEXT('?') || C == TEXT('=')) return false;
        return true;
    }

    bool IsSafeRelativePath(const FString& Path)
    {
        if (Path.IsEmpty() || Path.Contains(TEXT("\\")) || Path.StartsWith(TEXT("/")) || Path.Contains(TEXT(":"))) return false;
        TArray<FString> Parts;
        Path.ParseIntoArray(Parts, TEXT("/"), false);
        for (const FString& Part : Parts)
            if (Part.IsEmpty() || Part == TEXT(".") || Part == TEXT("..")) return false;
        return true;
    }

    /** An asset's object path inside Root: "/<ModId>/Folder/Asset.Asset", never a subobject. */
    bool IsModObjectPath(const FString& Value, const FString& Root)
    {
        // Validation precedes mounting. IsValidObjectPath requires a registered root and rejects every new mod here.
        int32 ObjectDelimiter = INDEX_NONE;
        if (!Value.FindChar(TEXT('.'), ObjectDelimiter) || Value.Contains(TEXT(":"))) return false;
        const FString Package = Value.Left(ObjectDelimiter);
        return Package.StartsWith(Root, ESearchCase::CaseSensitive)
            && FPackageName::IsValidTextForLongPackageName(Package)
            && IsSafeRelativePath(Package.Mid(Root.Len()))
            && Value.Equals(Package + TEXT(".") + FPackageName::GetShortName(Package), ESearchCase::CaseSensitive);
    }
}

bool FRetakeModManifest::IsValidModId(const FString& Value)
{
    return Value.StartsWith(TEXT("RetakeMod_"), ESearchCase::CaseSensitive) && Value.Len() > 10 && IsToken(Value, 48, true);
}

bool FRetakeModManifest::IsValidRevision(const FString& Value)
{
    return IsToken(Value, 32, false);
}

bool FRetakeModManifest::IsValidDisplayName(const FString& Value)
{
    return !Value.TrimStartAndEnd().IsEmpty() && IsSafeDisplayText(Value, 64);
}

bool FRetakeModManifest::IsValidDescription(const FString& Value)
{
    if (Value.Len() > 280) return false;
    for (TCHAR C : Value)
        if (C < 32 || C == 127 || C == TEXT('"') || C == TEXT('\\')) return false;
    return true;
}

TArray<FString> FRetakeModMap::GetPresentationPackages() const
{
    TArray<FString> Packages;
    for (const FString* Path : { &LoadingArt, &Preview, &Minimap, &Ambience, &Acoustics, &VisibilityBake, &DefaultLoadoutPolicy })
        if (!Path->IsEmpty()) Packages.AddUnique(FPackageName::ObjectPathToPackageName(*Path));
    for (const auto& Pair : ModeLoadoutPolicies) Packages.AddUnique(FPackageName::ObjectPathToPackageName(Pair.Value));
    return Packages;
}

bool FRetakeModManifest::Validate(FString& OutError) const
{
    if (Format != TEXT("RetakeMap") && !IsWeaponPack())
        { OutError = TEXT("not a supported fresh map/weapon export; re-export the editable source"); return false; }
    if (SchemaVersion != RetakeMapSDK::ManifestSchemaVersion)
        { OutError = FString::Printf(TEXT("manifest schema %d is not %d"), SchemaVersion, RetakeMapSDK::ManifestSchemaVersion); return false; }
    if (!IsValidModId(ModId)) { OutError = TEXT("ModId must be RetakeMod_ followed by letters, digits or underscores (max 48)"); return false; }
    if (!IsValidRevision(Revision)) { OutError = TEXT("Revision must be 1-32 letters or digits"); return false; }
    if (!IsSafeDisplayText(DisplayName, 64) || !IsSafeDisplayText(Author, 64))
        { OutError = TEXT("DisplayName and Author must be at most 64 printable characters without quotes, backslashes, '?' or '='"); return false; }
    if (IsWeaponPack() ? !Maps.IsEmpty() || WeaponPack.IsEmpty() : Maps.IsEmpty() || !WeaponPack.IsEmpty())
        { OutError = TEXT("Map exports require maps only; weapon exports require an owned weapon catalogue and no maps."); return false; }
    if (IsWeaponPack() && SdkContractVersion < RetakeMapSDK::MinimumWeaponContract)
        { OutError = FString::Printf(TEXT("Weapon packs require SDK contract %d prepared-pose bindings; re-export their editable source."),RetakeMapSDK::MinimumWeaponContract); return false; }
    if (!RequiredWeaponPacks.IsEmpty() && SdkContractVersion < 10)
        { OutError = TEXT("Weapon dependency declarations require SDK contract 10 or later."); return false; }
    if (SdkContractVersion < RetakeMapSDK::OldestReadableContract || SdkContractVersion > RetakeMapSDK::ContractVersion)
        { OutError = TEXT("the mod SDK contract is outside this reader's supported range"); return false; }
    if (Artifacts.IsEmpty() || Compatibility.IsEmpty()) { OutError = TEXT("the mod needs artifacts and platform compatibility records"); return false; }
    TSet<FString> Platforms;
    for (const FRetakeModPlatformCompatibility& Cook : Compatibility)
    {
        if ((Cook.Platform != TEXT("Windows") && Cook.Platform != TEXT("LinuxServer")) || Platforms.Contains(Cook.Platform)
            || !FRetakeMapCompatibility::IsHash(Cook.EngineRecordId)
            || (Cook.MaterialPath != TEXT("SdkMaterials") && Cook.MaterialPath != TEXT("CustomShaders"))
            || Cook.StructuralProfile.IsEmpty() || Cook.NativeClasses.IsEmpty())
            { OutError = TEXT("missing, duplicate or malformed platform compatibility record"); return false; }
        Platforms.Add(Cook.Platform);
    }
    const FString Root = TEXT("/") + ModId + TEXT("/");
    if (IsWeaponPack() && !IsModObjectPath(WeaponPack, Root))
        { OutError = TEXT("Weapon catalogue must be an object path inside its owning mod."); return false; }
    if (RequiredWeaponPacks.Num() > 64 || (IsWeaponPack() && !RequiredWeaponPacks.IsEmpty()))
        { OutError = TEXT("Map plugins may require up to 64 independent weapon packs; nested weapon-pack dependencies are unsupported."); return false; }
    TSet<FString> RequiredIds;
    for (const auto& Requirement : RequiredWeaponPacks)
    {
        if (!Requirement.Validate(OutError)) return false;
        const FString Id = Requirement.PackId.ToLower();
        if (Id == ModId.ToLower() || RequiredIds.Contains(Id))
            { OutError = TEXT("Weapon-pack requirements must be unique and cannot refer to their owner."); return false; }
        RequiredIds.Add(Id);
    }
    TSet<FString> MapIds;
    for (const FRetakeModMap& Map : Maps)
    {
        if (!Map.MapId.StartsWith(ModId + TEXT(".")) || Map.MapId.Len() > 64 || MapIds.Contains(Map.MapId))
            { OutError = FString::Printf(TEXT("map id '%s' must be unique, start with '%s.' and be at most 64 characters"), *Map.MapId, *ModId); return false; }
        for (TCHAR C : Map.MapId)
            if (C > 127 || !(FChar::IsAlnum(C) || C == TEXT('_') || C == TEXT('.') || C == TEXT('-')))
                { OutError = FString::Printf(TEXT("map id '%s' has an invalid character"), *Map.MapId); return false; }
        MapIds.Add(Map.MapId);
        if (!Map.PackageName.StartsWith(Root, ESearchCase::CaseSensitive) || !FPackageName::IsValidTextForLongPackageName(Map.PackageName))
            { OutError = FString::Printf(TEXT("map package '%s' is outside %s"), *Map.PackageName, *Root); return false; }
        if (!IsValidDisplayName(Map.DisplayName)) { OutError = FString::Printf(TEXT("map '%s' needs a safe display name"), *Map.MapId); return false; }
        if (!IsValidDescription(Map.Description)) { OutError = FString::Printf(TEXT("map '%s' has an unsafe description"), *Map.MapId); return false; }
        if (Map.Acoustics.IsEmpty()) { OutError = TEXT("gameplay maps require their current acoustic bake path"); return false; }
        for (const FString* Path : { &Map.LoadingArt, &Map.Preview, &Map.Minimap, &Map.Ambience, &Map.Acoustics, &Map.VisibilityBake, &Map.DefaultLoadoutPolicy })
            if (!Path->IsEmpty() && !IsModObjectPath(*Path, Root))
                { OutError = FString::Printf(TEXT("map '%s' presentation path '%s' is not an asset inside %s"), *Map.MapId, **Path, *Root); return false; }
        if (Map.SupportedModes.IsEmpty()) { OutError = FString::Printf(TEXT("map '%s' lists no modes"), *Map.MapId); return false; }
        if (Map.ModeLoadoutPolicies.Num() > 32) { OutError = TEXT("Too many mode loadout policies."); return false; }
        if ((!Map.DefaultLoadoutPolicy.IsEmpty() || !Map.ModeLoadoutPolicies.IsEmpty()) && SdkContractVersion < 7)
        { OutError = TEXT("Loadout policies require SDK contract 7 or later."); return false; }
        for (const auto& Pair : Map.ModeLoadoutPolicies)
            if (!Map.SupportedModes.Contains(Pair.Key) || !IsModObjectPath(Pair.Value, Root))
            { OutError = TEXT("A mode loadout policy must belong to this mod and name a supported map mode."); return false; }
        for (const FString& Mode : Map.SupportedModes)
            if (!IsToken(Mode, 32, true)) { OutError = FString::Printf(TEXT("map '%s' has an invalid mode id '%s'"), *Map.MapId, *Mode); return false; }
        if (Map.Visibility != TEXT("Baked") && Map.Visibility != TEXT("Unbaked"))
            { OutError = FString::Printf(TEXT("map '%s' visibility must be Baked or Unbaked"), *Map.MapId); return false; }
        if ((Map.Visibility == TEXT("Baked")) != !Map.VisibilityBake.IsEmpty())
            { OutError = TEXT("Baked maps require their own visibility bake path; Unbaked maps must omit it"); return false; }
    }
    for (const FRetakeModArtifact& Artifact : Artifacts)
    {
        if (!Platforms.Contains(Artifact.Platform) || !IsSafeRelativePath(Artifact.RelativePath) || Artifact.Size <= 0 || !FRetakeMapCompatibility::IsHash(Artifact.Hash))
            { OutError = FString::Printf(TEXT("artifact '%s' is malformed"), *Artifact.RelativePath); return false; }
    }
    return true;
}

bool FRetakeModManifest::SaveToFile(const FString& Path, FString& OutError) const
{
    FString Json;
    if (!FJsonObjectConverter::UStructToJsonObjectString(*this, Json)) { OutError = TEXT("manifest could not be encoded"); return false; }
    if (!FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        { OutError = FString::Printf(TEXT("could not write %s"), *Path); return false; }
    return true;
}

bool FRetakeModManifest::LoadFromFile(const FString& Path, FRetakeModManifest& Out, FString& OutError)
{
    const int64 ManifestSize = IFileManager::Get().FileSize(*Path);
    if (ManifestSize <= 0 || ManifestSize > 16 * 1024 * 1024) { OutError = TEXT("manifest is missing, empty or exceeds 16 MiB"); return false; }
    FString Json;
    if (!FFileHelper::LoadFileToString(Json, *Path)) { OutError = FString::Printf(TEXT("could not read %s"), *Path); return false; }
    Out = FRetakeModManifest();
    TSharedPtr<FJsonObject> Object;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object) || !Object.IsValid())
        { OutError = TEXT("manifest is not a JSON object"); return false; }
    // One fresh reader. Validate required structural fields explicitly, while optional metadata keeps its declared defaults.
    double Schema = 0;
    if (!Object->HasTypedField<EJson::Number>(TEXT("schemaVersion")) || !Object->TryGetNumberField(TEXT("schemaVersion"), Schema) || Schema != RetakeMapSDK::ManifestSchemaVersion)
        { OutError = FString::Printf(TEXT("manifest schema %g; this game reads schema %d; re-export the mod"), Schema, RetakeMapSDK::ManifestSchemaVersion); return false; }
    for (const TCHAR* Key : { TEXT("format"), TEXT("modId"), TEXT("revision"), TEXT("sdkContractVersion"), TEXT("maps"), TEXT("artifacts"), TEXT("compatibility") })
        if (!Object->HasField(Key)) { OutError = FString(TEXT("manifest is missing required field ")) + Key; return false; }
    for (const TCHAR* Key : { TEXT("format"), TEXT("modId"), TEXT("revision") })
        if (Object->Values[Key]->Type != EJson::String) { OutError = FString(Key) + TEXT(" must be a string"); return false; }
    double ContractNumber = 0;
    if (Object->Values[TEXT("sdkContractVersion")]->Type != EJson::Number || !Object->TryGetNumberField(TEXT("sdkContractVersion"), ContractNumber)
        || ContractNumber < 0 || ContractNumber > MAX_int32 || FMath::FloorToDouble(ContractNumber) != ContractNumber)
        { OutError = TEXT("sdkContractVersion must be a nonnegative integer"); return false; }
    const auto RequiredObjects = [&OutError](const FJsonObject& Root, const TCHAR* Key, std::initializer_list<const TCHAR*> Fields, bool bAllowEmpty = false)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Root.TryGetArrayField(Key, Values) || (!bAllowEmpty && Values->IsEmpty())) { OutError = FString(TEXT("missing or invalid array ")) + Key; return false; }
        for (const auto& Value : *Values)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            if (!Value->TryGetObject(Entry)) { OutError = FString(TEXT("expected object in ")) + Key; return false; }
            for (const TCHAR* Field : Fields)
            {
                const TSharedPtr<FJsonValue>* Member = (*Entry)->Values.Find(Field);
                const FString Name(Field);
                const EJson Expected = Name == TEXT("size") ? EJson::Number : Name == TEXT("customVersions") ? EJson::Object
                    : (Name == TEXT("supportedModes") || Name == TEXT("foreignPackages") || Name == TEXT("nativeClasses") || Name == TEXT("structuralProfile")) ? EJson::Array : EJson::String;
                if (!Member || !Member->IsValid() || (*Member)->Type != Expected)
                    { OutError = FString(Key) + TEXT(" entry is missing or has the wrong type for ") + Field; return false; }
                if (Expected == EJson::Array)
                    for (const auto& Item : (*Member)->AsArray())
                        if (!Item.IsValid() || Item->Type != EJson::String)
                            { OutError = Name + TEXT(" must contain only strings"); return false; }
                if (Expected == EJson::Number)
                {
                    const double Number = (*Member)->AsNumber();
                    if (!FMath::IsFinite(Number) || Number <= 0 || Number > 9007199254740991.0 || FMath::FloorToDouble(Number) != Number)
                        { OutError = Name + TEXT(" must be a positive exactly representable integer"); return false; }
                }
                if (Name == TEXT("customVersions"))
                    for (const auto& Item : (*Member)->AsObject()->Values)
                    {
                        FGuid Guid;
                        double Number = -1;
                        if (!FGuid::ParseExact(Item.Key, EGuidFormats::DigitsWithHyphensLower, Guid)
                            || !Item.Value.IsValid() || Item.Value->Type != EJson::Number || !Item.Value->TryGetNumber(Number)
                            || !FMath::IsFinite(Number) || Number < 0 || Number > MAX_int32 || FMath::FloorToDouble(Number) != Number)
                            { OutError = TEXT("customVersions requires GUID keys and nonnegative integer versions"); return false; }
                    }
            }
        }
        return true;
    };
    const bool bWeapons = Object->GetStringField(TEXT("format")) == TEXT("RetakeWeapons");
    if (const auto* Catalogue = Object->Values.Find(TEXT("weaponPack")); Catalogue && (!Catalogue->IsValid() || (*Catalogue)->Type != EJson::String))
        { OutError = TEXT("weaponPack must be a string when present"); return false; }
    if (bWeapons && !Object->HasTypedField<EJson::String>(TEXT("weaponPack")))
        { OutError = TEXT("Weapon exports require weaponPack"); return false; }
    if (Object->HasField(TEXT("requiredWeaponPacks")) && !RequiredObjects(*Object, TEXT("requiredWeaponPacks"),
        {TEXT("packId"), TEXT("revision"), TEXT("contentDigest"), TEXT("profileId")}, true)) return false;
    if (!RequiredObjects(*Object, TEXT("maps"), { TEXT("mapId"), TEXT("packageName"), TEXT("displayName"), TEXT("supportedModes"), TEXT("visibility"), TEXT("acoustics") }, bWeapons)
        || !RequiredObjects(*Object, TEXT("artifacts"), { TEXT("platform"), TEXT("relativePath"), TEXT("size"), TEXT("hash") })
        || !RequiredObjects(*Object, TEXT("compatibility"), { TEXT("platform"), TEXT("engineRecordId"), TEXT("materialPath"), TEXT("foreignPackages"), TEXT("nativeClasses"), TEXT("structuralProfile"), TEXT("customVersions") })) return false;
    for (const auto& Value : Object->GetArrayField(TEXT("maps")))
    {
        const auto Map = Value->AsObject();
        if (const auto* Default = Map->Values.Find(TEXT("defaultLoadoutPolicy")); Default && (*Default)->Type != EJson::String)
        { OutError = TEXT("defaultLoadoutPolicy must be a string when present"); return false; }
        if (const auto* Modes = Map->Values.Find(TEXT("modeLoadoutPolicies")))
        {
            if ((*Modes)->Type != EJson::Object) { OutError = TEXT("modeLoadoutPolicies must be an object"); return false; }
            for (const auto& Pair : (*Modes)->AsObject()->Values)
                if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String)
                { OutError = TEXT("modeLoadoutPolicies values must be strings"); return false; }
        }
    }
    FText Reason;
    if (!FJsonObjectConverter::JsonObjectToUStruct(Object.ToSharedRef(), &Out, 0, 0, false, &Reason))
        { OutError = FString::Printf(TEXT("manifest is not valid: %s"), *Reason.ToString()); return false; }
    return true;
}

FString FRetakeModManifest::GetContentDigest() const
{
    TArray<FString> Lines;
    Lines.Add(FString::Printf(TEXT("%s|%d|%d|%s|%s"), *Format, SchemaVersion, SdkContractVersion, *ModId, *Revision));
    // No rows for absent additions: existing map revision tokens remain byte-for-byte unchanged.
    if (!WeaponPack.IsEmpty()) Lines.Add(TEXT("weapon-pack|") + WeaponPack);
    for (const auto& Requirement : RequiredWeaponPacks)
    {
        FString Row(TEXT("requires-weapon-pack|"));
        for (const FString* Value : {&Requirement.PackId, &Requirement.Revision, &Requirement.ContentDigest, &Requirement.ProfileId})
            Row += FString::FromInt(Value->Len()) + TEXT(":") + *Value;
        Lines.Add(MoveTemp(Row));
    }
    for (const FRetakeModMap& Map : Maps)
    {
        const TArray<FString> Fields = { Map.MapId, Map.PackageName, Map.DisplayName, Map.Description, Map.Visibility,
            FString::Join(Map.SupportedModes, TEXT(",")), Map.LoadingArt, Map.Preview, Map.Minimap, Map.Ambience, Map.Acoustics, Map.VisibilityBake };
        FString Encoded(TEXT("map|"));
        for (const FString& Field : Fields) Encoded += FString::FromInt(Field.Len()) + TEXT(":") + Field;
        Lines.Add(MoveTemp(Encoded));
        // No new digest rows for absent fields: immutable contract-6 exports keep their original revision token.
        if (!Map.DefaultLoadoutPolicy.IsEmpty()) Lines.Add(TEXT("loadout|") + Map.MapId + TEXT("|default|") + Map.DefaultLoadoutPolicy);
        for (const auto& Pair : Map.ModeLoadoutPolicies) Lines.Add(TEXT("loadout|") + Map.MapId + TEXT("|") + Pair.Key + TEXT("|") + Pair.Value);
    }
    for (const FRetakeModPlatformCompatibility& Cook : Compatibility)
    {
        Lines.Add(Cook.Platform + TEXT("|") + Cook.EngineRecordId + TEXT("|") + Cook.MaterialPath);
        for (const FString& Entry : Cook.StructuralProfile) Lines.Add(Cook.Platform + TEXT("|profile|") + Entry);
        for (const FString& Entry : Cook.ForeignPackages) Lines.Add(Cook.Platform + TEXT("|package|") + Entry);
        for (const FString& Entry : Cook.NativeClasses) Lines.Add(Cook.Platform + TEXT("|class|") + Entry);
        for (const auto& Entry : Cook.CustomVersions) Lines.Add(Cook.Platform + TEXT("|custom|") + Entry.Key + TEXT("=") + FString::FromInt(Entry.Value));
    }
    for (const FRetakeModArtifact& Artifact : Artifacts)
        Lines.Add(FString::Printf(TEXT("%s|%s|%lld|%s"), *Artifact.Platform, *Artifact.RelativePath, Artifact.Size, *Artifact.Hash));
    Lines.Sort();
    const FTCHARToUTF8 Utf8(*FString::Join(Lines, TEXT("\n")));
    return LexToString(FBlake3::HashBuffer(Utf8.Get(), Utf8.Length())).ToLower().Left(12);
}

FString FRetakeModManifest::HashFile(const FString& Path, int64& OutSize)
{
    OutSize = 0;
    TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path));
    if (!Reader) return FString();
    FBlake3 Hasher;
    TArray<uint8> Buffer;
    Buffer.SetNumUninitialized(4 * 1024 * 1024);
    const int64 Total = Reader->TotalSize();
    while (Reader->Tell() < Total)
    {
        const int64 Chunk = FMath::Min<int64>(Buffer.Num(), Total - Reader->Tell());
        Reader->Serialize(Buffer.GetData(), Chunk);
        if (Reader->IsError()) return FString();
        Hasher.Update(Buffer.GetData(), Chunk);
    }
    OutSize = Total;
    return LexToString(Hasher.Finalize()).ToLower();
}
