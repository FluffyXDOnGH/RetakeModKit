// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

class URetakeAudioCueDefinition;
class URetakeAudioLibrary;

/** One immutable content provider. CueId and SemanticName are local to Owner. No recording loads occur here. */
struct RETAKEMAPSDK_API FRetakeAudioCueProvider
{
    FString Owner;
    FString ContentIdentity;
    TArray<const URetakeAudioCueDefinition*> Cues;
};

/** Borrowed metadata. Its owner must retain the source cue UObjects for the entire installed catalogue lifetime. */
struct RETAKEMAPSDK_API FRetakeAudioCatalogue
{
    TMap<uint16,const URetakeAudioCueDefinition*> Cues;
    TMap<const URetakeAudioCueDefinition*,uint16> Ids;
    FString Identity;
    uint32 Revision=0;
    bool bTemporaryContent=false;
    const URetakeAudioCueDefinition* Find(uint16 Id) const { const auto* Found=Cues.Find(Id); return Found ? *Found:nullptr; }
    uint16 FindId(const URetakeAudioCueDefinition* Cue) const { const auto* Found=Ids.Find(Cue); return Found ? *Found:0; }
};

namespace RetakeAudioCatalogues
{
    /** Deterministic provider-local -> session mapping. Base IDs remain fixed; failure leaves Out unchanged. */
    RETAKEMAPSDK_API bool Build(const URetakeAudioLibrary& Base,TConstArrayView<FRetakeAudioCueProvider> Providers,
        FRetakeAudioCatalogue& Out,FString& Error);
}
