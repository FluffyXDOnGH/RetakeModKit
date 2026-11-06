// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeDestructionConversion.h"
#include "RetakeDestructiblePlacement.h"
#include "RetakeDestructionDefinition.h"
#include "RetakeDoor.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Dataflow/AssetDefinition_DataflowAsset.h"
#include "Dataflow/DataflowAssetEditUtils.h"
#include "Dataflow/DataflowBlueprintLibrary.h"
#include "Dataflow/DataflowEdNode.h"
#include "Dataflow/DataflowGraph.h"
#include "Dataflow/DataflowInputOutput.h"
#include "Dataflow/DataflowNode.h"
#include "Dataflow/DataflowObject.h"
#include "Editor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollection/Facades/CollectionRemoveOnBreakFacade.h"
#include "LevelUtils.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunctionInterface.h"
#include "MeshDescription.h"
#include "Misc/DataValidation.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "ObjectTools.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "ScopedTransaction.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshComponentLODInfo.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
/** Where conversion writes and what it may reuse: Retake keeps its destruction content under /Game/Retake, a mod uses its own root. */
struct FConversionTarget { FString Folder,Reusable; };
FConversionTarget ConversionTarget(const UObject& Owner)
{
    const FString Root=FPackageName::GetPackageMountPoint(Owner.GetPackage()->GetName(),false).ToString();
    const FString Reusable=Root==TEXT("/Game/") ? TEXT("/Game/Retake/"):Root;
    return {Reusable/TEXT("Destruction"),Reusable};
}
bool SetNodeProperty(UDataflowEdNode* Node,const TCHAR* PropertyName,const FString& Value)
{
    if (!Node || !Node->GetDataflowNode()) return false;
    auto Data=Node->GetDataflowNode();
    FString Type=Data->GetType().ToString();Type.RemoveFromStart(TEXT("F"));
    UScriptStruct* Struct=FindObject<UScriptStruct>(nullptr,*(TEXT("/Script/GeometryCollectionNodes.")+Type));
    FProperty* Property=Struct ? FindFProperty<FProperty>(Struct,PropertyName):nullptr;
    if (!Property || !Property->ImportText_Direct(*Value,Property->ContainerPtrToValuePtr<void>(Data.Get()),nullptr,PPF_None)) return false;
    Data->Invalidate();return true;
}
bool Connect(UDataflow* Graph,UDataflowEdNode* From,const TCHAR* Output,UDataflowEdNode* To,const TCHAR* Input)
{
    if (!From || !To || !Graph->GetDataflow()) return false;
    auto* Out=From->GetDataflowNode()->FindOutput(FName(Output));
    auto* In=To->GetDataflowNode()->FindInput(FName(Input));
    return Out && In && Graph->GetDataflow()->CanConnect(*Out,*In) && Graph->GetDataflow()->Connect(*Out,*In);
}
bool IsValidRecipe(const FRetakeFractureRecipe& Recipe,FString& Message)
{
    if (Recipe.FragmentCount>=2 && Recipe.FragmentCount<=256 && Recipe.ChunkFragmentCount>=0 && Recipe.ChunkFragmentCount<=32
        && Recipe.FragmentCount*FMath::Max(1,Recipe.ChunkFragmentCount)<=4096 && Recipe.RandomSeed>=0
        && FMath::IsFinite(Recipe.CutNoiseCm) && Recipe.CutNoiseCm>=0 && Recipe.CutNoiseCm<=5) return true;
    Message=TEXT("Use 2–256 fragments, up to 32 per chunk and 4,096 pieces in total, a nonnegative seed and 0–5 cm noise.");
    return false;
}
UDataflowEdNode* FindNode(UDataflow* Graph,FName Name)
{
    if (Graph) for (UEdGraphNode* Node:Graph->Nodes)
        if (auto* EdNode=Cast<UDataflowEdNode>(Node); EdNode && EdNode->GetDataflowNode() && EdNode->GetDataflowNode()->GetName()==Name) return EdNode;
    return nullptr;
}
/** Group fracture spreads one pattern of Sites over the whole selection; per-bone fracture cuts each selected bone into Sites. */
bool SetFractureLevel(UDataflowEdNode* Node,int32 Sites,int32 Seed,int32 InteriorIndex,float NoiseCm,bool bGroupFracture)
{
    return SetNodeProperty(Node,TEXT("GroupFracture"),bGroupFracture ? TEXT("True"):TEXT("False"))
        && SetNodeProperty(Node,TEXT("MinVoronoiSites"),FString::FromInt(Sites)) && SetNodeProperty(Node,TEXT("MaxVoronoiSites"),FString::FromInt(Sites))
        && SetNodeProperty(Node,TEXT("RandomSeed"),FString::FromInt(Seed)) && SetNodeProperty(Node,TEXT("InternalMaterialID"),FString::FromInt(InteriorIndex))
        && SetNodeProperty(Node,TEXT("Amplitude"),FString::SanitizeFloat(NoiseCm)) && SetNodeProperty(Node,TEXT("PointSpacing"),TEXT("20"));
}
/**
 * Source -> RetakeFracture [-> RetakeChunkFracture] -> Terminal. The second level fractures every chunk again (an unconnected
 * selection fractures all geometry), giving root -> chunks -> pieces like the reference walls. Existing nodes are reused.
 */
