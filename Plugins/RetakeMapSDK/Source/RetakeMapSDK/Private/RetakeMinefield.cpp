// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeMinefield.h"
#include "RetakeAudioCueDefinition.h"
#include "UObject/Package.h"
#include "RetakeMapElementSubsystem.h"
#include "RetakeMapSDKLog.h"
#include "Components/BillboardComponent.h"
#include "Components/BoxComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Math/RandomStream.h"
#include "Misc/Crc.h"
#include "Misc/DataValidation.h"
#include "Net/UnrealNetwork.h"

const FName ARetakeMinefield::DetonationChannel(TEXT("Minefield.Detonation"));
const FName ARetakeMinefield::MinesChannel(TEXT("Minefield.Mines"));

namespace RetakeMinefieldLayout
{
    /** A surface a mine (or a player) can rest on: Retake's walkable floor limit. */
    constexpr double WalkableNormalZ = .7;
    /** A layout never holds more mines than this; a larger field needs a larger spacing. */
    constexpr int64 MaximumMines = 4096;

    bool Cells(const FVector2D& HalfExtent, float SpacingCm, int32& OutColumns, int32& OutRows, FVector2D& OutCell)
    {
        if (HalfExtent.ContainsNaN() || HalfExtent.GetMin() <= 0 || !FMath::IsFinite(SpacingCm) || SpacingCm <= 0) return false;
        const double Columns = FMath::Max(1., FMath::CeilToDouble(2 * HalfExtent.X / SpacingCm));
        const double Rows = FMath::Max(1., FMath::CeilToDouble(2 * HalfExtent.Y / SpacingCm));
        if (Columns * Rows > MaximumMines) return false;
        OutColumns = int32(Columns);
        OutRows = int32(Rows);
        OutCell = FVector2D(2 * HalfExtent.X / Columns, 2 * HalfExtent.Y / Rows);
        return true;
    }
}

ARetakeMinefield::ARetakeMinefield()
{
    PrimaryActorTick.bCanEverTick = false;
    // Detonations are a reliable multicast and the exploded mines a small replicated mask, from a map actor every client has.
    bReplicates = true;
    bAlwaysRelevant = true;
    bReplayRewindable = true;
    SetReplicateMovement(false);
    SetNetUpdateFrequency(1.f);
    // Nobody's cover: the anti-wallhack scene skips the actor (the mines carry the component tag too).
    Tags.Add(TEXT("Retake.Visibility.Ignore"));
    Field = CreateDefaultSubobject<UBoxComponent>(TEXT("Field"));
    SetRootComponent(Field);
    Field->SetBoxExtent(FVector(200, 200, 100));
    Field->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Field->SetGenerateOverlapEvents(false);
    Field->SetCanEverAffectNavigation(false);
    Field->SetHiddenInGame(true);
    Field->ShapeColor = FColor(220, 60, 40);
    Mines = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Mines"));
    Mines->SetupAttachment(Field);
    Mines->SetUsingAbsoluteScale(true); // instances live in the actor's unscaled frame, whatever the field's scale
    Mines->SetMobility(EComponentMobility::Movable); // exploded mines are hidden at runtime
    Mines->bUseDefaultCollision = false;
    Mines->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Mines->SetCollisionResponseToAllChannels(ECR_Ignore); // players walk over them; outside acoustics and every trace
    Mines->SetGenerateOverlapEvents(false);
    Mines->SetCanEverAffectNavigation(false);
    // The same for everyone: never distance-culled, no shadow (small, flat and cheap).
    Mines->SetCastShadow(false);
    Mines->bNeverDistanceCull = true;
    Mines->SetCullDistances(0, 0);
    Mines->ComponentTags.Add(TEXT("Retake.Visibility.Ignore"));
#if WITH_EDITORONLY_DATA
    Sprite = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("Sprite"));
    if (Sprite) Sprite->SetupAttachment(Field);
#endif
}

void ARetakeMinefield::BeginPlay()
{
    Super::BeginPlay();
    PresentMines();
    URetakeMapElementSubsystem::Register(*this);
}

void ARetakeMinefield::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    URetakeMapElementSubsystem::Unregister(*this);
    Super::EndPlay(EndPlayReason);
}

void ARetakeMinefield::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ARetakeMinefield, ExplodedMines);
}

