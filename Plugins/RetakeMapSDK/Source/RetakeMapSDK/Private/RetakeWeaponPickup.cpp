// © 2026 Codnyx. All Rights Reserved.
#include "RetakeWeaponPickup.h"
#include "RetakeMapElementSubsystem.h"
#include "RetakeMapSDKIds.h"

#include "Components/SkeletalMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Misc/DataValidation.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

const FName ARetakeWeaponPickup::StateChannel(TEXT("WeaponPickup.State"));

ARetakeWeaponPickup::ARetakeWeaponPickup()
{
	bReplicates = true;
	PrimaryActorTick.bCanEverTick = false;
	DropCollision = CreateDefaultSubobject<USphereComponent>(TEXT("DropCollision"));
	SetRootComponent(DropCollision);
	DropCollision->InitSphereRadius(8.f);
	DropCollision->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	DropCollision->SetCanEverAffectNavigation(false);
	Mesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("WorldWeapon"));
	Mesh->SetupAttachment(DropCollision);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetComponentTickEnabled(false);
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
	InteractionRange = CreateDefaultSubobject<USphereComponent>(TEXT("InteractionRange"));
	InteractionRange->SetupAttachment(Mesh);
	InteractionRange->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	InteractionRange->SetGenerateOverlapEvents(false);
	InteractionRange->SetCanEverAffectNavigation(false);
	InteractionRange->SetHiddenInGame(true);
	InteractionRange->ShapeColor = FColor(80, 200, 255);
#if WITH_EDITORONLY_DATA
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	WeaponProxy = CreateEditorOnlyDefaultSubobject<UStaticMeshComponent>(TEXT("WeaponProxy"));
	if (WeaponProxy)
	{
		WeaponProxy->SetupAttachment(DropCollision);
		WeaponProxy->SetStaticMesh(Cube.Object);
		WeaponProxy->SetRelativeScale3D(FVector(0.8f, 0.06f, 0.2f)); // 80 x 6 x 20 cm on the 100 cm cube: a rifle lying along +X.
		WeaponProxy->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		WeaponProxy->SetHiddenInGame(true);
		WeaponProxy->SetCastShadow(false);
		WeaponProxy->bIsEditorOnly = true;
	}
	WeaponLabel = CreateEditorOnlyDefaultSubobject<UTextRenderComponent>(TEXT("WeaponLabel"));
	if (WeaponLabel)
	{
		WeaponLabel->SetupAttachment(DropCollision);
		WeaponLabel->SetRelativeLocation(FVector(0, 0, 30));
		WeaponLabel->SetWorldSize(12.f);
		WeaponLabel->SetHorizontalAlignment(EHTA_Center);
		WeaponLabel->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		WeaponLabel->SetHiddenInGame(true);
		WeaponLabel->bIsEditorOnly = true;
	}
#endif
}

void ARetakeWeaponPickup::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARetakeWeaponPickup, bAvailable);
}

void ARetakeWeaponPickup::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	UpdateInteractionRange();
#if WITH_EDITORONLY_DATA
	if (WeaponLabel) WeaponLabel->SetText(FText::AsCultureInvariant(WeaponId.IsNone() ? FString(TEXT("No weapon")) : WeaponId.ToString()));
#endif
}

void ARetakeWeaponPickup::BeginPlay()
{
	Super::BeginPlay();
	UpdateInteractionRange();
	URetakeMapElementSubsystem::Register(*this);
}

void ARetakeWeaponPickup::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	URetakeMapElementSubsystem::Unregister(*this);
	Super::EndPlay(EndPlayReason);
}

void ARetakeWeaponPickup::UpdateInteractionRange() const
{
	InteractionRange->SetSphereRadius(InteractionDistance, false);
	InteractionRange->SetHiddenInGame(!bShowInteractionRange);
}

void ARetakeWeaponPickup::SetAvailable(bool bInAvailable)
{
	if (!HasAuthority() || bAvailable == bInAvailable) return;
	bAvailable = bInAvailable;
	OnRep_Available();
	ForceNetUpdate();
}

void ARetakeWeaponPickup::OnRep_Available()
{
	URetakeMapElementSubsystem::NotifyStateChanged(*this, StateChannel);
}

FText ARetakeWeaponPickup::GetConfigurationError() const
{
	if (WeaponId.IsNone()) return NSLOCTEXT("RetakeMapSDK", "PickupNoWeapon", "Choose a Weapon Id.");
	if (!RetakeMapSDKIds::Weapons().Contains(WeaponId))
		return FText::Format(NSLOCTEXT("RetakeMapSDK", "PickupUnknownWeapon", "Unknown Weapon Id '{0}'."), FText::AsCultureInvariant(WeaponId.ToString()));
	if (MagazineRounds < 0 || SpareRounds < 0 || SecondarySpareRounds < 0 || Quantity < 0)
		return NSLOCTEXT("RetakeMapSDK", "PickupNegativeCount", "Magazine Rounds, Spare Rounds, Secondary Spare Rounds and Quantity cannot be negative.");
	if (!FMath::IsFinite(RespawnSecondsOverride))
		return NSLOCTEXT("RetakeMapSDK", "PickupRespawn", "Respawn Seconds Override must be a number (negative uses the game's delay).");
	return FText::GetEmpty();
}

#if WITH_EDITOR
EDataValidationResult ARetakeWeaponPickup::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);
	// Blueprint defaults leave WeaponId to each placement.
	if (const FText Error = IsTemplate() ? FText::GetEmpty() : GetConfigurationError(); !Error.IsEmpty())
	{
		Context.AddError(Error);
		Result = EDataValidationResult::Invalid;
	}
	return Result;
}
#endif
