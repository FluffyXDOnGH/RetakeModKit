// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeMinimapBaker.h"
#include "RetakeMapSDKLog.h"
#include "RetakeMinimapCapture.h"
#include "RetakeMinimapDefinition.h"
#include "Algo/AnyOf.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Hash/Blake3.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "StaticMeshResources.h"
#include "TextureCompiler.h"
#include "UObject/Package.h"
#include "UObject/MetaData.h"
#include "UObject/SavePackage.h"

namespace
{
    /** URetakeMinimapDefinition::ValidateDefinition requires 1024 x 1024. */
    constexpr int32 ImageSide = 1024;
    const TCHAR* AuthoringModeKey = TEXT("Retake.Minimap.AuthoringMode");
    constexpr int32 Supersampling = 3;
    constexpr double MinimumUpNormal = .72;
    /** Neighbouring surfaces further apart than this (cm) are separated by an outline: ledges, stairs, crates. */
    constexpr float StepHeight = 50.f;
    /** In final pixels. */
    constexpr float OutlineWidth = 2.6f;
    constexpr float ArtOpacity = .88f;
    /** The Retake minimap palette (Docs/Systems/UI/Minimap.md): one gray per floor band, light outlines. */
    constexpr uint8 FloorGrays[] = { 49, 76, 103, 133 };
    const FColor OutlineColor(222, 225, 223);

    /** The capture's view of the world; ToUV is URetakeMinimapDefinition::WorldToUV for the definition the bake writes. */
    struct FCaptureFrame
    {
        FVector2D Center, North, East;
        double Yaw, Span, Datum, Ceiling;
        /** Absolute heights of the floor band boundaries. */
        TArray<double> Bands;

        explicit FCaptureFrame(const ARetakeMinimapCapture& Capture)
        {
            const FVector Location = Capture.GetActorLocation();
            Yaw = Capture.GetActorRotation().Yaw;
            const double Radians = FMath::DegreesToRadians(Yaw);
            Center = FVector2D(Location.X, Location.Y);
            North = FVector2D(FMath::Cos(Radians), FMath::Sin(Radians));
            East = FVector2D(-North.Y, North.X);
            Span = Capture.CaptureSize;
            Datum = Location.Z;
            Ceiling = Datum + Capture.CaptureHeight;
            for (const double Height : Capture.FloorBandHeights) Bands.Add(Datum + Height);
        }
        FVector2D ToUV(const FVector& Point) const
        {
            const FVector2D Delta(Point.X - Center.X, Point.Y - Center.Y);
            return FVector2D(.5 + FVector2D::DotProduct(Delta, East) / Span, .5 - FVector2D::DotProduct(Delta, North) / Span);
        }
        int32 Band(double Height) const
        {
            int32 Index = 0;
            while (Index < Bands.Num() && Height >= Bands[Index]) ++Index;
            return Index;
        }
    };

    struct FTriangle { FVector A, B, C; };