bool ARetakeMinefield::OrientedBoxOverlaps(const FVector& Center, const FQuat& Rotation, const FVector& HalfExtent, const FBox& WorldBox)
{
    if (!WorldBox.IsValid || Center.ContainsNaN() || Rotation.ContainsNaN() || HalfExtent.ContainsNaN() || HalfExtent.GetMin() <= 0) return false;
    const FVector Axes[3] = {Rotation.GetAxisX(), Rotation.GetAxisY(), Rotation.GetAxisZ()};
    const FVector WorldAxes[3] = {FVector::XAxisVector, FVector::YAxisVector, FVector::ZAxisVector};
    const FVector Extent = WorldBox.GetExtent();
    const FVector Delta = WorldBox.GetCenter() - Center;
    // Separating axis theorem for two boxes: the three face normals of each, then the nine edge cross products. The axes need
    // not be unit length: both projected radii and the centre distance scale with it.
    const auto Separates = [&](const FVector& Axis)
    {
        if (Axis.SizeSquared() < UE_KINDA_SMALL_NUMBER) return false; // parallel edges; their face axes already decide
        const double FieldRadius = HalfExtent.X * FMath::Abs(Axes[0] | Axis) + HalfExtent.Y * FMath::Abs(Axes[1] | Axis) + HalfExtent.Z * FMath::Abs(Axes[2] | Axis);
        const double BoxRadius = Extent.X * FMath::Abs(Axis.X) + Extent.Y * FMath::Abs(Axis.Y) + Extent.Z * FMath::Abs(Axis.Z);
        return FMath::Abs(Delta | Axis) > FieldRadius + BoxRadius;
    };
    for (const FVector& Axis : WorldAxes) if (Separates(Axis)) return false; // world axes first: they reject distant pawns
    for (const FVector& Axis : Axes) if (Separates(Axis)) return false;
    for (const FVector& A : Axes) for (const FVector& B : WorldAxes) if (Separates(A ^ B)) return false;
    return true;
}

bool ARetakeMinefield::OverlapsBox(const FBox& WorldBox) const
{
    return Field && OrientedBoxOverlaps(Field->GetComponentLocation(), Field->GetComponentQuat(), Field->GetScaledBoxExtent(), WorldBox);
}

FString ARetakeMinefield::GetConfigurationError() const
{
    const FVector Extent = Field ? Field->GetScaledBoxExtent() : FVector::ZeroVector;
    if (GetActorLocation().ContainsNaN() || Extent.ContainsNaN() || Extent.GetMin() <= KINDA_SMALL_NUMBER) return TEXT("The field needs a valid location and a positive size.");
    if (!FMath::IsFinite(MinimumDelaySeconds) || !FMath::IsFinite(MaximumDelaySeconds) || MinimumDelaySeconds < 0 || MaximumDelaySeconds < MinimumDelaySeconds)
        return TEXT("The delays must be finite, at least zero, and Maximum Delay must not be shorter than Minimum Delay.");
    if (!FMath::IsFinite(RearmSeconds) || RearmSeconds < 0) return TEXT("Rearm Seconds must be finite and at least zero.");
    if (!FMath::IsFinite(Damage) || Damage <= 0) return TEXT("Damage must be positive.");
    if (!FMath::IsFinite(DamageRadiusCm) || DamageRadiusCm <= 0) return TEXT("Damage Radius must be positive.");
    return FString();
}

FString ARetakeMinefield::GetPresentationError() const
{
    const UWorld* World = GetWorld();
    if (!World || GetLevel() != World->PersistentLevel)
        return TEXT("Minefields must be placed in the persistent level so presentation is available before player admission.");
    const FString Package = World->GetOutermost()->GetName();
    const int32 End = Package.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
    if (End == INDEX_NONE) return TEXT("Invalid minefield map package.");
    const FString Root = Package.Left(End + 1);
    for (const URetakeAudioCueDefinition* Cue : {TriggerCue.Get(), ExplosionCue.Get()})
    {
        if (!Cue) return TEXT("Assign both Trigger Cue and Explosion Cue; minefields do not borrow weapon presentation.");
        FString Error;
        if (!Cue->Validate(Error)) return Error;
        if (Cue->GetClass() != URetakeAudioCueDefinition::StaticClass() || !Cue->bSpatial || Cue->bLooping
            || Cue->Category != ERetakeAudioCategory::Gameplay)
            return TEXT("Minefield cues must be native, spatial, one-shot gameplay cues.");
        if (!Cue->GetOutermost()->GetName().StartsWith(Root, ESearchCase::IgnoreCase))
            return TEXT("Minefield cues must belong to the map's content namespace: ") + Cue->GetPathName();
    }
    if (ExplosionEffect.IsNull() || !ExplosionEffect.ToSoftObjectPath().GetLongPackageName().StartsWith(Root, ESearchCase::IgnoreCase))
        return TEXT("Assign an Explosion Effect owned by the map's content namespace.");
    return {};
}