bool ConfigureFractureGraph(UDataflow* Graph,UDataflowEdNode* Input,UDataflowEdNode* Terminal,const FRetakeFractureRecipe& Recipe,int32 InteriorIndex,FString& Message)
{
    // Dataflow graphs have no public editing API in UE6; the converter uses the engine's own asset edit utilities.
    PRAGMA_DISABLE_INTERNAL_WARNINGS
    UDataflowEdNode* Fracture=FindNode(Graph,TEXT("RetakeFracture"));
    if (!Fracture) Fracture=UE::Dataflow::FEditAssetUtils::AddNewNode(Graph,FVector2D(350,0),TEXT("RetakeFracture"),TEXT("FUniformFractureDataflowNode"),nullptr);
    UDataflowEdNode* Pieces=FindNode(Graph,TEXT("RetakeChunkFracture"));
    if (Pieces && Recipe.ChunkFragmentCount<=1) {UE::Dataflow::FEditAssetUtils::DeleteNodes(Graph,{Pieces});Pieces=nullptr;}
    if (!Pieces && Recipe.ChunkFragmentCount>1)
        Pieces=UE::Dataflow::FEditAssetUtils::AddNewNode(Graph,FVector2D(350,250),TEXT("RetakeChunkFracture"),TEXT("FUniformFractureDataflowNode"),nullptr);
    PRAGMA_ENABLE_INTERNAL_WARNINGS
    UDataflowEdNode* Last=Pieces ? Pieces:Fracture;
    if (!SetFractureLevel(Fracture,Recipe.FragmentCount,Recipe.RandomSeed,InteriorIndex,Recipe.CutNoiseCm,true)
        || (Pieces && !SetFractureLevel(Pieces,Recipe.ChunkFragmentCount,Recipe.RandomSeed+1,InteriorIndex,Recipe.CutNoiseCm,false))
        || !Connect(Graph,Input,TEXT("Collection"),Fracture,TEXT("Collection"))
        || (Pieces && !Connect(Graph,Fracture,TEXT("Collection"),Pieces,TEXT("Collection")))
        || !Connect(Graph,Last,TEXT("Collection"),Terminal,TEXT("Collection"))
        || !Connect(Graph,Input,TEXT("Materials"),Terminal,TEXT("Materials")))
    {Message=TEXT("Could not configure the fracture graph.");return false;}
    for (auto* Node:{Input,Fracture,Pieces,Terminal}) if (Node) Graph->RefreshEdNode(Node);
    return true;
}
/**
 * Settings the Dataflow evaluation resets. A reference-response preset also hands over its collection's damage, connection,
 * mass, sleep and per-piece removal settings, so the new geometry breaks in the reference style.
 */