    /** World-space upward faces of LOD0 render data inside the capture square and below its ceiling. */
    void GatherTriangles(const UWorld& World, const ARetakeMinimapCapture& Capture, const FCaptureFrame& Frame, TArray<FTriangle>& Out)
    {
        if (!World.PersistentLevel) return;
        for (const AActor* Actor : World.PersistentLevel->Actors)
        {
            if (!Actor || Actor->IsEditorOnly() || Actor->IsHidden() || Actor->ActorHasTag(Capture.ExcludeTag)) continue;
            for (const UStaticMeshComponent* Component : TInlineComponentArray<UStaticMeshComponent*>(Actor))
            {
                const UStaticMesh* Mesh = Component->GetStaticMesh();
                const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
                if (!RenderData || RenderData->LODResources.IsEmpty() || Component->IsEditorOnly() || Component->bHiddenInGame
                    || !Component->IsVisible() || Component->ComponentHasTag(Capture.ExcludeTag)) continue;
                const FStaticMeshLODResources& Lod = RenderData->LODResources[0];
                const FIndexArrayView Indices = Lod.IndexBuffer.GetArrayView();
                const FPositionVertexBuffer& Positions = Lod.VertexBuffers.PositionVertexBuffer;
                // A spline mesh bends each vertex along its spline first, as the renderer and its collision do.
                const auto* Spline = Cast<USplineMeshComponent>(Component);
                const auto Local = [&Positions, Spline](uint32 Vertex)
                {
                    FVector3f Position = Positions.VertexPosition(Vertex);
                    if (!Spline) return FVector(Position);
                    float& Along = USplineMeshComponent::GetAxisValueRef(Position, Spline->ForwardAxis);
                    const FTransform Slice = Spline->CalcSliceTransform(Along);
                    Along = 0.f;
                    return Slice.TransformPosition(FVector(Position));
                };
                TArray<FTransform, TInlineAllocator<1>> Transforms;
                if (const auto* Instanced = Cast<UInstancedStaticMeshComponent>(Component))
                {
                    for (int32 Instance = 0; Instance < Instanced->GetInstanceCount(); ++Instance)
                        Instanced->GetInstanceTransform(Instance, Transforms.AddDefaulted_GetRef(), true);
                }
                else Transforms.Add(Component->GetComponentTransform());
                for (const FTransform& Transform : Transforms)
                {
                    // Front faces wind clockwise seen from the front; a mirrored transform reverses that.
                    const double Facing = Transform.GetDeterminant() < 0 ? -1. : 1.;
                    for (int32 Index = 0; Index + 2 < Indices.Num(); Index += 3)
                    {
                        const FVector A = Transform.TransformPosition(Local(Indices[Index]));
                        const FVector B = Transform.TransformPosition(Local(Indices[Index + 1]));
                        const FVector C = Transform.TransformPosition(Local(Indices[Index + 2]));
                        if (FVector::CrossProduct(C - A, B - A).GetSafeNormal().Z * Facing < MinimumUpNormal || FMath::Min3(A.Z, B.Z, C.Z) >= Frame.Ceiling) continue;
                        const FVector2D UA = Frame.ToUV(A), UB = Frame.ToUV(B), UC = Frame.ToUV(C);
                        if (FMath::Max3(UA.X, UB.X, UC.X) < 0 || FMath::Min3(UA.X, UB.X, UC.X) > 1
                            || FMath::Max3(UA.Y, UB.Y, UC.Y) < 0 || FMath::Min3(UA.Y, UB.Y, UC.Y) > 1) continue;
                        Out.Add({ A, B, C });
                    }
                }
            }
        }
    }

    /** Centimetre vertices; sorted before hashing, so the same geometry hashes the same whatever the actor order. */
    struct FQuantizedTriangle
    {
        int64 Values[9];
        bool operator<(const FQuantizedTriangle& Other) const
        {
            for (int32 Index = 0; Index < 9; ++Index)
                if (Values[Index] != Other.Values[Index]) return Values[Index] < Other.Values[Index];
            return false;
        }
    };

    FString Fingerprint(const ARetakeMinimapCapture& Capture, const FCaptureFrame& Frame, const TArray<FTriangle>& Triangles)
    {
        TArray<FQuantizedTriangle> Quantized;
        Quantized.Reserve(Triangles.Num());
        for (const FTriangle& Triangle : Triangles)
        {
            FQuantizedTriangle& Entry = Quantized.AddDefaulted_GetRef();
            int32 Slot = 0;
            for (const FVector* Vertex : { &Triangle.A, &Triangle.B, &Triangle.C })
                for (int32 Axis = 0; Axis < 3; ++Axis) Entry.Values[Slot++] = FMath::RoundToInt64((*Vertex)[Axis]);
        }
        Quantized.Sort();
        TArray<FString> Bands;
        for (const double Height : Capture.FloorBandHeights) Bands.Add(FString::Printf(TEXT("%.2f"), Height));
        const FTCHARToUTF8 Header(*FString::Printf(TEXT("RetakeMinimapBake|%d|%.2f|%.2f|%.2f|%.3f|%.2f|%.2f|%s|"), RetakeMinimapBaker::Version,
            Frame.Center.X, Frame.Center.Y, Frame.Datum, Frame.Yaw, Frame.Span, Capture.CaptureHeight, *FString::Join(Bands, TEXT(","))));
        FBlake3 Hasher;
        Hasher.Update(Header.Get(), Header.Length());
        Hasher.Update(Quantized.GetData(), Quantized.Num() * sizeof(FQuantizedTriangle));
        return LexToString(Hasher.Finalize()).ToLower();
    }