int32 ARetakeMinefield::ComputeMineLayoutHash() const
{
    const FVector Location = GetActorLocation();
    const FQuat Rotation = GetActorQuat();
    const FVector Extent = Field ? Field->GetScaledBoxExtent() : FVector::ZeroVector;
    const FString Key = FString::Printf(TEXT("%.1f %.1f %.1f|%.4f %.4f %.4f %.4f|%.1f %.1f %.1f|%.2f %.3f %d"), Location.X, Location.Y, Location.Z,
        Rotation.X, Rotation.Y, Rotation.Z, Rotation.W, Extent.X, Extent.Y, Extent.Z, MineSpacingCm, MineJitter, MineLayoutSeed);
    return int32(FCrc::StrCrc32(*Key));
}

FString ARetakeMinefield::GetMineLayoutWarning() const
{
    if (MineTransforms.IsEmpty()) return TEXT("The field has no mines, so a blast comes from the player's feet (MOHAA). Run Regenerate Mines to lay them out.");
    if (!Mines || !Mines->GetStaticMesh()) return TEXT("The mines have no Static Mesh: set one on the Mines component.");
    if (MineLayoutHash != ComputeMineLayoutHash()) return TEXT("The field moved, changed size or changed its mine layout settings since Regenerate Mines: run it again.");
    return FString();
}

#if WITH_EDITOR
EDataValidationResult ARetakeMinefield::IsDataValid(FDataValidationContext& Context) const
{
    EDataValidationResult Result = Super::IsDataValid(Context);
    if (const FString Error = IsTemplate() ? FString() : GetConfigurationError(); !Error.IsEmpty())
    {
        Context.AddError(FText::FromString(Error));
        Result = EDataValidationResult::Invalid;
    }
    if (const FString Error = IsTemplate() ? FString() : GetPresentationError(); !Error.IsEmpty())
    {
        Context.AddError(FText::FromString(Error));
        Result = EDataValidationResult::Invalid;
    }
    if (const FString Warning = IsTemplate() ? FString() : GetMineLayoutWarning(); !Warning.IsEmpty()) Context.AddWarning(FText::FromString(Warning));
    return Result;
}
#endif

void ARetakeMinefield::LayoutGrid(const FVector2D& HalfExtent, float SpacingCm, float Jitter, int32 Seed, TArray<FVector2D>& OutPoints, TArray<float>& OutYaws)
{
    OutPoints.Reset();
    OutYaws.Reset();
    int32 Columns = 0, Rows = 0;
    FVector2D Cell;
    if (!RetakeMinefieldLayout::Cells(HalfExtent, SpacingCm, Columns, Rows, Cell)) return;
    const double Spread = FMath::IsFinite(Jitter) ? FMath::Clamp(double(Jitter), 0., 1.) : 0.;
    FRandomStream Random(Seed);
    for (int32 Row = 0; Row < Rows; ++Row)
        for (int32 Column = 0; Column < Columns; ++Column)
        {
            const double JitterX = Random.FRandRange(-.5f, .5f), JitterY = Random.FRandRange(-.5f, .5f);
            OutYaws.Add(Random.FRandRange(0.f, 360.f));
            OutPoints.Emplace(-HalfExtent.X + (Column + .5 + JitterX * Spread) * Cell.X, -HalfExtent.Y + (Row + .5 + JitterY * Spread) * Cell.Y);
        }
}

double ARetakeMinefield::LayoutCoverageCm(const FVector2D& HalfExtent, float SpacingCm, float Jitter)
{
    int32 Columns = 0, Rows = 0;
    FVector2D Cell;
    if (!RetakeMinefieldLayout::Cells(HalfExtent, SpacingCm, Columns, Rows, Cell)) return TNumericLimits<double>::Max();
    // A point in a cell is at most half a cell from its centre, and the cell's mine at most Spread / 2 of a cell from it.
    const double Spread = FMath::IsFinite(Jitter) ? FMath::Clamp(double(Jitter), 0., 1.) : 0.;
    return .5 * (1 + Spread) * Cell.Size();
}