void ApplyCollectionSettings(UGeometryCollection* Collection,const URetakeDestructionDefinition* Preset)
{
    Collection->EnableClustering=true;
    Collection->DamageModel=EDamageModelTypeEnum::Chaos_Damage_Model_UserDefined_Damage_Threshold;
    Collection->SizeSpecificData.Reset();auto& Size=Collection->SizeSpecificData.AddDefaulted_GetRef();Size.MaxSize=FLT_MAX;
    Size.CollisionShapes.Reset();auto& Shape=Size.CollisionShapes.AddDefaulted_GetRef();
    Shape.CollisionType=ECollisionTypeEnum::Chaos_Volumetric;Shape.ImplicitType=EImplicitTypeEnum::Chaos_Implicit_Convex;
    if (Preset && Preset->Collection)
        Collection->DamageThreshold=Preset->DamageThresholds.IsEmpty() ? Preset->Collection->DamageThreshold:Preset->DamageThresholds;
    else Collection->DamageThreshold={500000,50000,5000};
    if (!Preset || !Preset->bUseAuthoredResponse || !Preset->Collection) return;
    const UGeometryCollection* Source=Preset->Collection;
    for (const TCHAR* Name:{TEXT("DamageThreshold"),TEXT("bUseSizeSpecificDamageThreshold"),TEXT("DamageModel"),TEXT("ClusterConnectionType"),
        TEXT("ConnectionGraphBoundsFilteringMargin"),TEXT("bMassAsDensity"),TEXT("Mass"),TEXT("MinimumMassClamp"),TEXT("bRemoveOnMaxSleep"),
        TEXT("MaximumSleepTime"),TEXT("RemovalDuration"),TEXT("bScaleOnRemoval"),TEXT("bSlowMovingAsSleeping"),TEXT("SlowMovingVelocityThreshold"),
        TEXT("bAutomaticCrumblePartialClusters"),TEXT("EnableDamageFromCollision"),TEXT("bUseMaterialDamageModifiers")})
        if (const FProperty* Property=UGeometryCollection::StaticClass()->FindPropertyByName(Name))
            Property->CopyCompleteValue_InContainer(Collection,Source);
    if (const auto* SourceData=Source->GetGeometryCollection().Get(); SourceData && Collection->GetGeometryCollection())
    {
        const GeometryCollection::Facades::FCollectionRemoveOnBreakFacade SourceRemoval(*SourceData);
        GeometryCollection::Facades::FCollectionRemoveOnBreakFacade Removal(*Collection->GetGeometryCollection());
        if (SourceRemoval.IsValid() && SourceData->Transform.Num()>0) { Removal.DefineSchema(); Removal.SetToAll(SourceRemoval.GetData(0)); }
    }
}
bool HasSceneReferences(AStaticMeshActor* Source,FString& Message)
{
    for (TActorIterator<AActor> It(Source->GetWorld());It;++It)
    {
        if (*It==Source) continue;
        TArray<UObject*> References;
        FReferenceFinder Finder(References,nullptr,false,true,false,true);
        Finder.FindReferences(*It);
        for (UActorComponent* Component:It->GetComponents()) Finder.FindReferences(Component);
        if (References.Contains(Source) || References.Contains(Source->GetStaticMeshComponent()))
        {Message=FString::Printf(TEXT("%s references this mesh actor. Update that reference before converting it."),*It->GetActorLabel());return true;}
    }
    return false;
}
// Copy dependencies through Unreal's package-copy/ref-fixup API. Reuse clean Retake/engine assets.
bool CopySources(UStaticMesh* Mesh,TConstArrayView<UMaterialInterface*> Materials,UMaterialInterface* Interior,UPhysicalMaterial* PhysicsMaterial,
    const FString& Role,const FConversionTarget& Target,TMap<FString,FString>& Copies,FString& Message)
{
    IAssetRegistry& Registry=FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    TArray<FName> Pending{Mesh->GetOutermost()->GetFName()};
    for (const auto* Material:Materials) if (Material) Pending.AddUnique(Material->GetOutermost()->GetFName());
    if (Interior) Pending.AddUnique(Interior->GetOutermost()->GetFName());
    if (PhysicsMaterial) Pending.AddUnique(PhysicsMaterial->GetOutermost()->GetFName());
    TMap<FName,TArray<FName>> Dependencies;
    TSet<FName> MustCopy{Mesh->GetOutermost()->GetFName()};
    auto IsContent=[](FName Name)
    {
        const FString Path=Name.ToString();return Path.StartsWith(TEXT("/")) && !Path.StartsWith(TEXT("/Engine/"))
            && !Path.StartsWith(TEXT("/Script/")) && !Path.StartsWith(TEXT("/Temp/")) && !Path.StartsWith(TEXT("/Memory/"));
    };
    while (!Pending.IsEmpty())
    {
        const FName Name=Pending.Pop();const FString Path=Name.ToString();
        if (Dependencies.Contains(Name) || (!IsContent(Name) && Name!=Mesh->GetOutermost()->GetFName())) continue;
        if (Dependencies.Num()>=256) {Message=TEXT("This mesh has more than 256 content dependencies. Simplify or convert its source library first.");return false;}
        auto& Deps=Dependencies.Add(Name);Registry.GetDependencies(Name,Deps,UE::AssetRegistry::EDependencyCategory::Package);
        if (!Path.StartsWith(Target.Reusable)) MustCopy.Add(Name);
        for (FName Dependency:Deps) if (IsContent(Dependency)) Pending.Add(Dependency);
    }
    bool Changed=true;
    while (Changed)
    {
        Changed=false;
        for (const auto& Pair:Dependencies) if (!MustCopy.Contains(Pair.Key))
            for (FName Dep:Pair.Value) if (MustCopy.Contains(Dep)) {MustCopy.Add(Pair.Key);Changed=true;break;}
    }
    TArray<FName> Ordered=MustCopy.Array();Ordered.Sort(FNameLexicalLess());
    int32 Index=0;
    for (FName Name:Ordered)
    {
        const FString Package=Name.ToString();TArray<FAssetData> Assets;
        Registry.GetAssetsByPackageName(Name,Assets);
        if (Assets.Num()!=1 || !Assets[0].IsValid()) {Message=TEXT("Cannot safely copy source package: ")+Package;return false;}
        UObject* Asset=Assets[0].GetAsset();
        if (!Asset || Asset->IsA<UWorld>() || Asset->GetClass()->ClassGeneratedBy)
        {Message=TEXT("Unsupported source dependency: ")+Package;return false;}
        const FString Prefix=Asset->IsA<UStaticMesh>() ? TEXT("SM_"):Asset->IsA<UMaterial>() ? TEXT("M_"):
            Asset->IsA<UMaterialInterface>() ? TEXT("MI_"):Asset->IsA<UTexture>() ? TEXT("T_"):
            Asset->IsA<UMaterialFunctionInterface>() ? TEXT("MF_"):Asset->IsA<UPhysicalMaterial>() ? TEXT("PM_"):TEXT("Asset_");
        const FString NewName=Asset==Mesh ? TEXT("SM_")+Role+TEXT("_Source"):Prefix+Role+FString::Printf(TEXT("_%03d"),++Index);
        Copies.Add(Package,Target.Folder/TEXT("Sources")/Role/NewName);
    }
    IAssetTools& Tools=FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    TMap<FString,FString> AdvancedCopies=Copies;
    const bool bEngineMesh=Mesh->GetOutermost()->GetName().StartsWith(TEXT("/Engine/"));
    if (bEngineMesh) AdvancedCopies.Remove(Mesh->GetOutermost()->GetName());
    if (!AdvancedCopies.IsEmpty() && !Tools.AdvancedCopyPackages(AdvancedCopies,false,false))
    {Message=TEXT("Unreal could not copy the source dependencies. The original actor is unchanged.");return false;}
    // Advanced Copy deliberately refuses engine primitives; ordinary asset duplication supports them.
    if (bEngineMesh)
    {
        const FString& Package=Copies.FindChecked(Mesh->GetOutermost()->GetName());
        if (!Tools.DuplicateAsset(FPackageName::GetLongPackageAssetName(Package),FPackageName::GetLongPackagePath(Package),Mesh))
        {Message=TEXT("Could not duplicate the engine source mesh.");return false;}
    }
    return true;
}
UObject* Remapped(UObject* Original,const TMap<FString,FString>& Copies)
{
    if (!Original) return nullptr;
    if (const FString* Package=Copies.Find(Original->GetOutermost()->GetName()))
        return LoadObject<UObject>(nullptr,*( *Package+TEXT(".")+FPackageName::GetLongPackageAssetName(*Package)));
    return Original;
}
}