    /** Top-down: the highest surface per supersampled pixel, shaded by its floor band, outlined, then box-filtered to ImageSide. */
    TArray<FColor> Rasterize(const FCaptureFrame& Frame, const TArray<FTriangle>& Triangles)
    {
        constexpr int32 Full = ImageSide * Supersampling;
        const float Uncovered = TNumericLimits<float>::Lowest();
        TArray<float> Heights;
        Heights.Init(Uncovered, Full * Full);
        for (const FTriangle& Triangle : Triangles)
        {
            const FVector2D P[3] = { Frame.ToUV(Triangle.A) * Full, Frame.ToUV(Triangle.B) * Full, Frame.ToUV(Triangle.C) * Full };
            const double Z[3] = { Triangle.A.Z, Triangle.B.Z, Triangle.C.Z };
            const double Area = FVector2D::CrossProduct(P[1] - P[0], P[2] - P[0]);
            if (FMath::IsNearlyZero(Area)) continue;
            const int32 MinX = FMath::Max(0, FMath::FloorToInt32(FMath::Min3(P[0].X, P[1].X, P[2].X)));
            const int32 MaxX = FMath::Min(Full - 1, FMath::CeilToInt32(FMath::Max3(P[0].X, P[1].X, P[2].X)));
            const int32 MinY = FMath::Max(0, FMath::FloorToInt32(FMath::Min3(P[0].Y, P[1].Y, P[2].Y)));
            const int32 MaxY = FMath::Min(Full - 1, FMath::CeilToInt32(FMath::Max3(P[0].Y, P[1].Y, P[2].Y)));
            for (int32 Y = MinY; Y <= MaxY; ++Y)
                for (int32 X = MinX; X <= MaxX; ++X)
                {
                    const FVector2D Q(X + .5, Y + .5);
                    const double W0 = FVector2D::CrossProduct(P[1] - Q, P[2] - Q) / Area;
                    const double W1 = FVector2D::CrossProduct(P[2] - Q, P[0] - Q) / Area;
                    const double W2 = 1. - W0 - W1;
                    if (W0 < 0 || W1 < 0 || W2 < 0) continue;
                    const float Height = float(W0 * Z[0] + W1 * Z[1] + W2 * Z[2]);
                    float& Stored = Heights[Y * Full + X];
                    if (Height < Frame.Ceiling && Height > Stored) Stored = Height;
                }
        }

        TArray<int8> Bands;
        Bands.SetNumUninitialized(Full * Full);
        for (int32 Index = 0; Index < Heights.Num(); ++Index)
            Bands[Index] = Heights[Index] == Uncovered ? INDEX_NONE : int8(FMath::Min(Frame.Band(Heights[Index]), int32(UE_ARRAY_COUNT(FloorGrays)) - 1));
        // An outline centred on every pixel next to another band, a height step or the coverage edge (the image border included).
        const float Radius = OutlineWidth * Supersampling * .5f;
        const int32 Reach = FMath::CeilToInt32(Radius);
        TArray<FIntPoint> Disc;
        for (int32 DY = -Reach; DY <= Reach; ++DY)
            for (int32 DX = -Reach; DX <= Reach; ++DX)
                if (DX * DX + DY * DY <= Radius * Radius) Disc.Add(FIntPoint(DX, DY));
        TBitArray<> Outline(false, Full * Full);
        const FIntPoint Neighbours[] = { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) };
        for (int32 Y = 0; Y < Full; ++Y)
            for (int32 X = 0; X < Full; ++X)
            {
                const int32 Index = Y * Full + X;
                if (Bands[Index] == INDEX_NONE) continue;
                const bool bEdge = Algo::AnyOf(Neighbours, [&](const FIntPoint& Offset)
                {
                    const int32 NX = X + Offset.X, NY = Y + Offset.Y;
                    if (NX < 0 || NY < 0 || NX >= Full || NY >= Full) return true;
                    const int32 Other = NY * Full + NX;
                    return Bands[Other] != Bands[Index] || FMath::Abs(Heights[Other] - Heights[Index]) > StepHeight;
                });
                if (!bEdge) continue;
                for (const FIntPoint& Offset : Disc)
                {
                    const int32 OX = X + Offset.X, OY = Y + Offset.Y;
                    if (OX >= 0 && OY >= 0 && OX < Full && OY < Full) Outline[OY * Full + OX] = true;
                }
            }