void ARetakeMinefield::RegenerateMines()
{
    UWorld* World = GetWorld();
    if (!World || World->IsGameWorld() || !Field || !Mines) return;
    const FVector Extent = Field->GetScaledBoxExtent();
    const FTransform Box(GetActorQuat(), Field->GetComponentLocation());
    const FTransform Frame(GetActorQuat(), GetActorLocation());
    FCollisionQueryParams Query(SCENE_QUERY_STAT(RetakeMinefieldLayout), true, this);
    Query.bReturnFaceIndex = true;
    // The first surface down a column that a mine can rest on: visible in game, opaque (not water, foliage or a grate) and not a
    // player. Hidden collision (pawn-only floors, clip) is skipped; the caller decides whether it is walkable.
    const auto FindSurface = [&](double X, double Y, FHitResult& Out)
    {
        TArray<FHitResult> Hits;
        World->LineTraceMultiByObjectType(Hits, Box.TransformPosition(FVector(X, Y, Extent.Z)), Box.TransformPosition(FVector(X, Y, -Extent.Z)),
            FCollisionObjectQueryParams(ECC_WorldStatic), Query);
        Hits.Sort([](const FHitResult& A, const FHitResult& B) { return A.Time < B.Time; });
        for (const FHitResult& Hit : Hits)
        {
            const UPrimitiveComponent* Component = Hit.GetComponent();
            const AActor* Owner = Hit.GetActor();
            if (!Component || !Owner || Owner == this || Owner->IsHidden() || Owner->IsEditorOnly() || Component->IsEditorOnly()
                || !Component->IsVisible() || Component->bHiddenInGame) continue;
            int32 Section = 0;
            const UMaterialInterface* Material = Hit.FaceIndex != INDEX_NONE ? Component->GetMaterialFromCollisionFaceIndex(Hit.FaceIndex, Section) : nullptr;
            if (!Material) Material = Component->GetMaterial(0);
            if (Material && Material->GetBlendMode() != BLEND_Opaque) continue;
            Out = Hit;
            return true;
        }
        return false;
    };

    TArray<FVector2D> Points;
    TArray<float> Yaws;
    LayoutGrid(FVector2D(Extent.X, Extent.Y), MineSpacingCm, MineJitter, MineLayoutSeed, Points, Yaws);
    TArray<FTransform> Placed;
    TArray<FVector> Locations;
    int32 Missing = 0, Steep = 0;
    for (int32 Index = 0; Index < Points.Num(); ++Index)
    {
        FHitResult Surface;
        if (!FindSurface(Points[Index].X, Points[Index].Y, Surface)) { ++Missing; continue; }
        if (Surface.ImpactNormal.Z < RetakeMinefieldLayout::WalkableNormalZ) { ++Steep; continue; }
        // The field's yaw plus the mine's own turn, then tilted onto the surface.
        const FQuat Turned = GetActorQuat() * FQuat(FVector::UpVector, FMath::DegreesToRadians(double(Yaws[Index])));
        const FQuat Rotation = FQuat::FindBetweenNormals(Turned.GetUpVector(), Surface.ImpactNormal) * Turned;
        Placed.Add(FTransform(Rotation, Surface.ImpactPoint).GetRelativeTransform(Frame));
        Locations.Add(Surface.ImpactPoint);
    }
    // Coverage: every standable point of a finer grid (a quarter cell) to its nearest mine.
    double Worst = 0;
    int32 Standable = 0;
    const double Step = FMath::Max(25., double(MineSpacingCm) / 4);
    for (double X = -Extent.X + Step / 2; X < Extent.X; X += Step)
        for (double Y = -Extent.Y + Step / 2; Y < Extent.Y; Y += Step)
        {
            FHitResult Floor;
            if (!FindSurface(X, Y, Floor) || Floor.ImpactNormal.Z < RetakeMinefieldLayout::WalkableNormalZ) continue;
            ++Standable;
            double Nearest = TNumericLimits<double>::Max();
            for (const FVector& Mine : Locations) Nearest = FMath::Min(Nearest, FVector::Dist(Mine, Floor.ImpactPoint));
            Worst = FMath::Max(Worst, Nearest);
        }

    Modify();
    MineTransforms = MoveTemp(Placed);
    MineLayoutHash = ComputeMineLayoutHash();
    ExplodedMines.Reset();
    Mines->Modify();
    Mines->ClearInstances();
    if (!MineTransforms.IsEmpty()) Mines->AddInstances(MineTransforms, false, false, false);
    MineLayoutReport = FString::Printf(TEXT("%d mines from %d cells (%d without a surface, %d too steep); worst distance from %d standable points to their nearest mine %.0f cm, blast radius %.0f cm%s"),
        MineTransforms.Num(), Points.Num(), Missing, Steep, Standable, Standable ? Worst : 0., DamageRadiusCm,
        Standable && Worst > DamageRadiusCm ? TEXT(" (WARNING: some standable points have no mine in reach; reduce Mine Spacing)") : TEXT(""));
    UE_LOGF(LogRetakeMapSDK, Display, "event=MinefieldLayout field=%ls %ls", *GetName(), *MineLayoutReport);
}