bool URetakeDestructionConversion::RefractureDefinition(URetakeDestructionDefinition* Definition,const FRetakeFractureRecipe& Recipe,URetakeDestructionDefinition* Preset,FString& Message)
{
    if (!GEditor || GEditor->IsPlaySessionInProgress()) {Message=TEXT("Stop Play/Simulate before re-fracturing.");return false;}
    if (!IsValidRecipe(Recipe,Message)) return false;
    UGeometryCollection* Collection=Definition ? Definition->Collection.Get():nullptr;
    UDataflow* Graph=Collection ? Collection->GetDataflowInstance().GetDataflowAsset():nullptr;
    UDataflowEdNode* Input=FindNode(Graph,TEXT("RetakeSource"));UDataflowEdNode* Terminal=FindNode(Graph,TEXT("RetakeTerminal"));
    UDataflowEdNode* Fracture=FindNode(Graph,TEXT("RetakeFracture"));
    if (!Input || !Terminal || !Fracture)
    {Message=TEXT("Only converter-generated collections (embedded RetakeSource/RetakeFracture/RetakeTerminal graph) can be re-fractured.");return false;}
    const FConversionTarget Target=ConversionTarget(*Definition);
    if (Preset && (Preset==Definition || !Preset->GetPathName().StartsWith(Target.Reusable)))
    {Message=TEXT("Choose another definition from ")+Target.Reusable+TEXT(" as the preset.");return false;}
    // Keep the interior material slot the converter chose.
    const FProperty* InteriorProperty=FindFProperty<FProperty>(FindObject<UScriptStruct>(nullptr,TEXT("/Script/GeometryCollectionNodes.UniformFractureDataflowNode")),TEXT("InternalMaterialID"));
    if (!InteriorProperty) {Message=TEXT("Could not read the interior material slot.");return false;}
    int32 InteriorIndex=0;InteriorProperty->GetValue_InContainer(Fracture->GetDataflowNode().Get(),&InteriorIndex);
    const FScopedTransaction Transaction(FText::FromString(TEXT("Re-fracture Retake destructible")));
    Collection->Modify();Definition->Modify();
    if (!ConfigureFractureGraph(Graph,Input,Terminal,Recipe,InteriorIndex,Message)) return false;
    // This UE6 implementation returns bHasError despite the API comment claiming success.
    if (UDataflowBlueprintLibrary::EvaluateDataflow(Graph,Collection) || !Collection->GetGeometryCollection()
        || Collection->GetGeometryCollection()->Transform.Num()<3) {Message=TEXT("The mesh could not be fractured into usable pieces.");return false;}
    ApplyCollectionSettings(Collection,Preset);
    if (Preset) Definition->bUseAuthoredResponse=Preset->bUseAuthoredResponse && Preset->Collection;
    Definition->PreparedCollectionGuid.Invalidate();
    if (!Definition->PrepareCollection()) {Message=TEXT("Collection preparation failed.");return false;}
    FDataValidationContext Validation;
    if (Definition->IsDataValid(Validation)!=EDataValidationResult::Valid) {Message=TEXT("The re-fractured definition failed validation.");return false;}
    TArray<UPackage*> Packages{Collection->GetOutermost(),Definition->GetOutermost()};
    for (auto* AssetPackage:Packages) AssetPackage->MarkPackageDirty();
    if (!UEditorLoadingAndSavingUtils::SavePackages(Packages,true)) {Message=TEXT("Re-fractured assets could not all be saved.");return false;}
    Message=FString::Printf(TEXT("Re-fractured %s: %d transforms. Placements keep their reference."),*Definition->GetName(),Collection->GetGeometryCollection()->Transform.Num());
    return true;
}

