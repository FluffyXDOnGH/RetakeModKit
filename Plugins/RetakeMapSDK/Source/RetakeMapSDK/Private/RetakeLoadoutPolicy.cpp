// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeLoadoutPolicy.h"
#include "RetakeMapSDKIds.h"
#include "Hash/Blake3.h"
#include "Misc/DataValidation.h"
#if WITH_EDITOR
#include "RetakeMapSDKManifest.h"
#include "RetakeWeaponPack.h"
#include "RetakeWeaponSpecification.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "Interfaces/IPluginManager.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#endif

namespace
{
    void AppendId(FString& Out, FName Id)
    {
        const FString Value = Id.IsNone() ? FString() : Id.ToString().ToLower();
        Out += FString::Printf(TEXT("%d:%s;"), Value.Len(), *Value);
    }

    void AppendFaction(FString& Out, const FRetakeLoadoutFactionPolicy& Policy)
    {
        for (const auto Slot : {ERetakeLoadoutPolicySlot::Primary, ERetakeLoadoutPolicySlot::Secondary, ERetakeLoadoutPolicySlot::Grenade, ERetakeLoadoutPolicySlot::Equipment})
        {
            const auto& Rule = Policy.GetSlot(Slot);
            // Omitted equipment preserves existing policy fingerprints exactly.
            if (Slot==ERetakeLoadoutPolicySlot::Equipment && Rule.Rule==ERetakeLoadoutSlotRule::Inherit
                && Rule.DefaultWeapon.IsNone() && Rule.AllowedWeapons.IsEmpty() && !Rule.bAllowEmpty) continue;
            Out += FString::Printf(TEXT("slot=%d,rule=%d,empty=%d;"), int32(Slot), int32(Rule.Rule), Rule.bAllowEmpty);
            AppendId(Out, Rule.DefaultWeapon);
            // Order is significant: it is the creator's display/fallback order.
            Out += FString::Printf(TEXT("allowed=%d;"), Rule.AllowedWeapons.Num());
            for (FName Id : Rule.AllowedWeapons) AppendId(Out, Id);
        }
        if (!Policy.AmmoRepresentatives.IsEmpty())
        {
            TArray<FName> Pools;Policy.AmmoRepresentatives.GetKeys(Pools);Pools.Sort(FNameLexicalLess());
            Out+=TEXT("ammo-representatives-v1;");
            for (FName Pool:Pools) {AppendId(Out,Pool);AppendId(Out,Policy.AmmoRepresentatives.FindChecked(Pool));}
        }
        TArray<FRetakeLoadoutGrantPolicy> Grants = Policy.Grants;
        Grants.Sort([](const auto& A, const auto& B) { return A.WeaponId.LexicalLess(B.WeaponId); });
        Out += FString::Printf(TEXT("grants=%d;"), Grants.Num());
        for (const auto& Grant : Grants)
        {
            AppendId(Out, Grant.WeaponId);
            Out += FString::Printf(TEXT("%d,%d,%d;"), Grant.LoadedAmmo, Grant.ReserveAmmo, Grant.Quantity);
            if (Grant.SecondaryReserveAmmo!=-1) Out+=FString::Printf(TEXT("secondary-reserve=%d;"),Grant.SecondaryReserveAmmo);
        }
    }

