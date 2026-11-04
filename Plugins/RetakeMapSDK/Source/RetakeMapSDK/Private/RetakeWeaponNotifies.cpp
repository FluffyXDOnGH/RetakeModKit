// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeWeaponNotifies.h"
#include "RetakeWeaponNotifyPresentation.h"
#include "Animation/AnimTypes.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

void URetakeAnimNotifyState_HandProp::NotifyBegin(USkeletalMeshComponent* Component,UAnimSequenceBase* Animation,
    float Duration,const FAnimNotifyEventReference& Event)
{
    Super::NotifyBegin(Component,Animation,Duration,Event);
    if (auto* State=URetakeWeaponNotifyPresentation::FindOrCreate(Component)) State->BeginProp(Component,this,Mesh,Bone,Offset,Event);
}
void URetakeAnimNotifyState_HandProp::NotifyEnd(USkeletalMeshComponent* Component,UAnimSequenceBase* Animation,const FAnimNotifyEventReference& Event)
{
    Super::NotifyEnd(Component,Animation,Event);
    if (Component && Component->GetOwner()) if (auto* State=Component->GetOwner()->FindComponentByClass<URetakeWeaponNotifyPresentation>()) State->EndScope(Component,this,Event);
}
FString URetakeAnimNotifyState_HandProp::GetNotifyName_Implementation() const { return TEXT("Retake Hand Prop"); }

void URetakeAnimNotifyState_HideWeaponSection::NotifyBegin(USkeletalMeshComponent* Component,UAnimSequenceBase* Animation,
    float Duration,const FAnimNotifyEventReference& Event)
{
    Super::NotifyBegin(Component,Animation,Duration,Event);
    if (auto* State=URetakeWeaponNotifyPresentation::FindOrCreate(Component)) State->BeginHide(Component,this,MaterialSlot,Event);
}
void URetakeAnimNotifyState_HideWeaponSection::NotifyEnd(USkeletalMeshComponent* Component,UAnimSequenceBase* Animation,const FAnimNotifyEventReference& Event)
{
    Super::NotifyEnd(Component,Animation,Event);
    if (Component && Component->GetOwner()) if (auto* State=Component->GetOwner()->FindComponentByClass<URetakeWeaponNotifyPresentation>()) State->EndScope(Component,this,Event);
}
FString URetakeAnimNotifyState_HideWeaponSection::GetNotifyName_Implementation() const { return FString::Printf(TEXT("Hide Weapon: %s"),*MaterialSlot.ToString()); }

void URetakeAnimNotifyState_WeaponSectionVisibility::NotifyBegin(USkeletalMeshComponent* Component,UAnimSequenceBase* Animation,
    float Duration,const FAnimNotifyEventReference& Event)
{
    Super::NotifyBegin(Component,Animation,Duration,Event);
    if (auto* State=URetakeWeaponNotifyPresentation::FindOrCreate(Component))
        State->BeginVisibility(Component,this,MaterialSlot,bVisible,bAnimatedMesh,Event);
}
void URetakeAnimNotifyState_WeaponSectionVisibility::NotifyEnd(USkeletalMeshComponent* Component,UAnimSequenceBase* Animation,
    const FAnimNotifyEventReference& Event)
{
    Super::NotifyEnd(Component,Animation,Event);
    if (Component && Component->GetOwner())
        if (auto* State=Component->GetOwner()->FindComponentByClass<URetakeWeaponNotifyPresentation>()) State->EndScope(Component,this,Event);
}
FString URetakeAnimNotifyState_WeaponSectionVisibility::GetNotifyName_Implementation() const
{ return FString::Printf(TEXT("%s Section: %s"),bVisible ? TEXT("Show") : TEXT("Hide"),*MaterialSlot.ToString()); }

URetakeAnimNotify_WeaponRechamber::URetakeAnimNotify_WeaponRechamber(const FObjectInitializer& Initializer):Super(Initializer)
{
#if WITH_EDITORONLY_DATA
    bShouldFireInEditor=false; NotifyColor=FColor(70,180,220);
#endif
}
void URetakeAnimNotify_WeaponRechamber::Notify(USkeletalMeshComponent* Component,UAnimSequenceBase* Animation,const FAnimNotifyEventReference& Event)
{
    if (!IsValid(Component) || !Component->GetWorld() || !Component->GetWorld()->IsGameWorld() || Component->GetNetMode()==NM_DedicatedServer) return;
    RetakeWeaponPresentationHooks::Rechamber().ExecuteIfBound(Component,Animation,Event);
}
FString URetakeAnimNotify_WeaponRechamber::GetNotifyName_Implementation() const { return TEXT("Retake Weapon Rechamber"); }
#if WITH_EDITOR
void URetakeAnimNotify_WeaponRechamber::OnAnimNotifyCreatedInEditor(FAnimNotifyEvent& Event)
{
    Super::OnAnimNotifyCreatedInEditor(Event);
    Event.bTriggerOnDedicatedServer=false; Event.NotifyTriggerChance=1.f;
    Event.NotifyFilterType=ENotifyFilterType::NoFiltering; Event.bCanBeFilteredViaRequest=false; Event.MontageTickType=EMontageNotifyTickType::Queued;
}
#endif