bool URetakeDestructionConversion::CanConvertActor(AStaticMeshActor* Source,FString& Reason)
{
    if (!GEditor || GEditor->IsPlaySessionInProgress() || !IsValid(Source) || !Source->GetWorld() || Source->GetWorld()->WorldType!=EWorldType::Editor)
        Reason=TEXT("Select one static-mesh actor in the level editor with Play/Simulate stopped.");
    else if (FPackageName::IsTempPackage(Source->GetPackage()->GetName()))
        Reason=TEXT("Save the level first: the destructible's assets are created under its content root.");
    else if (Source->GetClass()!=AStaticMeshActor::StaticClass() || Source->GetComponents().Num()!=1)
        Reason=TEXT("Convert a plain static-mesh actor; Blueprint actors and additional components require manual integration.");
    else if (FLevelUtils::IsLevelLocked(Source->GetLevel()) || Source->GetAttachParentActor() || Source->IsLockLocation())
        Reason=TEXT("Unlock the level and detach this actor before converting it.");
    else if (!Source->GetDataLayerInstanceNames().IsEmpty()) Reason=TEXT("Convert an actor outside data layers, then assign the replacement to the required layers.");
    else if (!Source->GetStaticMeshComponent()->GetStaticMesh()) Reason=TEXT("The selected actor has no static mesh.");
    else
    {
        TArray<AActor*> Children;Source->GetAttachedActors(Children);
        const FVector Scale=Source->GetActorScale3D();
        if (!Children.IsEmpty()) Reason=TEXT("Detach child actors before converting this mesh.");
        else if (Scale.ContainsNaN() || Scale.GetMin()<=KINDA_SMALL_NUMBER) Reason=TEXT("The converter requires positive, nonzero scale; bake mirrored scale into the source mesh first.");
        else {Reason.Reset();return true;}
    }
    return false;
}

bool URetakeDestructionConversion::CanConvertDoorLeaf(ARetakeDoor* Door,FString& Reason)
{
    if (!GEditor || GEditor->IsPlaySessionInProgress() || !IsValid(Door) || !Door->GetWorld() || Door->GetWorld()->WorldType!=EWorldType::Editor)
        Reason=TEXT("Select one door in the level editor with Play/Simulate stopped.");
    else if (FPackageName::IsTempPackage(Door->GetPackage()->GetName()))
        Reason=TEXT("Save the level first: the destructible's assets are created under its content root.");
    else if (!Door->DoorMesh || !Door->DoorMesh->GetStaticMesh()) Reason=TEXT("This door has no leaf mesh to convert.");
    else if (Door->bStartLocked) Reason=TEXT("This door starts locked: a door with no gameplay keeps its own solid leaf and cannot be destroyed.");
    else
    {
        for (TActorIterator<ARetakeDestructiblePlacement> It(Door->GetWorld());It;++It)
            if (It->Door==Door) {Reason=It->GetActorLabel()+TEXT(" is already this door's leaf.");return false;}
        Reason.Reset();return true;
    }
    return false;
}

ARetakeDestructiblePlacement* URetakeDestructionConversion::ConvertDoorLeaf(ARetakeDoor* Door,const FRetakeDestructionConversionOptions& Options,FString& Message)
{
    if (!CanConvertDoorLeaf(Door,Message)) return nullptr;
    // The converter takes a plain static-mesh actor: stand one in for the leaf at its closed pose, convert it, then link the result.
    const UStaticMeshComponent* Leaf=Door->DoorMesh;
    FActorSpawnParameters Params;Params.ObjectFlags|=RF_Transactional;Params.OverrideLevel=Door->GetLevel();
    auto* Source=Door->GetWorld()->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(),Leaf->GetComponentTransform(),Params);
    if (!Source) {Message=TEXT("Could not stage the leaf for conversion.");return nullptr;}
    UStaticMeshComponent* Staged=Source->GetStaticMeshComponent();
    Staged->SetMobility(EComponentMobility::Movable);Staged->SetStaticMesh(Leaf->GetStaticMesh());
    for (int32 I=0;I<Leaf->OverrideMaterials.Num();++I) if (UMaterialInterface* Material=Leaf->OverrideMaterials[I]) Staged->SetMaterial(I,Material);
    Source->SetActorLabel(Door->GetActorLabel()+TEXT(" Leaf"));Source->SetFolderPath(Door->GetFolderPath());
    FRetakeDestructionConversionOptions DoorOptions=Options;
    DoorOptions.bUseWoodDoorDefaults=!Options.Preset;
    ARetakeDestructiblePlacement* Placement=ConvertActor(Source,DoorOptions,Message);
    if (!Placement) {if (IsValid(Source)) Source->GetWorld()->EditorDestroyActor(Source,true);return nullptr;}
    Placement->Modify();Placement->Door=Door;Placement->SetActorLabel(Door->GetActorLabel()+TEXT(" Leaf"));
    // The leaf hangs from a strip along its hinge edge, anchored as soon as the collection is configured (before physics runs).
    // Anchoring both ends would fix over half of a door-sized leaf and leave too little to break for the collapse.
    if (URetakeDestructionDefinition* Definition=Placement->Definition)
    {
        const FVector Scale=Leaf->GetComponentScale();
        const FBox Mesh=Leaf->GetStaticMesh()->GetBoundingBox();
        const FBox Local(Mesh.Min*Scale,Mesh.Max*Scale);
        const FVector Hinge=Placement->GetActorTransform().InverseTransformPosition(Door->GetHingeLocation());
        const int32 Long=Local.GetSize().X>=Local.GetSize().Y ? 0:1;
        FBox Strip=Local.ExpandBy(1.);
        if (FMath::Abs(Hinge[Long]-Local.Max[Long])<FMath::Abs(Hinge[Long]-Local.Min[Long])) Strip.Min[Long]=Local.Max[Long]-Placement->DoorEdgeCm;
        else Strip.Max[Long]=Local.Min[Long]+Placement->DoorEdgeCm;
        Definition->Modify();Definition->bAnchorEnds=false;Definition->bAnchorBase=false;Definition->AnchorBoxes={Strip};
        if (!Definition->PrepareCollection() || !UEditorLoadingAndSavingUtils::SavePackages({Definition->GetOutermost(),Definition->Collection->GetOutermost()},true))
            Message+=TEXT(" Warning: the leaf's hinge anchor could not be saved; anchor it before playing.");
    }
    Placement->RerunConstructionScripts();
    Message+=TEXT(" The placement is linked to ")+Door->GetActorLabel()+TEXT(" as its leaf.");
    return Placement;
}