    bool ValidateFaction(const FRetakeLoadoutFactionPolicy& Policy, FString& Error)
    {
        for (const auto Slot : {ERetakeLoadoutPolicySlot::Primary, ERetakeLoadoutPolicySlot::Secondary, ERetakeLoadoutPolicySlot::Grenade, ERetakeLoadoutPolicySlot::Equipment})
        {
            const auto& Rule = Policy.GetSlot(Slot);
            if (uint8(Rule.Rule) > uint8(ERetakeLoadoutSlotRule::Fixed)) { Error = TEXT("Unknown slot rule."); return false; }
            if (Rule.AllowedWeapons.Num() > 256) { Error = TEXT("A slot may name at most 256 weapons."); return false; }
            TSet<FName> Seen;
            for (FName Id : Rule.AllowedWeapons)
            {
                if (!FRetakeLoadoutPolicyDefinition::IsValidId(Id) || Seen.Contains(Id))
                { Error = TEXT("Allowed weapon IDs must be valid and unique."); return false; }
                Seen.Add(Id);
            }
            if (!Rule.DefaultWeapon.IsNone() && !FRetakeLoadoutPolicyDefinition::IsValidId(Rule.DefaultWeapon))
            { Error = TEXT("Invalid default weapon ID."); return false; }
            if (Rule.Rule == ERetakeLoadoutSlotRule::Inherit || Rule.Rule == ERetakeLoadoutSlotRule::Empty)
            {
                if (!Rule.DefaultWeapon.IsNone() || !Rule.AllowedWeapons.IsEmpty() || Rule.bAllowEmpty)
                { Error = TEXT("Inherit/Empty slots cannot also declare choices, a default or AllowEmpty."); return false; }
            }
            else if (Rule.Rule == ERetakeLoadoutSlotRule::Fixed)
            {
                if (Rule.DefaultWeapon.IsNone() || !Rule.AllowedWeapons.IsEmpty() || Rule.bAllowEmpty)
                { Error = TEXT("Fixed slots require a DefaultWeapon and no choice list; use Empty for an empty slot."); return false; }
            }
            else if (Rule.AllowedWeapons.IsEmpty() || (Rule.DefaultWeapon.IsNone() ? !Rule.bAllowEmpty : !Seen.Contains(Rule.DefaultWeapon)))
            { Error = TEXT("Choice slots require choices and a permitted default (or explicit AllowEmpty)."); return false; }
        }
        if (Policy.AmmoRepresentatives.Num()>64) {Error=TEXT("At most64 ammunition representatives are supported.");return false;}
        for (const auto& Pair:Policy.AmmoRepresentatives)
            if (!FRetakeLoadoutPolicyDefinition::IsValidId(Pair.Key) || !FRetakeLoadoutPolicyDefinition::IsValidId(Pair.Value))
            {Error=TEXT("Ammunition representatives require valid pool and weapon identities.");return false;}
        TSet<FName> Grants;
        for (const auto& Grant : Policy.Grants)
        {
            if (!FRetakeLoadoutPolicyDefinition::IsValidId(Grant.WeaponId) || Grants.Contains(Grant.WeaponId)
                || Grant.LoadedAmmo < -1 || Grant.ReserveAmmo < -1 || Grant.SecondaryReserveAmmo < -1 || Grant.Quantity < -1)
            { Error = TEXT("Grant IDs must be valid/unique and counts must be -1 (inherit) or nonnegative."); return false; }
            Grants.Add(Grant.WeaponId);
        }
        return true;
    }
}

const FRetakeLoadoutSlotPolicy& FRetakeLoadoutFactionPolicy::GetSlot(ERetakeLoadoutPolicySlot Slot) const
{
    switch (Slot)
    {
    case ERetakeLoadoutPolicySlot::Secondary: return Secondary;
    case ERetakeLoadoutPolicySlot::Grenade: return Grenade;
    case ERetakeLoadoutPolicySlot::Equipment: return Equipment;
    default: return Primary;
    }
}

FRetakeLoadoutSlotPolicy& FRetakeLoadoutFactionPolicy::GetSlot(ERetakeLoadoutPolicySlot Slot)
{
    return const_cast<FRetakeLoadoutSlotPolicy&>(static_cast<const FRetakeLoadoutFactionPolicy*>(this)->GetSlot(Slot));
}

bool FRetakeLoadoutPolicyDefinition::IsValidId(FName Id)
{
    if (Id.IsNone()) return false;
    return IsValidIdText(Id.ToString());
}

bool FRetakeLoadoutPolicyDefinition::IsValidIdText(const FString& Value)
{
    if (Value.IsEmpty() || Value.Len() > 128 || Value.Equals(TEXT("None"), ESearchCase::IgnoreCase)) return false;
    for (TCHAR C : Value)
        if (C > 127 || !(FChar::IsAlnum(C) || C == TEXT('_') || C == TEXT('-') || C == TEXT('.'))) return false;
    return true;
}

