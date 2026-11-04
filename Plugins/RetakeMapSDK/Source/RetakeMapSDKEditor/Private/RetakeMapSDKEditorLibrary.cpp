// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeMapSDKEditorLibrary.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "RetakeWeaponPack.h"
#include "RetakeMapSDKManifest.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraDataInterfaceArrayFloat.h"
#include "RetakeWeaponData.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/App.h"
#include "Editor.h"
#include "ScopedTransaction.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Misc/FrameRate.h"
#include "MovieScene.h"
#include "MovieSceneSequence.h"
#include "Hash/Blake3.h"
#include "RetakeWeaponPose.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "RetakeWeaponPoseBasis.h"
#include "RetakeWeaponPoseBaking.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "ReferenceSkeleton.h"
#include "SkeletalMeshAttributes.h"
#include "MeshDescription.h"
#include "RenderingThread.h"
#include "AssetCompilingManager.h"
#include "UObject/StrongObjectPtr.h"

FString URetakeMapSDKEditorLibrary::GetWeaponAuthoringBodyIdentity(URetakeWeaponPoseBasis* Basis)
{
    return Basis ? Basis->Body.ComputeIdentity() : FString();
}

FString URetakeMapSDKEditorLibrary::NormalizeWeaponAuthoringReference(URetakeWeaponPoseBasis* Basis,const FString& OwnerModId)
{
    if (!IsInGameThread() || !GEditor || GEditor->PlayWorld || !Basis || !Basis->ReferenceMesh
        || !FRetakeModManifest::IsValidModId(OwnerModId)) return TEXT("Select an owned authoring basis with PIE stopped.");
    USkeletalMesh* Mesh=Basis->ReferenceMesh;
    USkeleton* Skeleton=Mesh->GetSkeleton();
    for (const UObject* Object:{static_cast<const UObject*>(Basis),static_cast<const UObject*>(Mesh),static_cast<const UObject*>(Skeleton)})
        if (!Object || !URetakeWeaponPack::IsOwnedObjectPath(OwnerModId,FSoftObjectPath(Object)))
            return TEXT("The basis, reference mesh and animation skeleton must all belong to the selected mod.");
    FString Error;
    if (!Basis->Body.Validate(Error)) return Error;
    if (Mesh->GetLODNum()!=1) return TEXT("Normalize a single-LOD authoring reference before generating additional LODs.");
    FAssetCompilingManager::Get().FinishCompilationForObjects({Mesh});
    FMeshDescription* Description=Mesh->GetMeshDescription(0);
    if (!Description) return TEXT("The reference mesh needs editable mesh-description source data.");
    FSkeletalMeshAttributes Attributes(*Description);
    if (!Attributes.HasBoneNameAttribute() || !Attributes.HasBonePoseAttribute())
        return TEXT("The reference mesh has no editable bone names/poses.");
    FReferenceSkeleton MeshReference=Mesh->GetRefSkeleton(),AnimationReference=Skeleton->GetReferenceSkeleton();
    const auto Prepare=[&Error](FReferenceSkeleton& Reference,TConstArrayView<FRetakePoseBone> Expected,USkeleton* Owner,const TCHAR* Label)->bool
    {
        TArray<int32> Indices;
        for (int32 I=0;I<Expected.Num();++I)
        {
            const auto& Bone=Expected[I];const int32 Index=Reference.FindBoneIndex(Bone.Name);
            if (Index==INDEX_NONE || Reference.GetParentIndex(Index)!=(Bone.Parent==INDEX_NONE?INDEX_NONE:Indices[Bone.Parent]))
            {Error=TEXT("Authoring reference topology differs at ")+Bone.Name.ToString();return false;}
            const FTransform& Actual=Reference.GetRefBonePose()[Index];const FTransform Wanted=Bone.Reference();
            // Imported/reference quaternions can differ slightly in norm. AngularDistance assumes unit
            // quaternions; compare normalized orientations without changing the published stored values.
            const double RotationDelta=Actual.GetRotation().GetNormalized().AngularDistance(Wanted.GetRotation().GetNormalized());
            if (!Actual.IsValid() || !Actual.GetTranslation().Equals(Wanted.GetTranslation(),.001)
                || !Actual.GetScale3D().Equals(Wanted.GetScale3D(),.00001)
                || RotationDelta>.0001)
            {
                Error=FString::Printf(TEXT("%s reference difference exceeds source-import rounding at %s (position %.9g cm, rotation %.9g radians, scale %.9g); use the matching authoring rig."),
                    Label,*Bone.Name.ToString(),(Actual.GetTranslation()-Wanted.GetTranslation()).GetAbsMax(),
                    RotationDelta,(Actual.GetScale3D()-Wanted.GetScale3D()).GetAbsMax());
                return false;
            }
            Indices.Add(Index);
        }
        FReferenceSkeletonModifier Modifier(Reference,Owner);
        for (int32 I=0;I<Expected.Num();++I) Modifier.UpdateRefPoseTransform(Indices[I],Expected[I].Reference());
        return true;
    };
    if (!Prepare(MeshReference,Basis->Body.Bones,Skeleton,TEXT("Mesh")) || !Prepare(AnimationReference,Basis->Body.AnimationBones,Skeleton,TEXT("Animation skeleton"))) return Error;
    TMap<FName,FBoneID> DescriptionBones;
    auto Names=Attributes.GetBoneNames();
    for (const FBoneID Id:Attributes.Bones().GetElementIDs()) DescriptionBones.Add(Names[Id],Id);
    for (const auto& Bone:Basis->Body.Bones)
        if (!DescriptionBones.Contains(Bone.Name)) return TEXT("Mesh description is missing ")+Bone.Name.ToString();

    const FReferenceSkeleton PreviousMeshReference=Mesh->GetRefSkeleton(),PreviousAnimationReference=Skeleton->GetReferenceSkeleton();
    const FMeshDescription PreviousDescription=*Description;
    const bool bMeshWasDirty=Mesh->GetOutermost()->IsDirty(),bSkeletonWasDirty=Skeleton->GetOutermost()->IsDirty();

    const FScopedTransaction Transaction(NSLOCTEXT("RetakeModKit","NormalizeReference","Normalize weapon authoring reference"));
    Skeleton->Modify();Mesh->Modify();Mesh->ModifyMeshDescription(0);
    auto Poses=Attributes.GetBonePoses();
    for (const auto& Bone:Basis->Body.Bones) Poses[DescriptionBones.FindChecked(Bone.Name)]=Bone.Reference();
    FlushRenderingCommands();
    Mesh->SetRefSkeleton(MeshReference);
    Mesh->GetRefBasesInvMatrix().Reset();Mesh->CalculateInvRefMatrices();
    Mesh->CommitMeshDescription(0);
    // A mesh and its animation skeleton can legitimately have distinct reference transforms.
    // Use the engine's public skeleton API with an ephemeral reference holder; no extra asset is created.
    TStrongObjectPtr<USkeletalMesh> AnimationReferenceHolder(NewObject<USkeletalMesh>(GetTransientPackage()));
    AnimationReferenceHolder->SetRefSkeleton(AnimationReference);
    AnimationReferenceHolder->SetSkeleton(Skeleton);
    Skeleton->UpdateReferencePoseFromMesh(AnimationReferenceHolder.Get());
    Skeleton->MarkPackageDirty();Mesh->PostEditChange();Mesh->MarkPackageDirty();
    FAssetCompilingManager::Get().FinishCompilationForObjects({Mesh});
    RetakeWeaponPoseBaking::FContext Context;
    if (!Context.Initialize(*Mesh,Basis->Body.Bones,Error) || !Basis->Body.MatchesAnimationSkeleton(*Skeleton,Error))
    {
        *Mesh->GetMeshDescription(0)=PreviousDescription;
        Mesh->SetRefSkeleton(PreviousMeshReference);Mesh->GetRefBasesInvMatrix().Reset();Mesh->CalculateInvRefMatrices();
        Mesh->CommitMeshDescription(0);
        AnimationReferenceHolder->SetRefSkeleton(PreviousAnimationReference);
        Skeleton->UpdateReferencePoseFromMesh(AnimationReferenceHolder.Get());
        Mesh->PostEditChange();FAssetCompilingManager::Get().FinishCompilationForObjects({Mesh});
        Mesh->GetOutermost()->SetDirtyFlag(bMeshWasDirty);Skeleton->GetOutermost()->SetDirtyFlag(bSkeletonWasDirty);
        return TEXT("Reference normalization was rolled back: ")+Error;
    }
    return FString();
}

