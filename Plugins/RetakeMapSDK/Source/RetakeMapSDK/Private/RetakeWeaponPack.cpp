// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeWeaponPack.h"
#include "RetakeMapSDKManifest.h"
#include "Hash/Blake3.h"
#include "Misc/DataValidation.h"
#include "Misc/PackageName.h"

namespace
{
    constexpr int32 MaxWeapons = 512;
    constexpr int32 MaxProfiles = 16;
    constexpr int32 MaxPacks = 64;

    void Append(FString& Text, const FString& Value)
    {
        // Length-prefix prevents a display name or future delimiter from changing the record boundary.
        Text += FString::Printf(TEXT("%d:"), Value.Len());
        Text += Value;
    }

    FString Hash(const FString& Text)
    {
        const FTCHARToUTF8 Bytes(*Text);
        return LexToString(FBlake3::HashBuffer(Bytes.Get(), Bytes.Length())).ToLower();
    }

    bool SameRequirement(const FRetakeWeaponPackRequirement& A, const FRetakeWeaponPackRequirement& B)
    {
        return A.PackId == B.PackId && A.Revision == B.Revision && A.ContentDigest == B.ContentDigest
            && A.ProfileId == B.ProfileId;
    }

    FString SelectionText(const FRetakeWeaponPackRequirement& Value)
    {
        FString Result;
        for (const FString* Part : {&Value.PackId, &Value.Revision, &Value.ContentDigest, &Value.ProfileId}) Append(Result, *Part);
        return Result;
    }
}

bool URetakeWeaponPack::IsValidLocalId(const FString& Id)
{
    if (Id.IsEmpty() || Id.Len() > 64 || Id.Equals(TEXT("None"), ESearchCase::IgnoreCase)) return false;
    for (TCHAR C : Id)
        if (!((C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('A') && C <= TEXT('Z'))
            || (C >= TEXT('0') && C <= TEXT('9')) || C == TEXT('_') || C == TEXT('-'))) return false;
    return true;
}

bool URetakeWeaponPack::IsOwnedObjectPath(const FString& Owner, const FSoftObjectPath& Path)
{
    const FString Text = Path.ToString();
    const FString Root = TEXT("/") + Owner + TEXT("/");
    int32 Dot = INDEX_NONE;
    if (Text.Len() > 1024 || Text.Contains(TEXT(":")) || Text.Contains(TEXT("\\"))
        || !Text.FindChar(TEXT('.'), Dot)) return false;
    const FString Package = Text.Left(Dot);
    // Soft paths contain FNames: display case can change between editor and cooked processes.
    // Pack IDs themselves are checked for case-only ownership collisions before session resolution.
    if (!Package.StartsWith(Root, ESearchCase::IgnoreCase) || !FPackageName::IsValidTextForLongPackageName(Package)) return false;
    TArray<FString> Parts;
    Package.Mid(Root.Len()).ParseIntoArray(Parts, TEXT("/"), false);
    for (const FString& Part : Parts)
        if (Part.IsEmpty() || Part == TEXT(".") || Part == TEXT("..")) return false;
    return Text.Equals(Package + TEXT(".") + FPackageName::GetShortName(Package), ESearchCase::IgnoreCase);
}

const FRetakeWeaponPackProfile* URetakeWeaponPack::FindProfile(const FString& Id) const
{
    return Profiles.FindByPredicate([&](const auto& Profile) { return Profile.ProfileId == Id; });
}

