// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeAudioCatalogue.h"
#include "RetakeAudioCueDefinition.h"
#include "RetakeMapSDKManifest.h"
#include "RetakeMapSDKVersion.h"
#include "Hash/Blake3.h"
#include "Serialization/MemoryWriter.h"
#include "UObject/Package.h"

bool RetakeAudioCatalogues::Build(const URetakeAudioLibrary& Base,TConstArrayView<FRetakeAudioCueProvider> Providers,
    FRetakeAudioCatalogue& Out,FString& Error)
{
    Error.Reset();
    if (!Base.Validate(Error)) return false;
    if (Providers.Num()>64) { Error=TEXT("Audio preparation supports at most 64 content providers."); return false; }
    FRetakeAudioCatalogue Candidate;
    struct FBinding { FString Owner,Content; const URetakeAudioCueDefinition* Cue; uint16 Id; };
    TArray<FBinding> Bindings;
    for (const URetakeAudioCueDefinition* Cue:Base.Cues)
    {
        const uint16 Id=uint16(Cue->CueId);
        Candidate.Cues.Add(Id,Cue); Candidate.Ids.Add(Cue,Id);
        Bindings.Add({TEXT("builtin"),FString::FromInt(Base.Revision),Cue,Id});
    }
    TArray<const FRetakeAudioCueProvider*> Sorted;
    for (const auto& Provider:Providers) Sorted.Add(&Provider);
    Sorted.Sort([](const auto& A,const auto& B){return A.Owner.ToLower()<B.Owner.ToLower();});
    TSet<FString> Owners;
    uint32 NextId=1;
    const auto Allowed=[&](const FString& Owner,const FSoftObjectPath& Path)
    {
        if (Path.IsNull()) return true;
        const FString Package=Path.GetLongPackageName();
        if (Package.StartsWith(TEXT("/")+Owner+TEXT("/"),ESearchCase::IgnoreCase)) return true;
        for (const TCHAR* Root:RetakeMapSDK::AllowedForeignRoots)
            if (Package.StartsWith(Root,ESearchCase::IgnoreCase)) return true;
        return false;
    };
    for (const auto* Provider:Sorted)
    {
        if (!FRetakeModManifest::IsValidModId(Provider->Owner) || Provider->ContentIdentity.Len()!=64
            || Owners.Contains(Provider->Owner.ToLower()))
        { Error=TEXT("Audio provider has an invalid/duplicate owner or content identity."); return false; }
        for (TCHAR C:Provider->ContentIdentity) if (!FChar::IsHexDigit(C))
        { Error=TEXT("Audio provider content identity must be a full hexadecimal digest."); return false; }
        Owners.Add(Provider->Owner.ToLower());
        TArray<const URetakeAudioCueDefinition*> Cues=Provider->Cues;
        if (Cues.Num()>MAX_uint16-Candidate.Cues.Num())
        { Error=TEXT("Selected audio providers exceed 65535 session cues."); return false; }
        for (const auto* Cue:Cues)
            if (!Cue || Cue->GetClass()!=URetakeAudioCueDefinition::StaticClass() || !Cue->Validate(Error))
            { if (Error.IsEmpty()) Error=TEXT("Audio provider requires supported native cue definitions."); return false; }
        Cues.Sort([](const auto& A,const auto& B){return A.CueId<B.CueId;});
        TSet<int32> LocalIds; TSet<FName> Names;
        for (const auto* Cue:Cues)
        {
            if (LocalIds.Contains(Cue->CueId) || Names.Contains(Cue->SemanticName) || Candidate.Ids.Contains(Cue)
                || !Cue->GetOutermost()->GetName().StartsWith(TEXT("/")+Provider->Owner+TEXT("/"),ESearchCase::IgnoreCase))
            { Error=TEXT("Audio provider cue ownership or local identity conflicts: ")+Cue->GetPathName(); return false; }
            if (!Allowed(Provider->Owner,Cue->Generator.ToSoftObjectPath()))
            { Error=TEXT("Mod audio generator references private content: ")+Cue->GetPathName(); return false; }
            for (const auto& Recording:Cue->Recordings)
                if (!Allowed(Provider->Owner,Recording.Wave.ToSoftObjectPath()))
                { Error=TEXT("Mod audio recording references private content: ")+Cue->GetPathName(); return false; }
            while (NextId<=MAX_uint16 && Candidate.Cues.Contains(uint16(NextId))) ++NextId;
            if (NextId>MAX_uint16) { Error=TEXT("Selected audio providers exceed 65535 session cues."); return false; }
            const uint16 Id=uint16(NextId++);
            Candidate.Cues.Add(Id,Cue); Candidate.Ids.Add(Cue,Id);
            Bindings.Add({Provider->Owner,Provider->ContentIdentity,Cue,Id});
            LocalIds.Add(Cue->CueId); Names.Add(Cue->SemanticName);
        }
    }
    Bindings.Sort([](const auto& A,const auto& B){return A.Id<B.Id;});
    TArray<uint8> Bytes; FMemoryWriter Writer(Bytes,true);
    const auto Name=[&](FString Value){Value=Value.ToLower();Writer<<Value;};
    uint32 Format=2; Writer<<Format; // Recording-specific linear gain/pitch ranges participate in session identity.
    int32 Revision=Base.Revision; Writer<<Revision;
    int32 ProviderCount=Sorted.Num(),BindingCount=Bindings.Num();Writer<<ProviderCount<<BindingCount;
    // Include empty providers too: changing an enabled revision must not preserve a session identity accidentally.
    for (const auto* Provider:Sorted) { Name(Provider->Owner); Name(Provider->ContentIdentity); }
    for (const auto& Binding:Bindings)
    {
        const auto& Cue=*Binding.Cue;
        Name(Binding.Owner); Name(Binding.Content); Name(Cue.GetPathName()); Name(Cue.SemanticName.ToString());
        uint16 Id=Binding.Id,Local=uint16(Cue.CueId);Writer<<Id<<Local;
        uint8 Category=uint8(Cue.Category),Loading=uint8(Cue.Loading),Flags=(Cue.bSpatial?1:0)|(Cue.bLooping?2:0);
        Writer<<Category<<Loading<<Flags;
        auto Propagation=Cue.Propagation;
        Writer<<Propagation.RangeCm<<Propagation.FullGainRadiusCm<<Propagation.FalloffExponent<<Propagation.ReverbSend;
        float Priority=Cue.Priority,Gain=Cue.GainDb,Pitch=Cue.PitchVariationSemitones; int32 Maximum=Cue.MaximumInstances;
        Writer<<Priority<<Gain<<Pitch<<Maximum; Name(Cue.Generator.ToSoftObjectPath().ToString());
        int32 Count=Cue.Recordings.Num();Writer<<Count;
        for (const auto& Recording:Cue.Recordings)
        {
            Name(Recording.SourceId.ToString());Name(Recording.Wave.ToSoftObjectPath().ToString());float Level=Recording.GainDb;Writer<<Level;
            float VolumeMin=Recording.VolumeMinimum,VolumeMax=Recording.VolumeMaximum;
            float PitchMin=Recording.PitchMinimum,PitchMax=Recording.PitchMaximum;
            Writer<<VolumeMin<<VolumeMax<<PitchMin<<PitchMax;
            Candidate.bTemporaryContent|=Recording.SourceId.ToString().StartsWith(TEXT("placeholder:"));
        }
    }
    Candidate.Identity=LexToString(FBlake3::HashBuffer(Bytes.GetData(),Bytes.Num())).ToLower();
    // The full identity is compared at admission. This nonzero compact token only rejects mismatched event streams.
    Candidate.Revision=Providers.IsEmpty() ? uint32(Base.Revision) : uint32(FCString::Strtoui64(*Candidate.Identity.Left(8),nullptr,16));
    if (!Candidate.Revision) Candidate.Revision=1;
    Out=MoveTemp(Candidate);return true;
}