FString URetakeMapSDKEditorLibrary::RetireWeaponPoseContribution(const FString& OwnerModId,const FString& ObjectPath)
{
    const FSoftObjectPath Path(ObjectPath);
    if (!IsInGameThread() || !GEditor || GEditor->PlayWorld || !FRetakeModManifest::IsValidModId(OwnerModId)
        || !URetakeWeaponPack::IsOwnedObjectPath(OwnerModId,Path))
        return TEXT("Select a canonical owned preparation asset with PIE stopped.");
    const auto Plugin=IPluginManager::Get().FindPlugin(OwnerModId);
    FString Filename;
    if (!Plugin || !Plugin->IsEnabled() || !FPackageName::DoesPackageExist(Path.GetLongPackageName(),&Filename)
        || !FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Filename),FPaths::ConvertRelativePathToFull(Plugin->GetContentDir())))
        return TEXT("The preparation package is missing or outside its owning content plugin.");
    auto& Registry=FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    Registry.SearchAllAssets(true);
    TArray<FAssetIdentifier> Referencers;
    Registry.GetReferencers(FAssetIdentifier(Path.GetLongPackageFName()),Referencers,UE::AssetRegistry::EDependencyCategory::All);
    for (const auto& Ref:Referencers)
        if (Ref.PackageName!=Path.GetLongPackageFName()) return TEXT("Preparation is still referenced by ")+Ref.ToString();
    const FAssetData Asset=Registry.GetAssetByObjectPath(Path);
    if (!Asset.IsValid() || Asset.AssetClassPath!=URetakeWeaponPoseContribution::StaticClass()->GetClassPathName())
        return TEXT("Only native weapon pose contribution assets can be retired.");
    // ForceDeleteObjects can report an object deletion without retaining its package for cleanup.
    // Normal deletion performs its own live-reference check and removes the package through editor APIs.
    if (ObjectTools::DeleteAssets({Asset},false)!=1 || IFileManager::Get().FileExists(*Filename))
        return TEXT("The editor did not remove the preparation package; inspect live references and source-control state.");
    return {};
}