ARetakeDestructiblePlacement* URetakeDestructionConversion::ConvertActor(AStaticMeshActor* Source,const FRetakeDestructionConversionOptions& Options,FString& Message)
{
    if (!CanConvertActor(Source,Message)) return nullptr;
    if (!IsValidRecipe(Options.Fracture,Message)) return nullptr;
    if (Options.AssetName.IsEmpty() || Options.AssetName.Len()>48 || ObjectTools::SanitizeObjectName(Options.AssetName)!=Options.AssetName
        || !FMath::IsFinite(Options.AnchorDepthCm) || Options.AnchorDepthCm<=0)
    {Message=TEXT("Use a valid asset name (up to 48 characters) and a positive anchor depth.");return nullptr;}
    const FConversionTarget Target=ConversionTarget(*Source);
    if (Options.Preset && !Options.Preset->GetPathName().StartsWith(Target.Reusable))
    {Message=TEXT("Choose a definition preset from ")+Target.Reusable+TEXT(".");return nullptr;}
    if (HasSceneReferences(Source,Message)) return nullptr;
    UStaticMeshComponent* Component=Source->GetStaticMeshComponent();UStaticMesh* OriginalMesh=Component->GetStaticMesh();
    FStaticMeshCompilingManager::Get().FinishCompilation({OriginalMesh});
    const FMeshDescription* Description=OriginalMesh->GetMeshDescription(0);
    if (!Description || Description->Triangles().Num()<4 || Description->Triangles().Num()>250000)
    {Message=TEXT("Use an editable closed source mesh with 4–250,000 triangles. Reduce dense meshes before fracturing.");return nullptr;}
    for (const auto& LOD:Component->LODData) if (LOD.OverrideVertexColors)
    {Message=TEXT("Bake per-instance vertex paint into a source mesh before converting it.");return nullptr;}
    if (!Component->GetCustomPrimitiveData().Data.IsEmpty())
    {Message=TEXT("Bake per-instance custom primitive data into material instances before converting it.");return nullptr;}
    const FTransform OriginalTransform=Component->GetComponentTransform();
    const FVector Scale=OriginalTransform.GetScale3D();
    FScopedSlowTask Progress(5,FText::FromString(TEXT("Creating Retake destructible")));
    if (!IsRunningCommandlet()) Progress.MakeDialog();
    IAssetTools& Tools=FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    FString CollectionPackage,CollectionName,Role;
    IAssetRegistry& Registry=FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    for (int32 Attempt=0;Attempt<1000;++Attempt)
    {
        const FString Suffix=Attempt ? FString::FromInt(Attempt):FString();
        Tools.CreateUniqueAssetName(Target.Folder/TEXT("Generated")/(TEXT("GC_Generated_")+Options.AssetName+Suffix),TEXT(""),CollectionPackage,CollectionName);
        Role=CollectionName;Role.RemoveFromStart(TEXT("GC_"));
        TArray<FAssetData> ExistingSources;Registry.GetAssetsByPath(FName(*(Target.Folder/TEXT("Sources")/Role)),ExistingSources,true);
        if (ExistingSources.IsEmpty()) break;
        if (Attempt==999) {Message=TEXT("Choose another asset name; existing conversion assets occupy this name range.");return nullptr;}
    }
    const FString DefinitionPackage=Target.Folder/TEXT("Definitions/Generated")/(TEXT("DA_Destruction_")+Role);
    const FString SourceFolder=Target.Folder/TEXT("Sources")/Role;
    auto Fail=[&](const FString& Error)->ARetakeDestructiblePlacement*
    {Message=Error+TEXT(" Original actor retained. Any generated assets are under ")+SourceFolder+TEXT(" and ")+CollectionPackage;return nullptr;};
    TArray<UMaterialInterface*> Materials;for (int32 I=0;I<Component->GetNumMaterials();++I) Materials.Add(Component->GetMaterial(I));
    if (Materials.IsEmpty()) Materials.Add(UMaterial::GetDefaultMaterial(MD_Surface));
    Progress.EnterProgressFrame(1,FText::FromString(TEXT("Copying source mesh and required materials")));
    UPhysicalMaterial* PhysicsMaterial=Options.Preset ? Options.Preset->PhysicalMaterial.Get():Component->BodyInstance.GetSimplePhysicalMaterial();
    TMap<FString,FString> Copies;if (!CopySources(OriginalMesh,Materials,Options.InteriorMaterial,PhysicsMaterial,Role,Target,Copies,Message)) return Fail(Message);
    UStaticMesh* Mesh=Cast<UStaticMesh>(Remapped(OriginalMesh,Copies));if (!Mesh) return Fail(TEXT("The source copy could not be loaded."));
    FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});Mesh->Modify();
    for (int32 I=0;I<Materials.Num();++I) Mesh->SetMaterial(I,Cast<UMaterialInterface>(Remapped(Materials[I],Copies)));
    const int32 InteriorIndex=Options.InteriorMaterial ? Materials.Num():0;
    if (Options.InteriorMaterial) Mesh->SetMaterial(InteriorIndex,Cast<UMaterialInterface>(Remapped(Options.InteriorMaterial,Copies)));
    Progress.EnterProgressFrame(1,FText::FromString(TEXT("Creating editable fracture graph")));
    FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("GeometryCollectionNodes"));
    const FString EmptyTemplate;
    auto* Collection=Cast<UGeometryCollection>(UE::DataflowAssetDefinitionHelpers::FactoryCreateNew(UGeometryCollection::StaticClass(),
        CreatePackage(*CollectionPackage),*CollectionName,RF_Public|RF_Standalone|RF_Transactional,&EmptyTemplate,true,nullptr));
    if (!Collection) return Fail(TEXT("Could not create a collection."));
    FAssetRegistryModule::AssetCreated(Collection);
    UDataflow* Graph=Collection->GetDataflowInstance().GetDataflowAsset();if (!Graph) return Fail(TEXT("Could not create the embedded Dataflow graph."));
    PRAGMA_DISABLE_INTERNAL_WARNINGS
    auto* Input=UE::Dataflow::FEditAssetUtils::AddNewNode(Graph,FVector2D(0,0),TEXT("RetakeSource"),TEXT("FStaticMeshToCollectionDataflowNode_v2"),nullptr);
    auto* Terminal=UE::Dataflow::FEditAssetUtils::AddNewNode(Graph,FVector2D(700,0),TEXT("RetakeTerminal"),TEXT("FGeometryCollectionTerminalDataflowNode_v2"),nullptr);
    PRAGMA_ENABLE_INTERNAL_WARNINGS
    const FTransform BakeTransform(FQuat::Identity,FVector::ZeroVector,Scale);FString TransformText;
    TBaseStructure<FTransform>::Get()->ExportText(TransformText,&BakeTransform,nullptr,nullptr,PPF_None,nullptr);
    if (!SetNodeProperty(Input,TEXT("StaticMesh"),Mesh->GetPathName()) || !SetNodeProperty(Input,TEXT("MeshTransform"),TransformText)
        || !SetNodeProperty(Input,TEXT("bSetInternalFromMaterialIndex"),TEXT("False"))) return Fail(TEXT("Could not configure the fracture graph."));
    if (!ConfigureFractureGraph(Graph,Input,Terminal,Options.Fracture,InteriorIndex,Message)) return Fail(Message);
    Collection->GetDataflowInstance().SetDataflowTerminal(TEXT("RetakeTerminal"));
    Progress.EnterProgressFrame(1,FText::FromString(TEXT("Fracturing and preparing collision")));
    // This UE6 implementation returns bHasError despite the API comment claiming success.
    if (UDataflowBlueprintLibrary::EvaluateDataflow(Graph,Collection) || !Collection->GetGeometryCollection()
        || Collection->GetGeometryCollection()->Transform.Num()<3) return Fail(TEXT("The mesh could not be fractured into usable pieces."));
    Collection->EnableNanite=Mesh->GetNaniteSettings().bEnabled;
    ApplyCollectionSettings(Collection,Options.Preset);
    auto* Package=CreatePackage(*DefinitionPackage);const FName DefinitionName(*FPackageName::GetLongPackageAssetName(DefinitionPackage));
    auto* Definition=Options.Preset ? DuplicateObject<URetakeDestructionDefinition>(Options.Preset,Package,DefinitionName):
        NewObject<URetakeDestructionDefinition>(Package,DefinitionName,RF_Public|RF_Standalone|RF_Transactional);
    Definition->SetFlags(RF_Public|RF_Standalone|RF_Transactional);FAssetRegistryModule::AssetCreated(Definition);
    Definition->Collection=Collection;Definition->FrameMesh=nullptr;
    Definition->Materials.Reset();Definition->BreakMaterialIndices.Reset();Definition->BreakMaterials.Reset();Definition->AnchorBoxes.Reset();
    // New geometry has no imported per-piece removal contract of its own; a reference-response preset supplied one above.
    Definition->bUseAuthoredResponse=Options.Preset && Options.Preset->bUseAuthoredResponse && Options.Preset->Collection;
    Definition->bAnchorBase=Options.bAnchorBase;Definition->BaseAnchorDepthCm=Options.AnchorDepthCm;
    Definition->bAnchorEnds=Options.bAnchorEnds;Definition->EndAnchorDepthCm=Options.AnchorDepthCm;
    Definition->PreparedCollectionGuid.Invalidate();
    Definition->PhysicalMaterial=Cast<UPhysicalMaterial>(Remapped(PhysicsMaterial,Copies));
    if (Options.bUseWoodDoorDefaults && !Options.Preset)
    {
        // Each conversion owns this material alongside its copied source. There is no
        // private-game preset dependency and an explicit creator preset is never changed.
        FString WoodPackage,WoodName;
        Tools.CreateUniqueAssetName(SourceFolder/TEXT("PM_Wood"),TEXT(""),WoodPackage,WoodName);
        auto* Wood=NewObject<UPhysicalMaterial>(CreatePackage(*WoodPackage),*WoodName,RF_Public|RF_Standalone|RF_Transactional);
        Wood->SurfaceType=SurfaceType5; // Canonical Wood in the SDK's structural profile.
        FAssetRegistryModule::AssetCreated(Wood);
        Wood->MarkPackageDirty();
        Definition->PhysicalMaterial=Wood;
    }
    Collection->PhysicsMaterial=Definition->PhysicalMaterial;
    Progress.EnterProgressFrame(1,FText::FromString(TEXT("Preparing the collection and saving assets")));
    if (!Definition->PrepareCollection()) return Fail(TEXT("Collection preparation failed."));
    FDataValidationContext Validation;
    if (Definition->IsDataValid(Validation)!=EDataValidationResult::Valid) return Fail(TEXT("The generated definition failed validation."));
    TArray<UPackage*> Packages{Collection->GetOutermost(),Definition->GetOutermost()};
    if (Options.bUseWoodDoorDefaults && !Options.Preset && Definition->PhysicalMaterial)
        Packages.AddUnique(Definition->PhysicalMaterial->GetOutermost());
    for (const auto& Pair:Copies) if (UPackage* Copied=FindPackage(nullptr,*Pair.Value)) Packages.AddUnique(Copied);
    for (auto* AssetPackage:Packages) AssetPackage->MarkPackageDirty();
    if (!UEditorLoadingAndSavingUtils::SavePackages(Packages,true)) return Fail(TEXT("Generated assets could not all be saved."));
    FString Recheck;
    if (!CanConvertActor(Source,Recheck) || !Source->GetActorTransform().Equals(OriginalTransform,.001))
        return Fail(TEXT("The source actor changed during conversion. Inspect the generated assets before replacing it."));
    Progress.EnterProgressFrame(1,FText::FromString(TEXT("Replacing selected actor")));
    const FScopedTransaction Transaction(FText::FromString(TEXT("Convert mesh to Retake destructible")));
    Source->Modify();Source->GetLevel()->Modify();
    const FTransform PlacementTransform(OriginalTransform.GetRotation(),OriginalTransform.GetLocation(),FVector::OneVector);
    auto* Placement=Cast<ARetakeDestructiblePlacement>(GEditor->AddActor(Source->GetLevel(),ARetakeDestructiblePlacement::StaticClass(),PlacementTransform,true,RF_Transactional,false));
    if (!Placement) return Fail(TEXT("Could not place the generated destructible."));
    Placement->Modify();Placement->Definition=Definition;Placement->SetActorLabel(TEXT("Destructible ")+Options.AssetName);
    Placement->SetFolderPath(Source->GetFolderPath());for (FName Tag:Source->Tags) Placement->Tags.AddUnique(Tag);
    Placement->RerunConstructionScripts();
    if (!Source->GetWorld()->EditorDestroyActor(Source,true)) {Placement->GetWorld()->EditorDestroyActor(Placement,true);return Fail(TEXT("Could not replace the original actor."));}
    // Conversion is also used by isolated authoring commandlets, which have no Slate application or browser.
    if (!IsRunningCommandlet())
    {
        GEditor->SelectNone(false,true);GEditor->SelectActor(Placement,true,true);GEditor->NoteSelectionChange();
        GEditor->SyncBrowserToObjects(TArray<UObject*>{Definition,Collection});
        if (Options.bOpenFractureEditor) GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Collection);
    }
    Message=TEXT("Converted. Assets saved under ")+Target.Folder+TEXT(". Save the level to keep the placement; Undo restores the original actor. Scale is baked into the collection.");
    return Placement;
}
