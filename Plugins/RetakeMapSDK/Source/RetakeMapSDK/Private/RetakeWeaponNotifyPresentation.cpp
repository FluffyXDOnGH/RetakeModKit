// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeWeaponNotifyPresentation.h"
#include "Animation/ActiveMontageInstanceScope.h"
#include "Animation/AnimNotifyQueue.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace RetakeWeaponPresentationHooks
{
    FWeaponVisible& WeaponVisible() { static FWeaponVisible Hook; return Hook; }
    FRechamber& Rechamber() { static FRechamber Hook; return Hook; }
}
namespace
{
    int32 MontageId(const FAnimNotifyEventReference& Event)
    {
        const auto* Context=Event.GetContextData<UE::Anim::FAnimNotifyMontageInstanceContext>();
        return Context ? Context->MontageInstanceID : INDEX_NONE;
    }
}
URetakeWeaponNotifyPresentation::URetakeWeaponNotifyPresentation() { PrimaryComponentTick.bCanEverTick=false; }
URetakeWeaponNotifyPresentation* URetakeWeaponNotifyPresentation::FindOrCreate(USkeletalMeshComponent* Mesh)
{
    if (!Mesh || !Mesh->GetOwner() || Mesh->GetNetMode()==NM_DedicatedServer) return nullptr;
    auto* Owner=Mesh->GetOwner();
    if (auto* Existing=Owner->FindComponentByClass<URetakeWeaponNotifyPresentation>()) return Existing;
    auto* Result=NewObject<URetakeWeaponNotifyPresentation>(Owner,NAME_None,RF_Transient);
    Result->RegisterComponent(); return Result;
}
int32 URetakeWeaponNotifyPresentation::FindScope(USkeletalMeshComponent* Mesh,const UObject* Notify,const FAnimNotifyEventReference& Event) const
{
    const int32 Instance=MontageId(Event);
    return Scopes.IndexOfByPredicate([&](const auto& Scope){return Scope.Source==Mesh && Scope.Notify==Notify && Scope.MontageId==Instance;});
}
void URetakeWeaponNotifyPresentation::BeginProp(USkeletalMeshComponent* Mesh,const UObject* Notify,UStreamableRenderAsset* Asset,
    FName Bone,const FTransform& Offset,const FAnimNotifyEventReference& Event)
{
    if (!Mesh || !Asset || !Mesh->DoesSocketExist(Bone) || FindScope(Mesh,Notify,Event)!=INDEX_NONE) return;
    UMeshComponent* Prop=nullptr;
    if (auto* Skeletal=Cast<USkeletalMesh>(Asset))
    { auto* C=NewObject<USkeletalMeshComponent>(GetOwner(),NAME_None,RF_Transient); C->SetSkeletalMesh(Skeletal); Prop=C; }
    else if (auto* Static=Cast<UStaticMesh>(Asset))
    { auto* C=NewObject<UStaticMeshComponent>(GetOwner(),NAME_None,RF_Transient); C->SetStaticMesh(Static); Prop=C; }
    if (!Prop) return;
    Prop->SetCollisionEnabled(ECollisionEnabled::NoCollision); Prop->SetCastShadow(Mesh->CastShadow);
    Prop->SetFirstPersonPrimitiveType(Mesh->FirstPersonPrimitiveType);
    Prop->SetOnlyOwnerSee(Mesh->bOnlyOwnerSee); Prop->SetOwnerNoSee(Mesh->bOwnerNoSee);
    if (Mesh->FirstPersonPrimitiveType==EFirstPersonPrimitiveType::FirstPerson && RetakeWeaponPresentationHooks::WeaponVisible().IsBound())
        Prop->SetHiddenInGame(!RetakeWeaponPresentationHooks::WeaponVisible().Execute());
    Prop->bUseAttachParentBound=true; Prop->SetupAttachment(Mesh,Bone); Prop->SetRelativeTransform(Offset); Prop->RegisterComponent();
    FScope& Scope=Scopes.AddDefaulted_GetRef(); Scope.Source=Mesh; Scope.Notify=Notify; Scope.MontageId=MontageId(Event); Scope.Prop=Prop;
}
void URetakeWeaponNotifyPresentation::BeginHide(USkeletalMeshComponent* Mesh,const UObject* Notify,FName Slot,const FAnimNotifyEventReference& Event)
{
    BeginVisibility(Mesh,Notify,Slot,false,false,Event);
}
void URetakeWeaponNotifyPresentation::BeginVisibility(USkeletalMeshComponent* Mesh,const UObject* Notify,FName Slot,
    bool bVisible,bool bAnimatedMesh,const FAnimNotifyEventReference& Event)
{
    if (!Mesh || Slot.IsNone() || FindScope(Mesh,Notify,Event)!=INDEX_NONE) return;
    FScope& Scope=Scopes.AddDefaulted_GetRef(); Scope.Source=Mesh; Scope.Notify=Notify; Scope.MontageId=MontageId(Event);
    TArray<USkeletalMeshComponent*> Targets;
    if (bAnimatedMesh) Targets.Add(Mesh);
    else for (USceneComponent* Child:Mesh->GetAttachChildren())
    {
        auto* Weapon=Cast<USkeletalMeshComponent>(Child);
        if (Weapon && Weapon->GetOwner()!=Mesh->GetOwner()) Targets.Add(Weapon);
    }
    for (USkeletalMeshComponent* Weapon:Targets)
    {
        if (!Weapon->GetSkinnedAsset()) continue;
        const int32 Material=Weapon->GetSkinnedAsset()->GetMaterials().IndexOfByPredicate([&](const FSkeletalMaterial& M)
            {return M.MaterialSlotName.IsEqual(Slot,ENameCase::IgnoreCase);});
        if (Material==INDEX_NONE) continue;
        // Every writer of one weapon section shares its owner's stack, even when the notify runs on a character.
        if (auto* Target=FindOrCreate(Weapon)) Scope.Visibility.Add({Target,Target->AddVisibility(Weapon,Material,bVisible)});
    }
}
uint64 URetakeWeaponNotifyPresentation::AddVisibility(USkeletalMeshComponent* Mesh,int32 Material,bool bVisible)
{
    FMaterialVisibility* State=Materials.FindByPredicate([&](const auto& Item){return Item.Mesh==Mesh && Item.Material==Material;});
    if (!State)
    {
        State=&Materials.AddDefaulted_GetRef(); State->Mesh=Mesh; State->Material=Material;
        for (int32 LOD=0;LOD<Mesh->GetNumLODs();++LOD) State->DefaultShown.Add(Mesh->IsMaterialSectionShown(Material,LOD));
    }
    const uint64 Token=++NextVisibilityToken;
    State->Overrides.Add({Token,bVisible});
    ApplyVisibility(*State);
    return Token;
}
void URetakeWeaponNotifyPresentation::ApplyVisibility(const FMaterialVisibility& State)
{
    if (auto* Mesh=State.Mesh.Get())
        for (int32 LOD=0;LOD<State.DefaultShown.Num() && LOD<Mesh->GetNumLODs();++LOD)
            Mesh->ShowMaterialSection(State.Material,INDEX_NONE,
                State.Overrides.IsEmpty() ? State.DefaultShown[LOD] : State.Overrides.Last().bVisible,LOD);
}
void URetakeWeaponNotifyPresentation::RemoveVisibility(uint64 Token)
{
    for (int32 Index=0;Index<Materials.Num();++Index)
    {
        auto& State=Materials[Index];
        const int32 Override=State.Overrides.IndexOfByPredicate([Token](const auto& Item){return Item.Token==Token;});
        if (Override==INDEX_NONE) continue;
        State.Overrides.RemoveAt(Override); // Keep chronological precedence when an older scope ends first.
        ApplyVisibility(State);
        if (State.Overrides.IsEmpty()) Materials.RemoveAtSwap(Index);
        return;
    }
}
void URetakeWeaponNotifyPresentation::ClearTargetVisibility(USkeletalMeshComponent* Mesh)
{
    for (int32 Index=Materials.Num()-1;Index>=0;--Index)
        if (!Mesh || Materials[Index].Mesh==Mesh)
        {
            Materials[Index].Overrides.Reset(); ApplyVisibility(Materials[Index]); Materials.RemoveAtSwap(Index);
        }
}
void URetakeWeaponNotifyPresentation::ApplyDefaultMaterialVisibility(USkeletalMeshComponent* Mesh,TConstArrayView<FName> HiddenSlots)
{
    if (!Mesh || !Mesh->GetSkinnedAsset()) return;
    const auto& Slots=Mesh->GetSkinnedAsset()->GetMaterials();
    for (int32 LOD=0;LOD<Mesh->GetNumLODs();++LOD)
    {
        Mesh->ShowAllMaterialSections(LOD);
        for (int32 Material=0;Material<Slots.Num();++Material)
            if (HiddenSlots.Contains(Slots[Material].MaterialSlotName)) Mesh->ShowMaterialSection(Material,INDEX_NONE,false,LOD);
    }
    if (auto* Owner=Mesh->GetOwner()) if (auto* State=Owner->FindComponentByClass<URetakeWeaponNotifyPresentation>())
        for (auto& Item:State->Materials) if (Item.Mesh==Mesh)
        {
            Item.DefaultShown.Reset();
            for (int32 LOD=0;LOD<Mesh->GetNumLODs();++LOD) Item.DefaultShown.Add(Mesh->IsMaterialSectionShown(Item.Material,LOD));
            ApplyVisibility(Item);
        }
}
void URetakeWeaponNotifyPresentation::RemoveScope(int32 Index)
{
    FScope& Scope=Scopes[Index];
    if (auto* Prop=Scope.Prop.Get()) Prop->DestroyComponent();
    for (const auto& Ref:Scope.Visibility) if (auto* Target=Ref.Key.Get()) Target->RemoveVisibility(Ref.Value);
    Scopes.RemoveAtSwap(Index);
}
void URetakeWeaponNotifyPresentation::EndScope(USkeletalMeshComponent* Mesh,const UObject* Notify,const FAnimNotifyEventReference& Event)
{ const int32 At=FindScope(Mesh,Notify,Event); if (At!=INDEX_NONE) RemoveScope(At); }
void URetakeWeaponNotifyPresentation::Clear(USkeletalMeshComponent* Source)
{ for (int32 Index=Scopes.Num()-1;Index>=0;--Index) if (!Source || Scopes[Index].Source==Source) RemoveScope(Index); }
void URetakeWeaponNotifyPresentation::ClearForMesh(USkeletalMeshComponent* Mesh)
{ if (Mesh && Mesh->GetOwner()) if (auto* State=Mesh->GetOwner()->FindComponentByClass<URetakeWeaponNotifyPresentation>()) {State->Clear(Mesh);State->ClearTargetVisibility(Mesh);} }
void URetakeWeaponNotifyPresentation::ClearForActor(AActor* Owner)
{ if (Owner) if (auto* State=Owner->FindComponentByClass<URetakeWeaponNotifyPresentation>()) {State->Clear();State->ClearTargetVisibility();} }
void URetakeWeaponNotifyPresentation::EndPlay(const EEndPlayReason::Type Reason) { Clear(); ClearTargetVisibility(); Super::EndPlay(Reason); }
void URetakeWeaponNotifyPresentation::OnComponentDestroyed(bool bDestroyingHierarchy) { Clear(); ClearTargetVisibility(); Super::OnComponentDestroyed(bDestroyingHierarchy); }