        TArray<FColor> Pixels;
        Pixels.SetNumZeroed(ImageSide * ImageSide);
        for (int32 Y = 0; Y < ImageSide; ++Y)
            for (int32 X = 0; X < ImageSide; ++X)
            {
                uint32 Alpha = 0, R = 0, G = 0, B = 0;
                for (int32 SY = 0; SY < Supersampling; ++SY)
                    for (int32 SX = 0; SX < Supersampling; ++SX)
                    {
                        const int32 Index = (Y * Supersampling + SY) * Full + X * Supersampling + SX;
                        if (Outline[Index]) { R += OutlineColor.R; G += OutlineColor.G; B += OutlineColor.B; Alpha += 255; }
                        else if (Bands[Index] != INDEX_NONE) { const uint32 Gray = FloorGrays[Bands[Index]]; R += Gray; G += Gray; B += Gray; Alpha += 255; }
                    }
                if (Alpha == 0) continue;
                // Opaque samples only, so the colour is their plain average; coverage becomes alpha.
                const uint32 Opaque = Alpha / 255;
                Pixels[Y * ImageSide + X] = FColor(uint8(R / Opaque), uint8(G / Opaque), uint8(B / Opaque),
                    uint8(FMath::RoundToInt32(float(Alpha) / (Supersampling * Supersampling) * ArtOpacity)));
            }
        return Pixels;
    }

    /** The asset named by its package, created when missing; an earlier bake is replaced in place so references stay valid. */
    template<class T>
    T* FindOrCreateAsset(const FString& PackageName, FString& Error)
    {
        UPackage* Package = FPackageName::DoesPackageExist(PackageName) ? LoadPackage(nullptr, *PackageName, LOAD_None) : CreatePackage(*PackageName);
        if (!Package) { Error = FString::Printf(TEXT("could not open %s"), *PackageName); return nullptr; }
        const FString Name = FPackageName::GetShortName(PackageName);
        if (UObject* Existing = StaticFindObject(UObject::StaticClass(), Package, *Name))
        {
            if (T* Typed = Cast<T>(Existing)) return Typed;
            Error = FString::Printf(TEXT("%s exists and is not a %s; rename it, the bake owns this name"), *PackageName, *T::StaticClass()->GetName());
            return nullptr;
        }
        T* Asset = NewObject<T>(Package, *Name, RF_Public | RF_Standalone);
        FAssetRegistryModule::AssetCreated(Asset);
        return Asset;
    }

    bool SaveAsset(UObject& Asset, FString& Error)
    {
        UPackage* Package = Asset.GetPackage();
        Package->MarkPackageDirty();
        const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        if (UPackage::SavePackage(Package, &Asset, *Filename, Args)) return true;
        Error = FString::Printf(TEXT("could not save %s"), *Filename);
        return false;
    }
}

TArray<const ARetakeMinimapCapture*> RetakeMinimapBaker::FindCaptures(const UWorld& World)
{
    TArray<const ARetakeMinimapCapture*> Captures;
    if (World.PersistentLevel)
        for (const AActor* Actor : World.PersistentLevel->Actors)
            if (const ARetakeMinimapCapture* Capture = Cast<ARetakeMinimapCapture>(Actor)) Captures.Add(Capture);
    return Captures;
}