bool FRetakeLoadoutPolicyDefinition::Validate(FString& Error) const
{
    Error.Reset();
    if (!IsValidId(PolicyId)) { Error = TEXT("PolicyId must be a bounded stable identifier."); return false; }
    if (uint8(Distribution) > uint8(ERetakeLoadoutDistribution::MapPickups)
        || uint8(Restrictions) > uint8(ERetakeLoadoutRestriction::MatchWide))
    { Error = TEXT("Unknown distribution or restriction scope."); return false; }
    if (Factions.Num() > 16) { Error = TEXT("At most 16 faction overrides are supported."); return false; }
    if (!ValidateFaction(Shared, Error)) return false;
    for (const auto& Pair : Factions)
    {
        if (Pair.Key < 0 || Pair.Key >= 16) { Error = TEXT("Faction IDs must be in [0,15]."); return false; }
        if (!ValidateFaction(Pair.Value, Error)) return false;
    }
    return true;
}

FRetakeLoadoutFactionPolicy FRetakeLoadoutPolicyDefinition::ResolveFaction(int32 FactionId) const
{
    FRetakeLoadoutFactionPolicy Result = Shared;
    if (const auto* Override = Factions.Find(FactionId))
    {
        for (const auto Slot : {ERetakeLoadoutPolicySlot::Primary, ERetakeLoadoutPolicySlot::Secondary, ERetakeLoadoutPolicySlot::Grenade, ERetakeLoadoutPolicySlot::Equipment})
            if (Override->GetSlot(Slot).Rule != ERetakeLoadoutSlotRule::Inherit) Result.GetSlot(Slot) = Override->GetSlot(Slot);
        for (const auto& Pair:Override->AmmoRepresentatives) Result.AmmoRepresentatives.Add(Pair.Key,Pair.Value);
        for (const auto& Grant : Override->Grants)
        {
            if (auto* Existing = Result.Grants.FindByPredicate([&Grant](const auto& G) { return G.WeaponId == Grant.WeaponId; })) *Existing = Grant;
            else Result.Grants.Add(Grant);
        }
    }
    return Result;
}

FString FRetakeLoadoutPolicyDefinition::Fingerprint() const
{
    FString Canonical = TEXT("RetakeLoadoutPolicy:1;");
    AppendId(Canonical, PolicyId);
    Canonical += FString::Printf(TEXT("%d,%d,%d,%d,%d;"), int32(Distribution), int32(Restrictions),
        bAllowMapPickups, bAllowDroppedWeapons, bExplicitGrenadeGrants);
    AppendFaction(Canonical, Shared);
    TArray<int32> Keys;
    Factions.GetKeys(Keys);
    Keys.Sort();
    for (int32 Key : Keys)
    {
        Canonical += FString::Printf(TEXT("faction=%d;"), Key);
        AppendFaction(Canonical, Factions.FindChecked(Key));
    }
    FTCHARToUTF8 Utf8(*Canonical);
    return LexToString(FBlake3::HashBuffer(Utf8.Get(), Utf8.Length())).ToLower();
}

#if WITH_EDITOR
namespace
{
    struct FAuthoringWeapon
    {
        TSet<ERetakeLoadoutPolicySlot> Slots;
        TArray<FSoftObjectPath> Definitions;
    };

