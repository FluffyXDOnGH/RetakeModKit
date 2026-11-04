// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeAcousticMap.h"
#include "RetakeDoor.h"
#include "RetakeMapElementSubsystem.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "PhysicsEngine/BodySetup.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/DataValidation.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Net/UnrealNetwork.h"
#include "Serialization/MemoryWriter.h"
#include "UObject/Package.h"
#if WITH_EDITOR
#include "StaticMeshCompiler.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogRetakeAcousticMap, Log, All);
const FName ARetakeAcousticConnection::StateChannel(TEXT("Acoustics.Connection"));

namespace
{
    UBoxComponent* CreateBounds(AActor& Owner)
    {
        auto* Bounds = Owner.CreateDefaultSubobject<UBoxComponent>(TEXT("AcousticBounds"));
        Owner.SetRootComponent(Bounds);
        Bounds->SetBoxExtent(FVector(100));
        Bounds->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Bounds->SetGenerateOverlapEvents(false);
        Bounds->SetHiddenInGame(true);
        return Bounds;
    }

    // A signed zero as +0.0, decided on the bits. Cooked data can carry -0.0 where the editor holds +0.0 (Outpost's
    // container_45 roll in the alpha.30 Win64 client), and `Value + 0.0` does not fold it: fast floating-point math
    // (MSVC /fp:fast in Win64 game builds) removes the addition as an identity, while the Linux server kept it.
    double CanonicalZero(double Value)
    {
        uint64 Bits; FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
        return (Bits << 1) == 0 ? 0.0 : Value;
    }

    // Placement as the exact bits of the serialized relative transforms of a component and each attach ancestor, the
    // method of the visibility source manifest (v3). World transforms are derived through sin/cos and quaternion
    // products whose last bits differ between the Win64 editor and the cooked Linux server (alpha.30 smoke test: a
    // rotated region on Outpost and obj_team1, 130 Stalingrad brushes at -90 yaw), so they are never hashed.
    // Names are lowercased because FName display case is not stable across cooking.
    FString PlacementBits(const USceneComponent* Component)
    {
        static_assert(PLATFORM_LITTLE_ENDIAN, "Acoustic placement hashes little-endian double bits.");
        FString Result;
        for (const USceneComponent* It = Component; It; It = It->GetAttachParent())
        {
            const FVector L = It->GetRelativeLocation(), S = It->GetRelativeScale3D(); const FRotator R = It->GetRelativeRotation();
            double Values[9] = {L.X, L.Y, L.Z, R.Pitch, R.Yaw, R.Roll, S.X, S.Y, S.Z};
            for (double& Value : Values) Value = CanonicalZero(Value);
            const int32 Absolute = (It->IsUsingAbsoluteLocation() ? 1 : 0) | (It->IsUsingAbsoluteRotation() ? 2 : 0) | (It->IsUsingAbsoluteScale() ? 4 : 0);
            if (It != Component) Result += TEXT("<") + (It->GetOwner() ? It->GetOwner()->GetName() + TEXT("/") : FString()) + It->GetName() + TEXT(">");
            Result += BytesToHex(reinterpret_cast<const uint8*>(Values), static_cast<int32>(sizeof(Values)));
            Result += FString::Printf(TEXT(":%d:%s;"), Absolute, *It->GetAttachSocketName().ToString());
        }
        return Result.ToLower();
    }

    FString MatrixBits(const FMatrix& Matrix)
    {
        double Values[16];
        for (int32 Index = 0; Index < 16; ++Index) Values[Index] = CanonicalZero(Matrix.M[Index / 4][Index % 4]);
        return BytesToHex(reinterpret_cast<const uint8*>(Values), static_cast<int32>(sizeof(Values))).ToLower();
    }