bool URetakeWeaponPack::Validate(FString& Error) const
{
    Error.Reset();
    if (!FRetakeModManifest::IsValidModId(PackId) || !FRetakeModManifest::IsValidDisplayName(DisplayName))
    { Error = TEXT("Weapon pack needs a valid owning mod ID and display name."); return false; }
    if (Weapons.IsEmpty() || Weapons.Num() > MaxWeapons || Profiles.IsEmpty() || Profiles.Num() > MaxProfiles)
    { Error = TEXT("Weapon pack requires 1-512 weapons and 1-16 explicit profiles."); return false; }
    TSet<FString> WeaponIds, ProfileIds, UsedWeapons;
    for (const auto& Weapon : Weapons)
    {
        if (!IsValidLocalId(Weapon.LocalId) || !FRetakeModManifest::IsValidDisplayName(Weapon.DisplayName)
            || WeaponIds.Contains(Weapon.LocalId.ToLower()))
        { Error = TEXT("Invalid or duplicate weapon identity: ") + Weapon.LocalId; return false; }
        WeaponIds.Add(Weapon.LocalId.ToLower());
    }
    for (const auto& Profile : Profiles)
    {
        if (!IsValidLocalId(Profile.ProfileId) || !FRetakeModManifest::IsValidDisplayName(Profile.DisplayName)
            || ProfileIds.Contains(Profile.ProfileId.ToLower()) || Profile.Weapons.IsEmpty() || Profile.Weapons.Num() > MaxWeapons)
        { Error = TEXT("Invalid, empty or duplicate weapon profile: ") + Profile.ProfileId; return false; }
        ProfileIds.Add(Profile.ProfileId.ToLower());
        if (!IsOwnedObjectPath(PackId,Profile.PoseContribution))
        { Error=TEXT("Profile requires an owned prepared pose contribution: ")+Profile.ProfileId;return false; }
        TSet<FString> Bindings;
        for (const auto& Binding : Profile.Weapons)
        {
            const FString Key = Binding.Key.ToLower();
            // Require the declared spelling as well: FName's case-insensitive identity must never hide an export typo.
            if (!IsValidLocalId(Binding.Key) || Bindings.Contains(Key)
                || !Weapons.ContainsByPredicate([&](const auto& Weapon) { return Weapon.LocalId == Binding.Key; })
                || !IsOwnedObjectPath(PackId, Binding.Value))
            { Error = TEXT("Profile has an unknown, colliding or foreign definition binding: ") + Profile.ProfileId + TEXT("/") + Binding.Key; return false; }
            Bindings.Add(Key);
            UsedWeapons.Add(Key);
        }
    }
    if (UsedWeapons.Num() != WeaponIds.Num())
    { Error = TEXT("Every declared weapon must belong to at least one profile."); return false; }
    return true;
}

FString URetakeWeaponPack::GetCatalogueIdentity() const
{
    FString Error;
    if (!Validate(Error)) return {};
    FString Text = TEXT("RetakeWeaponPackCatalogue:2;");
    Append(Text, PackId); Append(Text, DisplayName);
    TArray<FString> Rows;
    for (const auto& Weapon : Weapons)
    {
        FString Row = TEXT("weapon;");
        Append(Row, Weapon.LocalId); Append(Row, Weapon.DisplayName); Rows.Add(MoveTemp(Row));
    }
    for (const auto& Profile : Profiles)
    {
        FString Row = TEXT("profile;");
        Append(Row, Profile.ProfileId); Append(Row, Profile.DisplayName); Append(Row,Profile.PoseContribution.ToString().ToLower()); Rows.Add(MoveTemp(Row));
        for (const auto& Binding : Profile.Weapons)
        {
            Row = TEXT("binding;");
            Append(Row, Profile.ProfileId); Append(Row, Binding.Key); Append(Row, Binding.Value.ToString().ToLower()); Rows.Add(MoveTemp(Row));
        }
    }
    Rows.Sort();
    for (const FString& Row : Rows) Append(Text, Row);
    return Hash(Text);
}

bool FRetakeWeaponPackRequirement::Validate(FString& Error) const
{
    Error.Reset();
    if (!FRetakeModManifest::IsValidModId(PackId) || !FRetakeModManifest::IsValidRevision(Revision)
        || !URetakeWeaponPack::IsValidLocalId(ProfileId) || ContentDigest.Len() != 12)
    { Error = TEXT("Weapon dependency requires an exact pack, revision, content digest and profile."); return false; }
    for (TCHAR C : ContentDigest)
        if (!((C >= TEXT('0') && C <= TEXT('9')) || (C >= TEXT('a') && C <= TEXT('f'))))
        { Error = TEXT("Weapon dependency content digest must be 12 lower-case hexadecimal digits."); return false; }
    return true;
}