UActorComponent* URetakeMapSDKEditorLibrary::AddInstanceComponent(AActor* Actor, TSubclassOf<UActorComponent> Class, FName Name, USceneComponent* Parent)
{
    const UWorld* World = Actor ? Actor->GetWorld() : nullptr;
    if (!World || World->WorldType != EWorldType::Editor || !Class || Class->HasAnyClassFlags(CLASS_Abstract) || Name.IsNone()
        || StaticFindObjectFast(nullptr, Actor, Name)) return nullptr;
    Actor->Modify();
    UActorComponent* Component = NewObject<UActorComponent>(Actor, Class, Name, RF_Transactional);
    if (USceneComponent* Scene = Cast<USceneComponent>(Component))
    {
        if (USceneComponent* AttachTo = Parent ? Parent : Actor->GetRootComponent()) Scene->SetupAttachment(AttachTo);
        else Actor->SetRootComponent(Scene);
    }
    Component->CreationMethod = EComponentCreationMethod::Instance;
    Actor->AddInstanceComponent(Component);
    Component->OnComponentCreated();
    Component->RegisterComponent();
    return Component;
}

namespace
{
FString AnimationFrameHash(const IAnimationDataModel& Model)
{
    TArray<FName> Names;Model.GetBoneTrackNames(Names);
    const int32 Frames=Model.GetNumberOfFrames();
    if (Names.IsEmpty() || Frames<1 || int64(Frames+1)*Names.Num()>1000000) return FString();
    Names.Sort([](FName A,FName B){return A.LexicalLess(B);});
    FBlake3 Hash;
    const int32 Counts[]={Frames,Names.Num()};Hash.Update(Counts,sizeof(Counts));
    for (FName Name:Names)
    {
        const FTCHARToUTF8 Text(*Name.ToString().ToLower());
        const int32 Length=Text.Length();Hash.Update(&Length,sizeof(Length));Hash.Update(Text.Get(),Length);
        for (int32 Frame=0;Frame<=Frames;++Frame)
        {
            const FTransform Pose=Model.GetBoneTrackTransform(Name,FFrameNumber(Frame));
            if (Pose.ContainsNaN()) return FString();
            const FVector T=Pose.GetTranslation(),S=Pose.GetScale3D();const FQuat Q=Pose.GetRotation();
            const double Values[]={T.X,T.Y,T.Z,Q.X,Q.Y,Q.Z,Q.W,S.X,S.Y,S.Z};
            Hash.Update(Values,sizeof(Values));
        }
    }
    return LexToString(Hash.Finalize());
}
}