    // The RetakeAcoustics response the runtime traces against. A component with bUseDefaultCollision takes its mesh's
    // collision in OnRegister, which runs after InitGame (UEngine::LoadMap calls SetGameMode before
    // InitializeActorsForPlay), so its own saved responses are never what plays; read the mesh's. Reading the component's
    // own responses let the editor gate and the cooked server disagree (alpha.30 smoke test: Outpost and Crash).
    ECollisionResponse AcousticResponse(UStaticMeshComponent& Component)
    {
        if (Component.bUseDefaultCollision && Component.SupportsDefaultCollision())
            if (const UBodySetup* Body = Component.GetBodySetup()) return Body->DefaultInstance.GetResponseToChannel(ECC_GameTraceChannel7);
        return Component.GetCollisionResponseToChannel(ECC_GameTraceChannel7);
    }
}

ARetakeAcousticRegion::ARetakeAcousticRegion() { Bounds = CreateBounds(*this); }
ARetakeAcousticOccluder::ARetakeAcousticOccluder()
{
    Bounds = CreateBounds(*this);
    Bounds->SetCollisionResponseToAllChannels(ECR_Ignore);
    Bounds->SetCollisionResponseToChannel(ECC_GameTraceChannel7, ECR_Block);
    Bounds->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
}
ARetakeAcousticConnection::ARetakeAcousticConnection()
{
    Bounds = CreateBounds(*this);
    OpenMaterial.Transmission = .9f; OpenMaterial.LowPassHz = 18000.f;
    bReplicates = true; bAlwaysRelevant = true;
}
ARetakeAcousticMap::ARetakeAcousticMap() { PrimaryActorTick.bCanEverTick = false; }

void ARetakeAcousticConnection::BeginPlay()
{
    Super::BeginPlay();
    if (HasAuthority()) { bOpenState = bInitiallyOpen; SetReplicates(bDynamic && !Door); }
    URetakeMapElementSubsystem::Register(*this);
}
void ARetakeAcousticConnection::EndPlay(const EEndPlayReason::Type Reason)
{ URetakeMapElementSubsystem::Unregister(*this); Super::EndPlay(Reason); }
void ARetakeAcousticConnection::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{ Super::GetLifetimeReplicatedProps(OutLifetimeProps); DOREPLIFETIME(ARetakeAcousticConnection, bOpenState); }
void ARetakeAcousticConnection::SetOpen(bool bOpen)
{
    if (!HasAuthority() || !bDynamic || Door || bOpenState == bOpen) return;
    bOpenState = bOpen; ForceNetUpdate(); OnRep_Open();
}
bool ARetakeAcousticConnection::IsOpen() const
{
    if (IsValid(Door))
    {
        const ERetakeDoorState State = Door->GetDoorState();
        return State == ERetakeDoorState::Open || State == ERetakeDoorState::Broken;
    }
    return bDynamic ? bOpenState : bInitiallyOpen;
}
void ARetakeAcousticConnection::OnRep_Open() { URetakeMapElementSubsystem::NotifyStateChanged(*this, StateChannel); }