FString RetakeMinimapBaker::ComputeFingerprint(const UWorld& World, const ARetakeMinimapCapture& Capture)
{
    const FCaptureFrame Frame(Capture);
    TArray<FTriangle> Triangles;
    GatherTriangles(World, Capture, Frame, Triangles);
    return Fingerprint(Capture, Frame, Triangles);
}

bool RetakeMinimapBaker::IsCustom(const URetakeMinimapDefinition& Definition)
{
    return Definition.GetPackage()->GetMetaData().GetValue(&Definition, AuthoringModeKey) == TEXT("Custom");
}

FString RetakeMinimapBaker::CustomFingerprint(const UWorld& World, const ARetakeMinimapCapture& Capture, const UTexture2D& Texture)
{
    const FString Input = TEXT("RetakeCustomMinimap1|") + ComputeFingerprint(World,Capture) + TEXT("|")
        + Texture.GetPathName() + TEXT("|") + Texture.Source.GetId().ToString(EGuidFormats::Digits);
    const FTCHARToUTF8 Bytes(*Input); FBlake3 Hash; Hash.Update(Bytes.Get(),Bytes.Length());
    return LexToString(Hash.Finalize()).ToLower();
}

bool RetakeMinimapBaker::ApplyCustom(const UWorld& World, const ARetakeMinimapCapture& Capture, UTexture2D& Texture,
    const FString& ModRoot, const FText& DisplayName, FString& OutDefinition, FString& Error)
{
    if(!Capture.ValidateCapture(Error))return false;
    if(!Texture.GetPackage()->GetName().StartsWith(ModRoot) || Texture.Source.GetSizeX()!=ImageSide || Texture.Source.GetSizeY()!=ImageSide)
    {Error=TEXT("Choose a 1024 x 1024 texture inside this mod. Import your PNG in its Content Browser first.");return false;}
    const FString Map=FPackageName::GetShortName(World.GetPackage()->GetName());
    URetakeMinimapDefinition* Definition=FindOrCreateAsset<URetakeMinimapDefinition>(ModRoot+TEXT("Minimap/DA_Minimap_")+Map,Error);
    if(!Definition)return false;
    Texture.Modify(); Texture.PreEditChange(nullptr);
    Texture.CompressionSettings=TC_EditorIcon;Texture.LODGroup=TEXTUREGROUP_UI;Texture.MipGenSettings=TMGS_NoMipmaps;
    Texture.NeverStream=true;Texture.SRGB=true;Texture.Filter=TF_Bilinear;
    Texture.PostEditChange();FTextureCompilingManager::Get().FinishCompilation({&Texture});
    const FCaptureFrame Frame(Capture);Definition->Modify();
    Definition->World=TSoftObjectPtr<UWorld>(FSoftObjectPath(World.GetPathName()));Definition->Floorplan=&Texture;
    Definition->DisplayName=DisplayName;Definition->WorldCenter=Frame.Center;Definition->WorldSpan=Frame.Span;
    Definition->NorthYaw=Frame.Yaw;Definition->ElevationDatum=Frame.Datum;
    Definition->GeometryFingerprint=CustomFingerprint(World,Capture,Texture);
    Definition->GetPackage()->GetMetaData().SetValue(Definition,AuthoringModeKey,TEXT("Custom"));
    if(!Definition->ValidateDefinition(Error)||!SaveAsset(Texture,Error)||!SaveAsset(*Definition,Error))return false;
    OutDefinition=Definition->GetPathName();
    UE_LOGF(LogRetakeMapSDK,Display,"event=CustomMinimapApplied map=%ls texture=%ls definition=%ls",*Map,*Texture.GetPathName(),*OutDefinition);
    return true;
}