FString URetakeMapSDKEditorLibrary::InspectAnimationFrames(UAnimSequence* Animation)
{
    auto Report=MakeShared<FJsonObject>();
    if (!IsInGameThread() || !Animation || (GEditor && GEditor->PlayWorld) || !Animation->GetDataModel())
        Report->SetStringField(TEXT("error"),TEXT("Inspect a loaded animation with a data model on the editor game thread with PIE stopped."));
    else
    {
        const auto& Model=*Animation->GetDataModel();const FFrameRate Rate=Model.GetFrameRate();
        const FString Hash=AnimationFrameHash(Model);
        if (Hash.IsEmpty())Report->SetStringField(TEXT("error"),TEXT("Invalid or excessive raw frame data."));
        Report->SetStringField(TEXT("asset"),Animation->GetPathName());
        Report->SetStringField(TEXT("algorithm"),TEXT("blake3-native-local-frame-poses-f64-v1"));
        Report->SetStringField(TEXT("framePoseHash"),Hash);
        Report->SetNumberField(TEXT("frames"),Model.GetNumberOfFrames());
        Report->SetNumberField(TEXT("keys"),Model.GetNumberOfKeys());
        Report->SetNumberField(TEXT("tracks"),Model.GetNumBoneTracks());
        Report->SetNumberField(TEXT("rateNumerator"),Rate.Numerator);
        Report->SetNumberField(TEXT("rateDenominator"),Rate.Denominator);
        Report->SetNumberField(TEXT("modelLength"),Model.GetPlayLength());
        Report->SetNumberField(TEXT("cachedLength"),Animation->GetPlayLength());
        Report->SetNumberField(TEXT("attributes"),Model.GetNumberOfAttributes());
        Report->SetNumberField(TEXT("floatCurves"),Model.GetNumberOfFloatCurves());
        Report->SetNumberField(TEXT("transformCurves"),Model.GetNumberOfTransformCurves());
        Report->SetBoolField(TEXT("hasNotifies"),Animation->IsNotifyAvailable());
        Report->SetNumberField(TEXT("syncMarkers"),Animation->AuthoredSyncMarkers.Num());
    }
    FString Json;FJsonSerializer::Serialize(Report,TJsonWriterFactory<>::Create(&Json));return Json;
}

bool URetakeMapSDKEditorLibrary::RetimeAnimationFrames(UAnimSequence* Animation,const FString& OwnerModId,
    int32 RateNumerator,int32 RateDenominator,FString& OutError)
{
    OutError.Reset();
    const FFrameRate Rate(RateNumerator,RateDenominator);
    if (!IsInGameThread() || !Animation || (GEditor && GEditor->PlayWorld)
        || !FRetakeModManifest::IsValidModId(OwnerModId)
        || !URetakeWeaponPack::IsOwnedObjectPath(OwnerModId,FSoftObjectPath(Animation))
        || Animation->GetOutermost()->IsDirty() || RateNumerator<=0 || RateDenominator<=0
        || !FMath::IsFinite(Rate.AsDecimal()) || Rate.AsDecimal()<1. || Rate.AsDecimal()>480.)
    { OutError=TEXT("Select a saved mod-owned animation and a sample rate from 1 to 480 fps with PIE stopped.");return false; }
    const auto Model=Animation->GetDataModelInterface();
    auto* Sequence=Model.GetObject() ? Cast<UMovieSceneSequence>(Model.GetObject()):nullptr;
    UMovieScene* Scene=Sequence ? Sequence->GetMovieScene():nullptr;
    if (!Model || !Scene || !Model->HasBeenPopulated() || Model->GetNumBoneTracks()==0
        || Model->GetNumberOfFrames()<1 || Model->GetNumberOfFrames()>100000
        || Model->GetNumberOfKeys()!=Model->GetNumberOfFrames()+1
        || Scene->GetTickResolution()!=Model->GetFrameRate())
    { OutError=TEXT("This operation requires a populated sequence-backed model with matching tick and sample rates.");return false; }
    if (Animation->IsValidAdditive() || Animation->IsNotifyAvailable() || !Animation->AuthoredSyncMarkers.IsEmpty()
        || Model->GetNumberOfFloatCurves() || Model->GetNumberOfTransformCurves() || Model->GetNumberOfAttributes())
    { OutError=TEXT("Retiming requires a raw clip without notifies, sync markers, curves, attributes or additive behavior.");return false; }
    const FString PreviousPoses=AnimationFrameHash(*Model);
    if (PreviousPoses.IsEmpty()) {OutError=TEXT("Invalid or excessive raw frame data.");return false;}
    const FFrameRate PreviousRate=Model->GetFrameRate();
    if (Rate==PreviousRate) return true;
    const int32 Frames=Model->GetNumberOfFrames(),Keys=Model->GetNumberOfKeys();
    const FScopedTransaction Transaction(NSLOCTEXT("RetakeModKit","RetimeAnimationFrames","Retime mod animation frame keys"));
    Animation->Modify();Model.GetObject()->Modify();Scene->Modify();
    // Both rates change directly: key frame numbers and the frame range stay untouched.
    // The ordinary SetFrameRate controller operation preserves seconds and resizes the range.
    Scene->SetDisplayRate(Rate);Scene->SetTickResolutionDirectly(Rate);
    Animation->GetController().NotifyPopulated(); // Refresh cached length, source identity and compression through the native model notification.
    if (Model->GetNumberOfFrames()!=Frames || Model->GetNumberOfKeys()!=Keys || AnimationFrameHash(*Model)!=PreviousPoses
        || !FMath::IsNearlyEqual(Model->GetPlayLength(),Rate.AsSeconds(FFrameNumber(Frames)),1.e-8))
    {
        Scene->SetDisplayRate(PreviousRate);Scene->SetTickResolutionDirectly(PreviousRate);
        Animation->GetController().NotifyPopulated();
        OutError=TEXT("Retiming could not preserve every source frame pose and the frame range. Previous rates restored; do not save this failed operation.");
        return false;
    }
    Animation->MarkPackageDirty();
    return true;
}

