// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "RetakeWeaponNotifyPresentation.generated.h"

class USkeletalMeshComponent;
class UMeshComponent;
class UStreamableRenderAsset;
class UAnimSequenceBase;
struct FAnimNotifyEventReference;

namespace RetakeWeaponPresentationHooks
{
    DECLARE_DELEGATE_RetVal(bool,FWeaponVisible);
    DECLARE_DELEGATE_ThreeParams(FRechamber,USkeletalMeshComponent*,UAnimSequenceBase*,const FAnimNotifyEventReference&);
    RETAKEMAPSDK_API FWeaponVisible& WeaponVisible();
    RETAKEMAPSDK_API FRechamber& Rechamber();
}

/** Per-owner cosmetic state. Notify assets contain authoring data only; no gameplay or replicated state lives here. */
UCLASS(Transient, NotBlueprintable)
class RETAKEMAPSDK_API URetakeWeaponNotifyPresentation : public UActorComponent
{
    GENERATED_BODY()
public:
    URetakeWeaponNotifyPresentation();
    static URetakeWeaponNotifyPresentation* FindOrCreate(USkeletalMeshComponent* Mesh);
    static void ClearForMesh(USkeletalMeshComponent* Mesh);
    static void ClearForActor(AActor* Owner);
    /** Apply definition defaults, retaining any active cosmetic overrides on this mesh. */
    static void ApplyDefaultMaterialVisibility(USkeletalMeshComponent* Mesh,TConstArrayView<FName> HiddenSlots);
    void BeginProp(USkeletalMeshComponent* Mesh,const UObject* Notify,UStreamableRenderAsset* Asset,FName Bone,
        const FTransform& Offset,const FAnimNotifyEventReference& Event);
    void BeginHide(USkeletalMeshComponent* Mesh,const UObject* Notify,FName Slot,const FAnimNotifyEventReference& Event);
    /** AnimatedMesh targets the source weapon clip itself; false targets weapons attached to a character mesh. */
    void BeginVisibility(USkeletalMeshComponent* Mesh,const UObject* Notify,FName Slot,bool bVisible,bool bAnimatedMesh,
        const FAnimNotifyEventReference& Event);
    void EndScope(USkeletalMeshComponent* Mesh,const UObject* Notify,const FAnimNotifyEventReference& Event);
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
private:
    struct FScope
    {
        TWeakObjectPtr<USkeletalMeshComponent> Source;
        TWeakObjectPtr<const UObject> Notify;
        int32 MontageId=INDEX_NONE;
        TWeakObjectPtr<UMeshComponent> Prop;
        TArray<TPair<TWeakObjectPtr<URetakeWeaponNotifyPresentation>,uint64>> Visibility;
    };
    struct FVisibilityOverride
    {
        uint64 Token=0;
        bool bVisible=false;
    };
    struct FMaterialVisibility
    {
        TWeakObjectPtr<USkeletalMeshComponent> Mesh;
        int32 Material=INDEX_NONE;
        TArray<bool> DefaultShown;
        TArray<FVisibilityOverride> Overrides;
    };
    TArray<FScope> Scopes;
    TArray<FMaterialVisibility> Materials;
    uint64 NextVisibilityToken=0;
    void RemoveScope(int32 Index);
    void Clear(USkeletalMeshComponent* Source=nullptr);
    uint64 AddVisibility(USkeletalMeshComponent* Mesh,int32 Material,bool bVisible);
    void RemoveVisibility(uint64 Token);
    void ClearTargetVisibility(USkeletalMeshComponent* Mesh=nullptr);
    static void ApplyVisibility(const FMaterialVisibility& State);
    int32 FindScope(USkeletalMeshComponent* Mesh,const UObject* Notify,const FAnimNotifyEventReference& Event) const;
};