    bool ReadWeaponMetadata(TMap<FName, FAuthoringWeapon>& Out, FString& Error)
    {
        const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("RetakeMapSDK"));
        FString Json;
        if (!Plugin || !FFileHelper::LoadFileToString(Json, *FPaths::Combine(Plugin->GetBaseDir(), TEXT("Contract/RetakeWeaponCatalogue.json"))))
        { Error = TEXT("The staged SDK weapon catalogue is missing; refresh the kit from a supported SDK release."); return false; }
        TSharedPtr<FJsonObject> Root;
        const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        double Format = 0;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid()
            || !Root->TryGetNumberField(TEXT("format"), Format) || Format != 1
            || !Root->TryGetArrayField(TEXT("weapons"), Entries) || Entries->IsEmpty() || Entries->Num() > 4096)
        { Error = TEXT("Invalid staged weapon catalogue."); return false; }
        for (const auto& Entry : *Entries)
        {
            const TSharedPtr<FJsonObject>* Object = nullptr;
            FString Id, Slot;
            if (!Entry->TryGetObject(Object) || !(*Object)->TryGetStringField(TEXT("id"), Id) || !(*Object)->TryGetStringField(TEXT("slot"), Slot)
                || !FRetakeLoadoutPolicyDefinition::IsValidIdText(Id) || Out.Contains(FName(*Id)))
            { Error = TEXT("Invalid or duplicate staged weapon identity."); return false; }
            const auto Type = Slot == TEXT("Primary") ? ERetakeLoadoutPolicySlot::Primary : Slot == TEXT("Secondary")
                ? ERetakeLoadoutPolicySlot::Secondary : Slot == TEXT("Equipment") ? ERetakeLoadoutPolicySlot::Equipment : ERetakeLoadoutPolicySlot::Grenade;
            if (Slot != TEXT("Primary") && Slot != TEXT("Secondary") && Slot != TEXT("Grenade") && Slot != TEXT("Equipment"))
            { Error = TEXT("Unknown staged weapon slot."); return false; }
            Out.FindOrAdd(FName(*Id)).Slots.Add(Type);
        }

        // Only registered catalogue bindings from enabled content plugins are choices.
        // Orphan drafts, foreign assets and private game classes are never a provider.
        IAssetRegistry& Registry=FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        FARFilter Filter;
        Filter.ClassPaths.Add(URetakeWeaponPack::StaticClass()->GetClassPathName());
        TArray<FAssetData> Catalogues;
        Registry.GetAssets(Filter,Catalogues);
        Catalogues.Sort([](const FAssetData& A,const FAssetData& B){return A.PackageName.LexicalLess(B.PackageName);});
        const auto Providers=IPluginManager::Get().GetEnabledPlugins();
        TSet<FName> Owners;
        for (const FAssetData& Asset:Catalogues)
        {
            FString Owner,Relative;
            if (!Asset.PackageName.ToString().RightChop(1).Split(TEXT("/"),&Owner,&Relative)
                || !FRetakeModManifest::IsValidModId(Owner)) continue;
            const auto* Provider=Providers.FindByPredicate([&Owner](const auto& Value)
                {return Value->GetName().Equals(Owner,ESearchCase::IgnoreCase);});
            if (!Provider || !(*Provider)->CanContainContent()) continue;
            Owner=(*Provider)->GetName();
            if ((*Provider)->GetMountedAssetPath()!=TEXT("/")+Owner+TEXT("/")) continue;
            if (Owners.Contains(FName(*Owner)))
            { Error=TEXT("Enabled weapon workspace has multiple catalogues: ")+Owner;return false; }
            Owners.Add(FName(*Owner));
            const auto* Catalogue=Cast<URetakeWeaponPack>(Asset.GetAsset());
            if (!Catalogue || Catalogue->PackId!=Owner || !Catalogue->Validate(Error))
            { Error=TEXT("Invalid enabled weapon catalogue ")+Asset.GetObjectPathString()+TEXT(": ")+Error;return false; }
            for (const auto& Profile:Catalogue->Profiles) for (const auto& Binding:Profile.Weapons)
            {
                const FAssetData DefinitionAsset=Registry.GetAssetByObjectPath(Binding.Value);
                if (DefinitionAsset.AssetClassPath!=URetakeWeaponSpecification::StaticClass()->GetClassPathName())
                { Error=TEXT("Loadout choice requires an owned native weapon specification: ")+Binding.Value.ToString();return false; }
                const auto* Definition=Cast<URetakeWeaponSpecification>(DefinitionAsset.GetAsset());
                const FName Id(*(Owner+TEXT(".")+Binding.Key));
                if (!Definition || Definition->WeaponId!=Id || uint8(Definition->Slot)>uint8(ERetakeWeaponSlot::Equipment))
                { Error=TEXT("Invalid loadout choice identity or slot: ")+Binding.Value.ToString();return false; }
                auto& Metadata=Out.FindOrAdd(Id);
                Metadata.Slots.Add(static_cast<ERetakeLoadoutPolicySlot>(Definition->Slot));
                Metadata.Definitions.AddUnique(Binding.Value);
                if (Out.Num()>4096)
                { Error=TEXT("Too many enabled authoring weapons; disable unused weapon workspaces.");return false; }
            }
        }
        return true;
    }

    bool ValidateRepresentative(const FAuthoringWeapon& Weapon,FName Pool,FString& Error)
    {
        // Builtin metadata deliberately contains no private resource paths. Its pool
        // binding is verified by game admission; mod resources can be checked here.
        if (Weapon.Definitions.IsEmpty()) return true;
        for (const auto& Path:Weapon.Definitions)
        {
            const auto* Definition=Cast<URetakeWeaponSpecification>(Path.ResolveObject());
            if (!Definition) {Error=TEXT("Authoring weapon is no longer loaded: ")+Path.ToString();return false;}
            for (const auto& Reference:{Definition->Ammo,Definition->SecondaryAmmo})
                if (const auto* Ammo=Reference.LoadSynchronous();Ammo && Ammo->AmmoId==Pool) return true;
        }
        Error=TEXT("No enabled profile of the ammunition representative uses pool: ")+Pool.ToString();return false;
    }
}