FString URetakeMapSDKEditorLibrary::ConfigureSmokeEmissionInterface(UNiagaraSystem* System,const FString& OwnerModId)
{
    if (!IsInGameThread() || !System || (GEditor && GEditor->PlayWorld) || !FRetakeModManifest::IsValidModId(OwnerModId)
        || !URetakeWeaponPack::IsOwnedObjectPath(OwnerModId,FSoftObjectPath(System)))
        return TEXT("Select an owned Niagara system on the editor game thread with PIE stopped.");
    auto& Parameters=System->GetExposedParameters();
    const FNiagaraVariable Origins(FNiagaraTypeDefinition(UNiagaraDataInterfaceArrayFloat3::StaticClass()),TEXT("User.EmissionOriginOffsets"));
    const FNiagaraVariable Version(FNiagaraTypeDefinition::GetIntDef(),TEXT("User.EmissionHistoryVersion"));
    const FNiagaraVariable Count(FNiagaraTypeDefinition::GetIntDef(),TEXT("User.InitialPuffCount"));
    TArray<FNiagaraVariable> Existing;Parameters.GetParameters(Existing);
    for (const auto& Expected:{Origins,Version,Count})
        for (const auto& Variable:Existing)
            if (Variable.GetName()==Expected.GetName() && Variable.GetType()!=Expected.GetType())
                return TEXT("An existing smoke interface parameter has an incompatible type: ")+Expected.GetName().ToString();
    const FScopedTransaction Transaction(FText::FromString(TEXT("Configure smoke emission history")));
    System->Modify();
    if (Parameters.IndexOf(Origins)==INDEX_NONE) Parameters.AddParameter(Origins);
    auto* Array=Cast<UNiagaraDataInterfaceArrayFloat3>(Parameters.GetDataInterface(Origins));
    if (!Array || Array->GetOuter()!=System)
    {
        Array=NewObject<UNiagaraDataInterfaceArrayFloat3>(System,NAME_None,RF_Transactional);
        Parameters.SetDataInterface(Array,Origins);
    }
    Array->Modify();Array->FloatData={FVector::ZeroVector};Array->PostEditChange();
    Parameters.SetParameterValue<int32>(1,Version,true);
    if (Parameters.IndexOf(Count)==INDEX_NONE) Parameters.SetParameterValue<int32>(6,Count,true);
    System->PostEditChange();System->MarkPackageDirty();
    FRetakeSmokeRules Rules;Rules.bTrackEmissionOrigins=true;
    FString Error;
    Rules.ValidateEmissionPresentation(System,Error);
    return Error;
}