bool ARetakeMinefield::IsBitSet(const TArray<uint8>& Bits, int32 Index)
{
    return Index >= 0 && Bits.IsValidIndex(Index / 8) && (Bits[Index / 8] & uint8(1u << (Index % 8))) != 0;
}

bool ARetakeMinefield::SetBit(TArray<uint8>& Bits, int32 Index, int32 Count)
{
    if (Index < 0 || Index >= Count || IsBitSet(Bits, Index)) return false;
    if (Bits.Num() < (Count + 7) / 8) Bits.SetNumZeroed((Count + 7) / 8);
    Bits[Index / 8] |= uint8(1u << (Index % 8));
    return true;
}

int32 ARetakeMinefield::GetAvailableMineCount() const
{
    int32 Available = 0;
    for (int32 Index = 0; Index < MineTransforms.Num(); ++Index) Available += IsBitSet(ExplodedMines, Index) ? 0 : 1;
    return Available;
}

FVector ARetakeMinefield::GetMineLocation(int32 Index) const
{
    return MineTransforms.IsValidIndex(Index) ? FTransform(GetActorQuat(), GetActorLocation()).TransformPosition(MineTransforms[Index].GetLocation()) : GetActorLocation();
}

FVector ARetakeMinefield::GetMineUp(int32 Index) const
{
    return MineTransforms.IsValidIndex(Index) ? (GetActorQuat() * MineTransforms[Index].GetRotation()).GetUpVector() : FVector::UpVector;
}

bool ARetakeMinefield::ServerConsumeMine(int32 Index)
{
    if (!HasAuthority() || !SetBit(ExplodedMines, Index, MineTransforms.Num())) return false;
    OnRep_ExplodedMines();
    ForceNetUpdate();
    return true;
}

void ARetakeMinefield::ServerRestoreMines()
{
    if (!HasAuthority() || ExplodedMines.IsEmpty()) return;
    ExplodedMines.Reset();
    OnRep_ExplodedMines();
    ForceNetUpdate();
}

void ARetakeMinefield::OnRep_ExplodedMines()
{
    PresentMines();
    URetakeMapElementSubsystem::NotifyStateChanged(*this, MinesChannel);
}

void ARetakeMinefield::PresentMines()
{
    // Editor worlds show the authored instances; changing them there would dirty the map.
    const UWorld* World = GetWorld();
    if (!Mines || !World || !World->IsGameWorld()) return;
    const int32 Count = MineTransforms.Num();
    if (Mines->GetInstanceCount() != Count)
    {
        Mines->ClearInstances();
        if (Count) Mines->AddInstances(MineTransforms, false, false, false);
    }
    bool bChanged = false;
    for (int32 Index = 0; Index < Count; ++Index)
    {
        const FTransform& Authored = MineTransforms[Index];
        const FTransform Shown = IsBitSet(ExplodedMines, Index) ? FTransform(Authored.GetRotation(), Authored.GetLocation(), FVector::ZeroVector) : Authored;
        FTransform Current;
        if (Mines->GetInstanceTransform(Index, Current, false) && Current.Equals(Shown, .01)) continue;
        Mines->UpdateInstanceTransform(Index, Shown, false, false, true);
        bChanged = true;
    }
    if (bChanged) Mines->MarkRenderStateDirty();
}

void ARetakeMinefield::ServerPresentDetonation(const FVector& Location)
{
    if (!HasAuthority() || Location.ContainsNaN()) return;
    ForceNetUpdate();
    MulticastDetonation(FVector_NetQuantize(Location));
}

void ARetakeMinefield::MulticastDetonation_Implementation(const FVector_NetQuantize& Location)
{
    LastDetonationLocation = Location;
    URetakeMapElementSubsystem::NotifyStateChanged(*this, DetonationChannel);
}