bool URetakeLoadoutPolicy::ValidateAuthoringWeapons(FString& Error) const
{
    if (!Definition.Validate(Error)) return false;
    TMap<FName, FAuthoringWeapon> Weapons;
    if (!ReadWeaponMetadata(Weapons, Error)) return false;
    TArray<int32> Factions;
    Definition.Factions.GetKeys(Factions); Factions.Add(INDEX_NONE);
    for (int32 Faction : Factions)
    {
        const auto Policy = Definition.ResolveFaction(Faction);
        for (const auto Slot : {ERetakeLoadoutPolicySlot::Primary, ERetakeLoadoutPolicySlot::Secondary, ERetakeLoadoutPolicySlot::Grenade, ERetakeLoadoutPolicySlot::Equipment})
        {
            const auto& Rule = Policy.GetSlot(Slot);
            TArray<FName> Ids = Rule.AllowedWeapons;
            if (!Rule.DefaultWeapon.IsNone()) Ids.AddUnique(Rule.DefaultWeapon);
            for (FName Id : Ids)
                if (const auto* Metadata = Weapons.Find(Id); !Metadata || !Metadata->Slots.Contains(Slot))
                { Error = TEXT("Unknown or wrong-slot weapon; stage the SDK or enable its weapon workspace: ") + Id.ToString(); return false; }
        }
        for (const auto& Grant : Policy.Grants)
            if (!Weapons.Contains(Grant.WeaponId)) { Error = TEXT("Grant names an unknown authoring weapon: ") + Grant.WeaponId.ToString(); return false; }
        for (const auto& Pair:Policy.AmmoRepresentatives)
        {
            const auto* Metadata=Weapons.Find(Pair.Value);
            if (!Metadata) {Error=TEXT("Unknown ammunition representative: ")+Pair.Value.ToString();return false;}
            if (!ValidateRepresentative(*Metadata,Pair.Key,Error)) return false;
        }
    }
    return true;
}

EDataValidationResult URetakeLoadoutPolicy::IsDataValid(FDataValidationContext& Context) const
{
    const EDataValidationResult Parent = Super::IsDataValid(Context);
    FString Error;
    if (!ValidateAuthoringWeapons(Error)) { Context.AddError(FText::FromString(Error)); return EDataValidationResult::Invalid; }
    return Parent == EDataValidationResult::Invalid ? Parent : EDataValidationResult::Valid;
}
#endif

TArray<FName> URetakeLoadoutPolicy::GetWeaponIds()
{
#if WITH_EDITOR
    TMap<FName, FAuthoringWeapon> Weapons;
    FString Error;
    if (ReadWeaponMetadata(Weapons, Error))
    {
        TArray<FName> Result; Weapons.GetKeys(Result);
        Result.Sort(FNameLexicalLess());
        return Result;
    }
#endif
    return RetakeMapSDKIds::Weapons();
}