FString URetakeMapSDKEditorLibrary::InspectNiagaraSystem(UNiagaraSystem* System)
{
    const TSharedRef<FJsonObject> Report=MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("scope"),TEXT("Editor compiled-data inspection; no simulation, package or performance acceptance"));
    if (!IsInGameThread() || !System || (GEditor && GEditor->PlayWorld))
        Report->SetStringField(TEXT("error"),TEXT("Inspect a loaded system on the editor game thread with PIE stopped."));
    else
    {
        System->EnsureFullyLoaded();
        if (System->NeedsRequestCompile()) System->RequestCompile(false);
        System->WaitForCompilationComplete(true,false);
        const UNiagaraScript* Spawn=System->GetSystemSpawnScript();
        const UNiagaraScript* Update=System->GetSystemUpdateScript();
        Report->SetStringField(TEXT("system"),System->GetPathName());
        Report->SetBoolField(TEXT("valid"),System->IsValid());
        Report->SetBoolField(TEXT("readyToRun"),System->IsReadyToRun());
        Report->SetBoolField(TEXT("renderingAvailable"),FApp::CanEverRender());
        Report->SetBoolField(TEXT("systemDataSetsMatch"),Spawn && Update && Spawn->GetVMExecutableData().Attributes==Update->GetVMExecutableData().Attributes);
        Report->SetNumberField(TEXT("spawnAttributeCount"),Spawn ? Spawn->GetVMExecutableData().Attributes.Num():0);
        Report->SetNumberField(TEXT("updateAttributeCount"),Update ? Update->GetVMExecutableData().Attributes.Num():0);
        TArray<TSharedPtr<FJsonValue>> Emitters;
        for (const FNiagaraEmitterHandle& Handle:System->GetEmitterHandles())
        {
            const TSharedRef<FJsonObject> Entry=MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"),Handle.GetName().ToString());
            Entry->SetBoolField(TEXT("enabled"),Handle.GetIsEnabled());
            if (const FVersionedNiagaraEmitterData* Data=Handle.GetEmitterData())
            {
                Entry->SetBoolField(TEXT("readyToRun"),Data->IsReadyToRun());
                const FVersionedNiagaraEmitter Parent=Data->GetParent();
                Entry->SetStringField(TEXT("parent"),Parent.Emitter ? Parent.Emitter->GetPathName():FString());
                const FVersionedNiagaraEmitter Previous=Data->GetParentAtLastMerge();
                Entry->SetStringField(TEXT("parentAtLastMerge"),Previous.Emitter ? Previous.Emitter->GetPathName():FString());
            }
            Emitters.Add(MakeShared<FJsonValueObject>(Entry));
        }
        Report->SetArrayField(TEXT("emitters"),Emitters);
    }
    FString Json;
    FJsonSerializer::Serialize(Report,TJsonWriterFactory<>::Create(&Json));
    return Json;
}

bool URetakeMapSDKEditorLibrary::DetachNiagaraEmitterParents(UNiagaraSystem* System,const FString& OwnerModId,FString& OutError)
{
    OutError.Reset();
    if (!IsInGameThread() || !System || (GEditor && GEditor->PlayWorld)
        || !FRetakeModManifest::IsValidModId(OwnerModId)
        || !URetakeWeaponPack::IsOwnedObjectPath(OwnerModId,FSoftObjectPath(System)))
    { OutError=TEXT("Select a system owned by the specified mod on the editor game thread with PIE stopped.");return false; }
    System->EnsureFullyLoaded();
    // Validate all embedded emitters before changing any parent. Never modify a shared external emitter.
    for (const FNiagaraEmitterHandle& Handle:System->GetEmitterHandles())
        if (const UNiagaraEmitter* Emitter=Handle.GetInstance().Emitter;Emitter && !Emitter->IsIn(System))
        { OutError=TEXT("An emitter is not embedded in the selected system.");return false; }
    const FScopedTransaction Transaction(NSLOCTEXT("RetakeModKit","DetachEffectParents","Detach mod effect emitter parents"));
    for (FNiagaraEmitterHandle& Handle:System->GetEmitterHandles())
    {
        FVersionedNiagaraEmitterData* Data=Handle.GetEmitterData();
        if (!Data) continue;
        if (!Data->GetParent().Emitter && !Data->GetParentAtLastMerge().Emitter) continue;
        System->Modify();
        Handle.GetInstance().Emitter->Modify();
        // Same operation as Niagara's Remove Parent Emitter editor action; merged modules remain intact.
        Data->RemoveParent();
        System->MarkPackageDirty();
    }
    return true;
}
