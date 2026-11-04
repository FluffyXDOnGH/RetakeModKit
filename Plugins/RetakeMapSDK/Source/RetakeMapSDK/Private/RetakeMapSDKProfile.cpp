// Copyright 2026 Codnyx. All Rights Reserved.

#include "RetakeMapSDKProfile.h"
#include "CoreGlobals.h"
#include "Hash/Blake3.h"
#include "HAL/PlatformProperties.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"

namespace
{
    struct FProfileSection
    {
        const TCHAR* Name;
        /** Empty: every key of the section. */
        TArray<const TCHAR*> Keys;
        /** Empty: every platform. */
        const TCHAR* Platform = nullptr;
    };

    /** Settings that change cooked map data or the shaders compiled for it. Runtime-only tuning stays out. */
    const TArray<FProfileSection>& ProfileSections()
    {
        static const TArray<FProfileSection> Sections = {
            { TEXT("/Script/Engine.CollisionProfile"), {} },
            { TEXT("/Script/Engine.PhysicsSettings"), { TEXT("PhysicalSurfaces"), TEXT("bSuppressFaceRemapTable"), TEXT("bSupportUVFromHitResults"), TEXT("DefaultShapeComplexity") } },
            { TEXT("/Script/Engine.RendererSettings"), {} },
            { TEXT("/Script/WindowsTargetPlatform.WindowsTargetSettings"), { TEXT("D3D12TargetedShaderFormats"), TEXT("D3D11TargetedShaderFormats") }, TEXT("Windows") },
            { TEXT("/Script/LinuxTargetPlatform.LinuxTargetSettings"), { TEXT("TargetedRHIs") }, TEXT("Linux") },
        };
        return Sections;
    }
}

FString FRetakeMapSDKProfile::BuildCanonicalText(FConfigCacheIni& Config, const FString& IniPlatformName)
{
    TArray<FString> Lines;
    for (const FProfileSection& Wanted : ProfileSections())
    {
        if (Wanted.Platform && IniPlatformName != Wanted.Platform) continue;
        const FConfigSection* Section = Config.GetSection(Wanted.Name, false, GEngineIni);
        if (!Section)
        {
            Lines.Add(FString::Printf(TEXT("%s|<missing>"), Wanted.Name));
            continue;
        }
        for (const auto& Pair : *Section)
        {
            const FString Key = Pair.Key.ToString();
            if (!Wanted.Keys.IsEmpty() && !Wanted.Keys.ContainsByPredicate([&Key](const TCHAR* K) { return Key.Equals(K, ESearchCase::IgnoreCase); }))
                continue;
            FString Value = Pair.Value.GetValue();
            Value.TrimStartAndEndInline();
            // Array entries are self-describing (channel, surface and profile entries carry their own IDs), so value
            // order is not part of the identity and differing merge orders cannot produce a false mismatch.
            Lines.Add(FString::Printf(TEXT("%s|%s=%s"), Wanted.Name, *Key, *Value));
        }
    }
    Lines.Sort();
    const IConsoleVariable* RegisteredSubobjects = IConsoleManager::Get().FindConsoleVariable(TEXT("net.SubObjects.DefaultUseSubObjectReplicationList"));
    Lines.Add(FString::Printf(TEXT("ConstructorDefaults|net.SubObjects.DefaultUseSubObjectReplicationList=%d"), RegisteredSubobjects ? RegisteredSubobjects->GetInt() : -1));
    return FString::Join(Lines, TEXT("\n"));
}

FString FRetakeMapSDKProfile::ComputeHash(FConfigCacheIni& Config, const FString& IniPlatformName)
{
    const FTCHARToUTF8 Utf8(*BuildCanonicalText(Config, IniPlatformName));
    return LexToString(FBlake3::HashBuffer(Utf8.Get(), Utf8.Length())).ToLower();
}

FString FRetakeMapSDKProfile::GetRuntimeIniPlatformName()
{
    return ANSI_TO_TCHAR(FPlatformProperties::IniPlatformName());
}

FString FRetakeMapSDKProfile::ComputeRuntimeHash()
{
    return GConfig ? ComputeHash(*GConfig, GetRuntimeIniPlatformName()) : FString();
}

TArray<FString> FRetakeMapSDKProfile::StructuralEntries(FConfigCacheIni& Config)
{
    TArray<FString> Lines;
    for (const TCHAR* Name : { TEXT("/Script/Engine.CollisionProfile"), TEXT("/Script/Engine.PhysicsSettings") })
    {
        const FConfigSection* Section = Config.GetSection(Name, false, GEngineIni);
        if (!Section) continue;
        for (const auto& Pair : *Section)
        {
            const FString Key = Pair.Key.ToString();
            if (FCString::Strcmp(Name, TEXT("/Script/Engine.PhysicsSettings")) == 0 && Key != TEXT("PhysicalSurfaces")) continue;
            Lines.Add(FString(Name) + TEXT("|") + Key + TEXT("=") + Pair.Value.GetValue().TrimStartAndEnd());
        }
    }
    Lines.Sort();
    const IConsoleVariable* RegisteredSubobjects = IConsoleManager::Get().FindConsoleVariable(TEXT("net.SubObjects.DefaultUseSubObjectReplicationList"));
    Lines.Add(FString::Printf(TEXT("ConstructorDefaults|net.SubObjects.DefaultUseSubObjectReplicationList=%d"), RegisteredSubobjects ? RegisteredSubobjects->GetInt() : -1));
    return Lines;
}
