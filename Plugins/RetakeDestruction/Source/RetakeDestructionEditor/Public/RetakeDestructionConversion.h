// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "RetakeDestructionConversion.generated.h"

class AStaticMeshActor;
class ARetakeDoor;
class ARetakeDestructiblePlacement;
class URetakeDestructionDefinition;
class UMaterialInterface;

/** Voronoi recipe of a generated collection: root -> chunks -> pieces. */
USTRUCT(BlueprintType)
struct FRetakeFractureRecipe
{
    GENERATED_BODY()
    /**
     * First level: chunks. With ChunkFragmentCount above 1 each chunk is fractured again, like the reference walls. Bullet strain
     * reaches pieces within the preset's point radius, so chunks no wider than about twice that radius open a hole per hit.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Retake|Destruction|Fractures", meta=(ClampMin="2",ClampMax="256")) int32 FragmentCount=32;
    /** Second level: pieces per chunk (0 or 1 = single level). A hit frees pieces near it; untouched chunks stay whole. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Retake|Destruction|Fractures", meta=(ClampMin="0",ClampMax="32")) int32 ChunkFragmentCount=0;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Retake|Destruction|Fractures", meta=(ClampMin="0")) int32 RandomSeed=1337;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Retake|Destruction|Fractures", meta=(ClampMin="0",ClampMax="5",Units="cm")) float CutNoiseCm=0;
};

USTRUCT(BlueprintType)
struct FRetakeDestructionConversionOptions
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Retake|Destruction|Assets") FString AssetName=TEXT("Destructible");
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Retake|Destruction|Fractures", meta=(ShowOnlyInnerProperties)) FRetakeFractureRecipe Fracture;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Retake|Destruction|Materials") TObjectPtr<UMaterialInterface> InteriorMaterial;
    /**
     * Copies damage, effects and cleanup settings; geometry and materials come from the selected mesh. A preset using the
     * reference response (e.g. DA_Destruction_Wall_Plaster) also hands over its collection's damage, connection, mass,
     * sleep and per-piece removal settings, so the new mesh breaks in the reference style.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Retake|Destruction|Definitions") TObjectPtr<URetakeDestructionDefinition> Preset;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Retake|Destruction|Anchors") bool bAnchorBase=false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Retake|Destruction|Anchors", meta=(EditCondition="bAnchorBase",ClampMin=".1",Units="cm")) float AnchorDepthCm=5;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Retake|Destruction|Anchors") bool bAnchorEnds=false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Retake|Destruction|Editor") bool bOpenFractureEditor=true;
};

/**
 * Editor authoring API. Generated assets go under the content root of the converted level or re-fractured definition: Destruction
 * below /Game/Retake in Retake, Destruction below a mod's own root in the Map Kit. Presets come from the same root.
 */
UCLASS()
class RETAKEDESTRUCTIONEDITOR_API URetakeDestructionConversion : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="Retake|Destruction|Editor", meta=(DevelopmentOnly))
    static ARetakeDestructiblePlacement* ConvertActor(AStaticMeshActor* Source,const FRetakeDestructionConversionOptions& Options,FString& Message);
    UFUNCTION(BlueprintPure, Category="Retake|Destruction|Editor", meta=(DevelopmentOnly))
    static bool CanConvertActor(AStaticMeshActor* Source,FString& Reason);
    /**
     * Converts Door's leaf (its mesh at the closed pose) like ConvertActor, and links the new placement to Door so it becomes the
     * door's destructible leaf (ARetakeDestructiblePlacement::Door). The door keeps its leaf mesh as the leaf's pawn body.
     */
    UFUNCTION(BlueprintCallable, Category="Retake|Destruction|Editor", meta=(DevelopmentOnly))
    static ARetakeDestructiblePlacement* ConvertDoorLeaf(ARetakeDoor* Door,const FRetakeDestructionConversionOptions& Options,FString& Message);
    UFUNCTION(BlueprintPure, Category="Retake|Destruction|Editor", meta=(DevelopmentOnly))
    static bool CanConvertDoorLeaf(ARetakeDoor* Door,FString& Reason);
    /**
     * Re-fractures a converter-generated definition in place, re-applies Preset's reference response and removal data (when
     * Preset uses it), re-prepares the definition and saves it. Placements and Blueprint presets keep their reference.
     */
    UFUNCTION(BlueprintCallable, Category="Retake|Destruction|Editor", meta=(DevelopmentOnly))
    static bool RefractureDefinition(URetakeDestructionDefinition* Definition,const FRetakeFractureRecipe& Recipe,URetakeDestructionDefinition* Preset,FString& Message);
};