bool RetakeWeaponPacks::Resolve(TConstArrayView<FRetakeSelectedWeaponPack> Selected,
    TConstArrayView<FRetakeWeaponPackRequirement> Required, const TSet<FName>& BuiltinWeaponIds,
    FRetakeWeaponPackSession& Out, FString& Error)
{
    Out = {}; Error.Reset();
    if (Selected.Num() > MaxPacks || Required.Num() > 256)
    { Error = TEXT("Too many weapon pack selections or requirements."); return false; }
    TMap<FString, const FRetakeSelectedWeaponPack*> Packs;
    for (const auto& Pack : Selected)
    {
        if (!Pack.Catalogue || !Pack.Selection.Validate(Error) || !Pack.Catalogue->Validate(Error))
        { if (Error.IsEmpty()) Error = TEXT("Missing weapon pack catalogue."); return false; }
        if (Pack.Selection.PackId != Pack.Catalogue->PackId || Packs.Contains(Pack.Selection.PackId.ToLower()))
        { Error = TEXT("Only one revision/profile may be selected for weapon pack: ") + Pack.Selection.PackId; return false; }
        if (!Pack.Catalogue->FindProfile(Pack.Selection.ProfileId))
        { Error = TEXT("Unknown weapon profile: ") + Pack.Selection.PackId + TEXT("/") + Pack.Selection.ProfileId; return false; }
        Packs.Add(Pack.Selection.PackId.ToLower(), &Pack);
    }
    TMap<FString, const FRetakeWeaponPackRequirement*> Requirements;
    for (const auto& Requirement : Required)
    {
        if (!Requirement.Validate(Error)) return false;
        const FString Key = Requirement.PackId.ToLower();
        if (const auto* Previous = Requirements.Find(Key); Previous && !SameRequirement(**Previous, Requirement))
        { Error = TEXT("Conflicting exact weapon pack requirements: ") + Requirement.PackId; return false; }
        Requirements.Add(Key, &Requirement);
        const auto* Pack = Packs.Find(Key);
        if (!Pack || !SameRequirement((*Pack)->Selection, Requirement))
        { Error = TEXT("Required weapon pack revision/profile is not selected: ") + Requirement.PackId + TEXT("/") + Requirement.Revision + TEXT("/") + Requirement.ProfileId; return false; }
    }
    FRetakeWeaponPackSession Result;
    TSet<FName> WeaponIds = BuiltinWeaponIds;
    TArray<FString> Rows;
    for (const auto& Pack : Selected)
    {
        FString Row = SelectionText(Pack.Selection);
        Append(Row, Pack.Catalogue->GetCatalogueIdentity()); Rows.Add(MoveTemp(Row));
        const auto* Profile = Pack.Catalogue->FindProfile(Pack.Selection.ProfileId);
        for (const auto& Binding : Profile->Weapons)
        {
            const FString Id = Pack.Selection.PackId + TEXT(".") + Binding.Key;
            const FName Name(*Id);
            if (WeaponIds.Contains(Name))
            { Error = TEXT("Weapon identity collision, including builtin/case-only aliases: ") + Id; return false; }
            WeaponIds.Add(Name);
            const auto* Weapon = Pack.Catalogue->Weapons.FindByPredicate([&](const auto& Entry) { return Entry.LocalId == Binding.Key; });
            Result.Weapons.Add({Id, Weapon->DisplayName, Binding.Value});
        }
    }
    Result.Weapons.Sort([](const auto& A, const auto& B) { return A.WeaponId < B.WeaponId; });
    Rows.Sort();
    FString Text = TEXT("RetakeWeaponPackSession:1;");
    for (const FString& Row : Rows) Append(Text, Row);
    Result.Identity = Hash(Text);
    Out = MoveTemp(Result);
    return true;
}

#if WITH_EDITOR
EDataValidationResult URetakeWeaponPack::IsDataValid(FDataValidationContext& Context) const
{
    FString Error;
    if (!Validate(Error)) { Context.AddError(FText::FromString(Error)); return EDataValidationResult::Invalid; }
    return Super::IsDataValid(Context) == EDataValidationResult::Invalid ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}
#endif
