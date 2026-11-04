// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeAmbienceZoneComponent.h"
#include "RetakeMapElementSubsystem.h"
#include "GameFramework/Actor.h"
#if WITH_EDITOR
#include "Components/BrushComponent.h"
#include "GameFramework/Volume.h"
#include "Misc/DataValidation.h"
#endif

const FName URetakeAmbienceZoneComponent::StateChannel(TEXT("Ambience.Listener"));

URetakeAmbienceZoneComponent::URetakeAmbienceZoneComponent() { PrimaryComponentTick.bCanEverTick=false; }

void URetakeAmbienceZoneComponent::BeginPlay()
{
    Super::BeginPlay();
    URetakeMapElementSubsystem::Register(*GetOwner());
}

void URetakeAmbienceZoneComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    URetakeMapElementSubsystem::Unregister(*GetOwner());
    Super::EndPlay(Reason);
}

// The volume's toggle only changes state where an audio listener exists, so these never run on a dedicated server.
void URetakeAmbienceZoneComponent::OnListenerEnter_Implementation() { SetListenerInside(true); }
void URetakeAmbienceZoneComponent::OnListenerExit_Implementation() { SetListenerInside(false); }

void URetakeAmbienceZoneComponent::SetListenerInside(bool bInside)
{
    if (bListenerInside==bInside) return;
    bListenerInside=bInside;
    URetakeMapElementSubsystem::NotifyStateChanged(*GetOwner(),StateChannel);
}

#if WITH_EDITOR
EDataValidationResult URetakeAmbienceZoneComponent::IsDataValid(FDataValidationContext& Context) const
{
    EDataValidationResult Result=Super::IsDataValid(Context);
    const auto Fail=[&Context,&Result](const TCHAR* Error) { Context.AddError(FText::FromString(Error)); Result=EDataValidationResult::Invalid; };
    if (!Profile) Fail(TEXT("The ambience zone has no profile."));
    const AActor* Owner=GetOwner();
    if (!Owner) return Result;
    // Enter/exit come from the Audio Gameplay Volume's toggle component; without it the zone never activates.
    static const FTopLevelAssetPath TogglePath(TEXT("/Script/AudioGameplayVolume"),TEXT("AudioGameplayVolumeComponent"));
    UClass* Toggle=FindObject<UClass>(TogglePath);
    if (!Toggle || !Owner->FindComponentByClass(Toggle)) Fail(TEXT("The ambience zone must be on an Audio Gameplay Volume."));
    TInlineComponentArray<URetakeAmbienceZoneComponent*> Zones(Owner);
    if (Zones.Num()>1) Fail(TEXT("An Audio Gameplay Volume holds at most one ambience zone."));
    // The listener test is the volume's own shape; a brush that answers queries would also block traces through the space.
    const AVolume* Volume=Cast<AVolume>(Owner);
    if (Volume && Volume->GetBrushComponent() && Volume->GetBrushComponent()->IsQueryCollisionEnabled())
        Fail(TEXT("The ambience zone's volume brush must have no collision (No Collision profile)."));
    return Result;
}
#endif