bool ARetakeAcousticMap::Gather(TArray<FRetakeAcousticRegionData>& Regions, TArray<FRetakeAcousticConnectionData>& Connections,
    FString& Signature, FString& Error) const
{
    Regions.Reset(); Connections.Reset(); Signature.Reset();
    if (!GetWorld()) { Error = TEXT("No acoustic authoring world."); return false; }
    TArray<ARetakeAcousticRegion*> RegionActors;
    TArray<ARetakeAcousticConnection*> ConnectionActors;
    for (TActorIterator<ARetakeAcousticRegion> It(GetWorld()); It; ++It) RegionActors.Add(*It);
    for (TActorIterator<ARetakeAcousticConnection> It(GetWorld()); It; ++It) ConnectionActors.Add(*It);
    RegionActors.Sort([](const auto& A, const auto& B) { return A.RegionId.LexicalLess(B.RegionId); });
    ConnectionActors.Sort([](const auto& A, const auto& B) { return A.ConnectionId.LexicalLess(B.ConnectionId); });
    // ValidateBake recomputes this signature in cooked servers and clients, so it may hash only data that cooked
    // builds keep bit-identical on every platform, independent of registration (the cooked-identity rule,
    // Docs/Operations/Publishing.md#acoustic-gate). Format 2 dropped the static mesh BodySetupGuid, which cooking
    // re-derives (alpha.29). Format 3 hashes serialized placement bits instead of world transforms and reads default
    // collision from the mesh (alpha.30 smoke test).
    TArray<uint8> Bytes;
    FMemoryWriter Writer(Bytes, true);
    int32 Version = URetakeAcousticMapData::FormatVersion; Writer << Version;
    int32 SignatureFormat = 3; Writer << SignatureFormat;
    // -RetakeAcousticSignatureDump=<folder> writes every hashed record, so editor and cooked inputs can be diffed.
    FString DumpFolder;
    const bool bDump = FParse::Value(FCommandLine::Get(), TEXT("RetakeAcousticSignatureDump="), DumpFolder);
    TArray<FString> Records;
    const auto Record = [&Records, bDump](const FString& Line) { if (bDump) Records.Add(Line); };
    const auto TransformText = [](const FTransform& T)
    {
        const FVector L = T.GetLocation(), S = T.GetScale3D(); const FQuat Q = T.GetRotation();
        return FString::Printf(TEXT("%.17g %.17g %.17g | %.17g %.17g %.17g %.17g | %.17g %.17g %.17g"), L.X, L.Y, L.Z, Q.X, Q.Y, Q.Z, Q.W, S.X, S.Y, S.Z);
    };
    for (const auto* Actor : RegionActors)
    {
        auto& Region = Regions.AddDefaulted_GetRef();
        Region.Id = Actor->RegionId; Region.Transform = Actor->Bounds->GetComponentTransform();
        Region.Extent = Actor->Bounds->GetUnscaledBoxExtent(); Region.Priority = Actor->Priority;
        Region.InternalTransmission = Actor->InternalTransmission; Region.ReverbDecaySeconds = Actor->ReverbDecaySeconds;
        FString Id = Region.Id.ToString().ToLower(), Placement = PlacementBits(Actor->Bounds);
        Writer << Id << Placement << Region.Extent << Region.Priority;
        Writer << Region.InternalTransmission.Transmission << Region.InternalTransmission.LowPassHz << Region.ReverbDecaySeconds;
        Record(TEXT("region ") + Id + TEXT(" ") + Placement + TEXT(" ") + Region.Extent.ToString() + TEXT(" | ") + TransformText(Region.Transform));
    }
    for (const auto* Actor : ConnectionActors)
    {
        auto& Connection = Connections.AddDefaulted_GetRef();
        Connection.Id = Actor->ConnectionId; Connection.RegionA = RegionActors.IndexOfByKey(Actor->RegionA.Get());
        Connection.RegionB = RegionActors.IndexOfByKey(Actor->RegionB.Get()); Connection.Position = Actor->GetActorLocation();
        Connection.OpenMaterial = Actor->OpenMaterial; Connection.ClosedMaterial = Actor->ClosedMaterial;
        Connection.bInitiallyOpen = Actor->bInitiallyOpen; Connection.bDynamic = Actor->bDynamic || Actor->Door != nullptr;
        FString Id = Connection.Id.ToString().ToLower();
        FString DoorPath = Actor->Door ? Actor->Door->GetName().ToLower() : FString(), Placement = PlacementBits(Actor->GetRootComponent());
        Writer << Id << Connection.RegionA << Connection.RegionB << Placement << DoorPath;
        Writer << Connection.OpenMaterial.Transmission << Connection.OpenMaterial.LowPassHz;
        Writer << Connection.ClosedMaterial.Transmission << Connection.ClosedMaterial.LowPassHz << Connection.bInitiallyOpen << Connection.bDynamic;
        Record(FString::Printf(TEXT("connection %s %d %d %s door=%s | %s"), *Id, Connection.RegionA, Connection.RegionB, *Placement, *DoorPath, *Connection.Position.ToString()));
        if (Regions.IsValidIndex(Connection.RegionA)) Regions[Connection.RegionA].Connections.Add(Connections.Num() - 1);
        if (Regions.IsValidIndex(Connection.RegionB)) Regions[Connection.RegionB].Connections.Add(Connections.Num() - 1);
    }
    // Structural geometry participates in stale-bake detection. Moving/destructible blockers are represented
    // by dynamic connections; their runtime state must not change the static authoring signature.
#if WITH_EDITOR
    {
        // The editor compiles static meshes asynchronously after load. Until a mesh finishes, its body setup is a
        // placeholder and every component using it still holds its saved collision; finishing the build re-applies the
        // mesh's collision to components that use default collision (UStaticMeshComponent::PostStaticMeshCompilation).
        // Read only finished meshes, as cooked builds always do (alpha.30: reading mid-build certified Outpost's 354 and
        // Crash's 4,003 opt-ins, none of which block at runtime).
        TSet<UStaticMesh*> Compiling;
        for (TActorIterator<AActor> It(GetWorld()); It; ++It)
        {
            TInlineComponentArray<UStaticMeshComponent*> Meshes(*It);
            for (const auto* Component : Meshes)
                if (UStaticMesh* Mesh = Component->GetStaticMesh(); Mesh && Mesh->IsCompiling()) Compiling.Add(Mesh);
        }
        if (!Compiling.IsEmpty()) FStaticMeshCompilingManager::Get().FinishCompilation(Compiling.Array());
    }
#endif
    TArray<UStaticMeshComponent*> Structure;
    for (TActorIterator<AActor> It(GetWorld()); It; ++It)
    {
        TInlineComponentArray<UStaticMeshComponent*> Meshes(*It);
        for (auto* Component : Meshes)
        {
            if (IsStructuralBlocker(*Component)) Structure.Add(Component);
            if (bDump && Component->GetStaticMesh())
            {
                // Why a component is or is not structure: its own response, the effective one, and where that comes from.
                const UBodySetup* Body = Component->GetBodySetup();
                const ECollisionResponse Own = Component->GetCollisionResponseToChannel(ECC_GameTraceChannel7), Effective = AcousticResponse(*Component);
                Record(FString::Printf(TEXT("collision %s/%s default=%d own=%d effective=%d profile=%s mesh_profile=%s"),
                    *It->GetName().ToLower(), *Component->GetName().ToLower(), Component->bUseDefaultCollision ? 1 : 0, int32(Own), int32(Effective),
                    *Component->GetCollisionProfileName().ToString(), Body ? *Body->DefaultInstance.GetCollisionProfileName().ToString() : TEXT("-")));
            }
        }
    }
    Structure.Sort([](const auto& A, const auto& B)
    {
        const FString AKey = A.GetOwner()->GetName() + TEXT("/") + A.GetName();
        const FString BKey = B.GetOwner()->GetName() + TEXT("/") + B.GetName();
        return AKey.ToLower() < BKey.ToLower();
    });
    for (const auto* Component : Structure)
    {
        FString Id = (Component->GetOwner()->GetName() + TEXT("/") + Component->GetName()).ToLower();
        FString Mesh = Component->GetStaticMesh()->GetPathName().ToLower();
        FString Placement = PlacementBits(Component);
        Writer << Id << Mesh << Placement;
        Record(TEXT("structure ") + Id + TEXT(" ") + Mesh + TEXT(" ") + Placement + TEXT(" | ") + TransformText(Component->GetComponentTransform()));
        if (const auto* Instances = Cast<UInstancedStaticMeshComponent>(Component))
        {
            // The serialized per-instance matrices, not FTransforms decomposed from them.
            int32 Count = Instances->PerInstanceSMData.Num(); Writer << Count;
            for (const FInstancedStaticMeshInstanceData& Instance : Instances->PerInstanceSMData) { FString Bits = MatrixBits(Instance.Transform); Writer << Bits; }
            Record(FString::Printf(TEXT("instances %s %d"), *Id, Count));
        }
    }
    TArray<ARetakeAcousticOccluder*> Occluders;
    for (TActorIterator<ARetakeAcousticOccluder> It(GetWorld()); It; ++It) Occluders.Add(*It);
    Occluders.Sort([](const auto& A, const auto& B) { return A.GetName().ToLower() < B.GetName().ToLower(); });
    for (const auto* Occluder : Occluders)
    {
        FString Name = Occluder->GetName().ToLower(), Placement = PlacementBits(Occluder->Bounds);
        FVector Extent = Occluder->Bounds->GetUnscaledBoxExtent(); Writer << Name << Placement << Extent;
        Record(TEXT("occluder ") + Name + TEXT(" ") + Placement + TEXT(" ") + Extent.ToString());
    }
    uint8 Hash[FSHA1::DigestSize]; FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash);
    Signature = BytesToHex(Hash, UE_ARRAY_COUNT(Hash)).ToLower();
    if (bDump)
    {
        const FString Map = UWorld::RemovePIEPrefix(GetWorld()->GetOutermost()->GetName());
        const TCHAR* Context = IsRunningDedicatedServer() ? TEXT("server") : !GetWorld()->bIsWorldInitialized ? TEXT("saved") : GIsEditor ? TEXT("editor") : TEXT("game");
        const FString File = FPaths::Combine(DumpFolder, Map.Replace(TEXT("/"), TEXT("_")) + TEXT("-") + Context + TEXT(".txt"));
        Records.Insert(TEXT("signature ") + Signature, 0);
        FFileHelper::SaveStringArrayToFile(Records, *File);
        UE_LOGF(LogRetakeAcousticMap, Display, "Acoustic signature records for %ls written to %ls.", *Map, *File);
    }
    return true;
}

