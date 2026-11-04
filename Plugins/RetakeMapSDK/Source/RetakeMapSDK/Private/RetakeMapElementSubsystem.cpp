// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeMapElementSubsystem.h"

#include "Engine/DemoNetDriver.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

URetakeMapElementSubsystem* URetakeMapElementSubsystem::Get(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	return World ? World->GetSubsystem<URetakeMapElementSubsystem>() : nullptr;
}

void URetakeMapElementSubsystem::Register(AActor& Element)
{
	URetakeMapElementSubsystem* Self = Get(&Element);
	if (!Self || Self->Elements.Contains(&Element)) return;
	Self->Elements.Add(&Element);
	Self->OnElementAdded.Broadcast(Element);
}

void URetakeMapElementSubsystem::Unregister(AActor& Element)
{
	URetakeMapElementSubsystem* Self = Get(&Element);
	if (!Self || !Self->Elements.Contains(&Element)) return;
	Self->OnElementRemoved.Broadcast(Element);
	Self->Elements.RemoveSingle(&Element);
}

void URetakeMapElementSubsystem::NotifyStateChanged(AActor& Element, FName Channel)
{
	URetakeMapElementSubsystem* Self = Get(&Element);
	if (Self && Self->Elements.Contains(&Element)) Self->OnElementStateChanged.Broadcast(Element, Channel);
}

bool URetakeMapElementSubsystem::IsReconstructingReplay(const UWorld* World)
{
	if (!World || !World->IsPlayingReplay()) return false;
	const UDemoNetDriver* Driver = World->GetDemoNetDriver();
	return Driver && (Driver->IsLoadingCheckpoint() || Driver->IsFastForwarding());
}

void URetakeMapElementSubsystem::Unobserve(FRetakeMapElementObservation& Observation)
{
	OnElementAdded.Remove(Observation.Added);
	OnElementRemoved.Remove(Observation.Removed);
	OnElementStateChanged.Remove(Observation.StateChanged);
	Observation = FRetakeMapElementObservation();
}

bool URetakeMapElementSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void URetakeMapElementSubsystem::Deinitialize()
{
	OnElementAdded.Clear();
	OnElementRemoved.Clear();
	OnElementStateChanged.Clear();
	Elements.Reset();
	Super::Deinitialize();
}
