// Copyright 2026 Codnyx. All Rights Reserved.

#include "RetakeMapSDKProbe.h"
#include "RetakeMapSDKLog.h"
#include "RetakeMapSDKVersion.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/Package.h"

ARetakeMapSDKProbe::ARetakeMapSDKProbe()
{
    PrimaryActorTick.bCanEverTick = false;
    bReplicates = false;
    Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
    RootComponent = Mesh;
    // Engine content the base game already cooks; a plugin-only mod cook never contains /Engine packages.
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (Cube.Succeeded()) Mesh->SetStaticMesh(Cube.Object);
}

FString ARetakeMapSDKProbe::Describe() const
{
    TArray<FString> Items;
    for (int32 Value : ProbeArray) Items.Add(FString::FromInt(Value));
    const UWorld* World = GetWorld();
    const TCHAR* Net = !World ? TEXT("none") : World->GetNetMode() == NM_DedicatedServer ? TEXT("server")
        : World->GetNetMode() == NM_Client ? TEXT("client") : World->GetNetMode() == NM_ListenServer ? TEXT("listen") : TEXT("standalone");
    return FString::Printf(TEXT("event=MapSDKProbe net=%s contract=%d oldestReadable=%d package=%s int=%d float=%.3f flag=%d name=%s string=\"%s\" vector=%s color=%s array=[%s] enum=%d material=%s"),
        Net, RetakeMapSDK::ContractVersion, RetakeMapSDK::OldestReadableContract, *GetOutermost()->GetName(),
        ProbeInteger, ProbeFloat, bProbeFlag ? 1 : 0, *ProbeName.ToString(), *ProbeString, *ProbeVector.ToString(),
        *ProbeColor.ToString(), *FString::Join(Items, TEXT(",")), static_cast<int32>(ProbeEnum), *ProbeMaterial.ToString());
}

void ARetakeMapSDKProbe::BeginPlay()
{
    Super::BeginPlay();
    if (UMaterialInterface* Material = ProbeMaterial.LoadSynchronous()) Mesh->SetMaterial(0, Material);
    UE_LOGF(LogRetakeMapSDK, Display, "%ls", *Describe());
}