bool ARetakeAcousticMap::IsStructuralBlocker(UStaticMeshComponent& Component)
{
    const AActor* Owner = Component.GetOwner();
    if (!Owner || Owner->IsA<ARetakeDoor>() || Owner->ActorHasTag(TEXT("RetakeAcousticDynamic")) || !Component.GetStaticMesh()) return false;
    return AcousticResponse(Component) == ECR_Block;
}

bool ARetakeAcousticMap::ValidateBake(FString& Error) const
{
    if (!Data) { Error = TEXT("Gameplay map has no acoustic bake asset."); return false; }
    if (!Data->Validate(Error)) return false;
    if (UWorld::RemovePIEPrefix(GetWorld()->GetPackage()->GetName()) != Data->MapPackage)
    { Error = TEXT("Acoustic bake belongs to a different map."); return false; }
    TArray<FRetakeAcousticRegionData> Regions; TArray<FRetakeAcousticConnectionData> Connections; FString Signature;
    if (!Gather(Regions, Connections, Signature, Error)) return false;
    if (Signature != Data->SourceSignature) { Error = TEXT("Acoustic authoring changed; rebake this map before cooking or playing."); return false; }
    return true;
}

#if WITH_EDITOR
void ARetakeAcousticMap::Bake()
{
    if (!Data) { UE_LOGF(LogRetakeAcousticMap, Error, "Assign an acoustic map data asset before baking."); return; }
    TArray<FRetakeAcousticRegionData> Regions; TArray<FRetakeAcousticConnectionData> Connections; FString Signature, Error;
    if (!Gather(Regions, Connections, Signature, Error)) { UE_LOGF(LogRetakeAcousticMap, Error, "%ls", *Error); return; }
    auto* Candidate = NewObject<URetakeAcousticMapData>();
    Candidate->MapPackage = GetWorld()->GetPackage()->GetName(); Candidate->SourceSignature = Signature;
    Candidate->Regions = MoveTemp(Regions); Candidate->Connections = MoveTemp(Connections);
    if (!Candidate->Validate(Error)) { UE_LOGF(LogRetakeAcousticMap, Error, "%ls", *Error); return; }
    Data->Modify(); Data->Version = URetakeAcousticMapData::FormatVersion;
    Data->MapPackage = Candidate->MapPackage; Data->SourceSignature = Signature;
    Data->Regions = MoveTemp(Candidate->Regions); Data->Connections = MoveTemp(Candidate->Connections);
    Data->MarkPackageDirty();
    UE_LOGF(LogRetakeAcousticMap, Display, "Baked %d acoustic regions and %d connections; save %ls.", Data->Regions.Num(), Data->Connections.Num(), *Data->GetPathName());
}
EDataValidationResult ARetakeAcousticMap::IsDataValid(FDataValidationContext& Context) const
{
    FString Error;
    if (!ValidateBake(Error)) { Context.AddError(FText::FromString(Error)); return EDataValidationResult::Invalid; }
    return EDataValidationResult::Valid;
}
#endif
