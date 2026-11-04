// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeAmbienceProfile.h"
#include "Sound/SoundBase.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

namespace
{
bool IsValidInterval(const FFloatInterval& Range,float Minimum)
{ return FMath::IsFinite(Range.Min) && FMath::IsFinite(Range.Max) && Range.Min>=Minimum && Range.Min<=Range.Max; }
}

bool URetakeAmbienceProfile::ValidateProfile(TArray<FString>& Errors) const
{
    Errors.Reset();
    for (int32 i=0;i<Beds.Num();++i)
    {
        const USoundBase* Sound=Beds[i].Sound;
        if (!Sound) Errors.Add(FString::Printf(TEXT("Bed %d has no sound."),i));
        // A one-shot bed would fall silent after its first play.
        else if (!Sound->IsLooping()) Errors.Add(FString::Printf(TEXT("Bed %d (%s) does not loop."),i,*Sound->GetName()));
        if (!FMath::IsFinite(Beds[i].Volume) || Beds[i].Volume<0.f || Beds[i].Volume>2.f)
            Errors.Add(FString::Printf(TEXT("Bed %d volume must be in [0,2]."),i));
    }
    for (int32 i=0;i<Elements.Num();++i)
    {
        const FRetakeAmbienceElement& Element=Elements[i];
        if (Element.MaxActive<1 || Element.MaxActive>8) Errors.Add(FString::Printf(TEXT("Element %d MaxActive must be in [1,8]."),i));
        if (Element.Sounds.IsEmpty()) Errors.Add(FString::Printf(TEXT("Element %d has no sounds."),i));
        for (const USoundBase* Sound:Element.Sounds)
        {
            if (!Sound) Errors.Add(FString::Printf(TEXT("Element %d has an empty sound slot."),i));
            // A looping element never finishes, so it would hold its voice slot forever.
            else if (Sound->IsLooping()) Errors.Add(FString::Printf(TEXT("Element %d (%s) loops."),i,*Sound->GetName()));
        }
        if (!IsValidInterval(Element.Interval,.5f)) Errors.Add(FString::Printf(TEXT("Element %d interval must be at least 0.5 s with Min <= Max."),i));
        if (!IsValidInterval(Element.Distance,0.f)) Errors.Add(FString::Printf(TEXT("Element %d distance must be non-negative with Min <= Max."),i));
        if (!FMath::IsFinite(Element.Height.Min) || !FMath::IsFinite(Element.Height.Max) || Element.Height.Min>Element.Height.Max)
            Errors.Add(FString::Printf(TEXT("Element %d height needs Min <= Max."),i));
        if (!IsValidInterval(Element.Volume,0.f) || !IsValidInterval(Element.Pitch,.1f))
            Errors.Add(FString::Printf(TEXT("Element %d volume or pitch range is invalid."),i));
    }
    if (!FMath::IsFinite(FadeSeconds) || FadeSeconds<0) Errors.Add(TEXT("FadeSeconds must be non-negative."));
    return Errors.IsEmpty();
}

#if WITH_EDITOR
EDataValidationResult URetakeAmbienceProfile::IsDataValid(FDataValidationContext& Context) const
{
    const EDataValidationResult Parent=Super::IsDataValid(Context);
    TArray<FString> Errors;
    ValidateProfile(Errors);
    for (const FString& Error:Errors) Context.AddError(FText::FromString(Error));
    if (!Errors.IsEmpty()) return EDataValidationResult::Invalid;
    return Parent==EDataValidationResult::Invalid ? Parent : EDataValidationResult::Valid;
}
#endif
