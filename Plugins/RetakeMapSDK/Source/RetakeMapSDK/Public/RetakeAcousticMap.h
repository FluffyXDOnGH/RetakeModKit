// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "RetakeAcousticMapData.h"
#include "RetakeAcousticMap.generated.h"

class UBoxComponent;
class UStaticMeshComponent;
class ARetakeDoor;

/** Invisible simplified structural collision; affects only the dedicated acoustic trace channel. */
UCLASS(NotBlueprintable, meta=(DisplayName="Retake Acoustic Occluder"))
class RETAKEMAPSDK_API ARetakeAcousticOccluder : public AInfo
{
    GENERATED_BODY()
public:
    ARetakeAcousticOccluder();
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") TObjectPtr<UBoxComponent> Bounds;
};

/** An authored oriented region. Overlap resolution uses priority, then smaller volume, then stable ID. */
UCLASS(NotBlueprintable, meta=(DisplayName="Retake Acoustic Region"))
class RETAKEMAPSDK_API ARetakeAcousticRegion : public AInfo
{
    GENERATED_BODY()
public:
    ARetakeAcousticRegion();
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") TObjectPtr<UBoxComponent> Bounds;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio") FName RegionId;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio") int32 Priority = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio") FRetakeAcousticMaterial InternalTransmission;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio", meta=(ClampMin="0.1", ClampMax="5", Units="s")) float ReverbDecaySeconds = .5f;
};

/** Doorways, structural transmission boundaries and authored destructible openings. */
UCLASS(NotBlueprintable, meta=(DisplayName="Retake Acoustic Connection"))
class RETAKEMAPSDK_API ARetakeAcousticConnection : public AInfo
{
    GENERATED_BODY()
public:
    ARetakeAcousticConnection();
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Retake|Audio") TObjectPtr<UBoxComponent> Bounds;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio") FName ConnectionId;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio") TObjectPtr<ARetakeAcousticRegion> RegionA;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio") TObjectPtr<ARetakeAcousticRegion> RegionB;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio") FRetakeAcousticMaterial OpenMaterial;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio") FRetakeAcousticMaterial ClosedMaterial;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio") bool bInitiallyOpen = true;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio") bool bDynamic = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio") TObjectPtr<ARetakeDoor> Door;
    /** Structural destruction calls this on the server. Door-bound connections instead observe the door state channel. */
    UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category="Retake|Audio") void SetOpen(bool bOpen);
    bool IsOpen() const;
    static const FName StateChannel;
protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
private:
    UPROPERTY(ReplicatedUsing=OnRep_Open) bool bOpenState = true;
    UFUNCTION() void OnRep_Open();
};

/** Exactly one in each gameplay map. Holds the cooked bake through a hard reference. */
UCLASS(NotBlueprintable, meta=(DisplayName="Retake Acoustic Map"))
class RETAKEMAPSDK_API ARetakeAcousticMap : public AInfo
{
    GENERATED_BODY()
public:
    ARetakeAcousticMap();
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Retake|Audio") TObjectPtr<URetakeAcousticMapData> Data;
    /**
     * Baked in a fixed sorted order. Dynamic open/closed state is excluded from this signature. Cooked servers recompute it
     * in InitGame, before components register, so it hashes only serialized data that every platform keeps bit-identical.
     */
    bool Gather(TArray<FRetakeAcousticRegionData>& Regions, TArray<FRetakeAcousticConnectionData>& Connections, FString& Signature, FString& Error) const;
    /** A component Gather fingerprints as structure: a static mesh outside doors and RetakeAcousticDynamic actors whose runtime
     *  collision blocks RetakeAcoustics. With Use Default Collision that is its mesh's collision, read from the mesh so the
     *  answer is the same before and after the component registers. */
    static bool IsStructuralBlocker(UStaticMeshComponent& Component);
    bool ValidateBake(FString& Error) const;
#if WITH_EDITOR
    UFUNCTION(CallInEditor, Category="Retake|Audio") void Bake();
    virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};