bool RetakeMinimapBaker::Bake(const UWorld& World, const ARetakeMinimapCapture& Capture, const FString& ModRoot, const FText& DisplayName,
    FString& OutDefinition, FString& Error, bool bReplaceCustom)
{
    if (!Capture.ValidateCapture(Error)) return false;
    const FString DefinitionPackage = ModRoot + TEXT("Minimap/DA_Minimap_") + FPackageName::GetShortName(World.GetPackage()->GetName());
    if(FPackageName::DoesPackageExist(DefinitionPackage))
        if(const auto* Existing=LoadObject<URetakeMinimapDefinition>(nullptr,*(DefinitionPackage+TEXT(".")+FPackageName::GetShortName(DefinitionPackage))); Existing && IsCustom(*Existing) && !bReplaceCustom)
        {Error=TEXT("This map uses a custom minimap. Review/apply its image and alignment; automatic preparation never overwrites custom artwork.");return false;}
    const FCaptureFrame Frame(Capture);
    TArray<FTriangle> Triangles;
    GatherTriangles(World, Capture, Frame, Triangles);
    if (Triangles.IsEmpty()) { Error = TEXT("the capture square holds no upward faces of visible static meshes below Capture Height"); return false; }
    const TArray<FColor> Pixels = Rasterize(Frame, Triangles);

    const FString Map = FPackageName::GetShortName(World.GetPackage()->GetName());
    const FString Folder = ModRoot + TEXT("Minimap/");
    UTexture2D* Floorplan = FindOrCreateAsset<UTexture2D>(Folder + TEXT("T_Minimap_") + Map, Error);
    if (!Floorplan) return false;
    Floorplan->PreEditChange(nullptr);
    Floorplan->Source.Init(ImageSide, ImageSide, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Pixels.GetData()));
    // The UI texture settings the validator requires: uncompressed, no mips, always resident, sRGB.
    Floorplan->CompressionSettings = TC_EditorIcon;
    Floorplan->LODGroup = TEXTUREGROUP_UI;
    Floorplan->MipGenSettings = TMGS_NoMipmaps;
    Floorplan->NeverStream = true;
    Floorplan->SRGB = true;
    Floorplan->Filter = TF_Bilinear;
    Floorplan->PostEditChange();
    FTextureCompilingManager::Get().FinishCompilation({ Floorplan });

    URetakeMinimapDefinition* Definition = FindOrCreateAsset<URetakeMinimapDefinition>(Folder + TEXT("DA_Minimap_") + Map, Error);
    if (!Definition) return false;
    Definition->World = TSoftObjectPtr<UWorld>(FSoftObjectPath(World.GetPathName()));
    Definition->Floorplan = Floorplan;
    Definition->DisplayName = DisplayName;
    Definition->WorldCenter = Frame.Center;
    Definition->WorldSpan = Frame.Span;
    Definition->NorthYaw = Frame.Yaw;
    Definition->ElevationDatum = Frame.Datum;
    Definition->GeometryFingerprint = Fingerprint(Capture, Frame, Triangles);
    Definition->GetPackage()->GetMetaData().SetValue(Definition,AuthoringModeKey,TEXT("Generated"));
    if (!Definition->ValidateDefinition(Error) || !SaveAsset(*Floorplan, Error) || !SaveAsset(*Definition, Error)) return false;
    OutDefinition = Definition->GetPathName();

    TArray64<uint8> Png;
    FImageUtils::PNGCompressImageArray(ImageSide, ImageSide, Pixels, Png);
    const FString Preview = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("RetakeMinimap"),
        FString::Printf(TEXT("%s_%s.png"), *ModRoot.Mid(1, ModRoot.Len() - 2), *Map));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Preview), true);
    const bool bPreview = FFileHelper::SaveArrayToFile(Png, *Preview);
    UE_LOGF(LogRetakeMapSDK, Display, "event=MinimapBaked map=%ls triangles=%d definition=%ls fingerprint=%ls preview=\"%ls\" previewWritten=%d",
        *Map, Triangles.Num(), *OutDefinition, *Definition->GeometryFingerprint, *FPaths::ConvertRelativePathToFull(Preview), bPreview ? 1 : 0);
    return true;
}
