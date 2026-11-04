// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeAcousticMapData.h"
#include "Misc/DataValidation.h"

bool FRetakeAcousticMaterial::IsValid() const
{
    return FMath::IsFinite(Transmission) && Transmission >= 0 && Transmission <= 1
        && FMath::IsFinite(LowPassHz) && LowPassHz >= 20 && LowPassHz <= 20000;
}

bool FRetakeAcousticRegionData::Contains(const FVector& Position) const
{
    const FVector Local = Transform.InverseTransformPosition(Position);
    return FMath::Abs(Local.X) <= Extent.X && FMath::Abs(Local.Y) <= Extent.Y && FMath::Abs(Local.Z) <= Extent.Z;
}

int32 URetakeAcousticMapData::FindRegion(const FVector& Position) const
{
    if (Position.ContainsNaN()) return INDEX_NONE;
    int32 Found = INDEX_NONE;
    double Volume = TNumericLimits<double>::Max();
    for (int32 Index = 0; Index < Regions.Num(); ++Index)
    {
        const auto& Region = Regions[Index];
        if (!Region.Contains(Position)) continue;
        const double CandidateVolume = Region.Extent.X * Region.Extent.Y * Region.Extent.Z * FMath::Abs(Region.Transform.GetDeterminant());
        if (Found == INDEX_NONE || Region.Priority > Regions[Found].Priority
            || (Region.Priority == Regions[Found].Priority && CandidateVolume < Volume))
        { Found = Index; Volume = CandidateVolume; }
    }
    return Found;
}

bool URetakeAcousticMapData::Validate(FString& Error) const
{
    if (Version != FormatVersion || MapPackage.IsEmpty() || SourceSignature.Len() != 40
        || Regions.IsEmpty() || Regions.Num() > MaximumRegions || Connections.Num() > MaximumConnections)
    { Error = TEXT("Missing, stale, unsupported or oversized acoustic map bake."); return false; }
    TSet<FName> Ids;
    for (int32 RegionIndex = 0; RegionIndex < Regions.Num(); ++RegionIndex)
    {
        const auto& Region = Regions[RegionIndex];
        const FVector Scale = Region.Transform.GetScale3D();
        if (Region.Id.IsNone() || Ids.Contains(Region.Id) || Region.Transform.ContainsNaN() || Region.Extent.ContainsNaN()
            || Region.Extent.GetMin() <= 0 || Scale.GetAbs().GetMin() <= UE_SMALL_NUMBER || !Region.InternalTransmission.IsValid()
            || !FMath::IsFinite(Region.ReverbDecaySeconds) || Region.ReverbDecaySeconds < .1f || Region.ReverbDecaySeconds > 5.f)
        { Error = TEXT("Invalid or duplicate acoustic region."); return false; }
        Ids.Add(Region.Id);
        TSet<int32> Links;
        for (int32 Index : Region.Connections)
        {
            if (!Connections.IsValidIndex(Index) || Links.Contains(Index)
                || (Connections[Index].RegionA != RegionIndex && Connections[Index].RegionB != RegionIndex))
            { Error = TEXT("Invalid acoustic region adjacency index."); return false; }
            Links.Add(Index);
        }
    }
    Ids.Reset();
    for (int32 ConnectionIndex = 0; ConnectionIndex < Connections.Num(); ++ConnectionIndex)
    {
        const auto& Connection = Connections[ConnectionIndex];
        if (Connection.Id.IsNone() || Ids.Contains(Connection.Id) || Connection.RegionA == Connection.RegionB
            || !Regions.IsValidIndex(Connection.RegionA) || !Regions.IsValidIndex(Connection.RegionB)
            || Connection.Position.ContainsNaN() || !Connection.OpenMaterial.IsValid() || !Connection.ClosedMaterial.IsValid())
        { Error = TEXT("Invalid or duplicate acoustic connection."); return false; }
        Ids.Add(Connection.Id);
        if (!Regions[Connection.RegionA].Connections.Contains(ConnectionIndex) || !Regions[Connection.RegionB].Connections.Contains(ConnectionIndex))
        { Error = TEXT("Acoustic connection is missing from its region adjacency."); return false; }
    }
    return true;
}

FRetakeAcousticPathResult RetakeAcousticGraph::Resolve(const URetakeAcousticMapData& Map, TConstArrayView<uint8> OpenStates,
    const FVector& Source, const FVector& Listener, const FRetakeAudioPropagationProfile& Profile)
{
    FRetakeAcousticPathResult Result;
    const int32 Start = Map.FindRegion(Source), Finish = Map.FindRegion(Listener);
    if (Start == INDEX_NONE || Finish == INDEX_NONE || OpenStates.Num() != Map.Connections.Num()
        || FVector::DistSquared(Source, Listener) >= FMath::Square(Profile.RangeCm)) return Result;
    if (Start == Finish)
    {
        Result.DistanceCm = FVector::Distance(Source, Listener);
        const auto& Material = Map.Regions[Start].InternalTransmission;
        Result.Gain = Profile.GainAtDistance(Result.DistanceCm) * Material.Transmission;
        Result.LowPassHz = Material.LowPassHz;
        return Result;
    }
    struct FNode
    {
        int32 Region = INDEX_NONE;
        FVector Position = FVector::ZeroVector;
        float Distance = 0, Transmission = 1, LowPass = 20000, UpperGain = 1;
        int32 Hops = 0;
        int32 Visited[MaximumHops + 1]{};
    };
    TArray<FNode, TInlineAllocator<MaximumSearchNodes>> Pending;
    FNode Root; Root.Region = Start; Root.Position = Source; Root.Visited[0] = Start;
    Pending.Add(Root);
    int32 Expanded = 0, Alternatives = 0;
    while (!Pending.IsEmpty() && Expanded < MaximumSearchNodes && Alternatives < MaximumAlternatives)
    {
        int32 Best = 0;
        for (int32 Index = 1; Index < Pending.Num(); ++Index)
            if (Pending[Index].UpperGain > Pending[Best].UpperGain) Best = Index;
        const FNode Node = Pending[Best]; Pending.RemoveAtSwap(Best, EAllowShrinking::No);
        ++Expanded;
        if (Node.Region == Finish)
        {
            ++Alternatives;
            const float Distance = Node.Distance + FVector::Distance(Node.Position, Listener);
            const float Gain = Profile.GainAtDistance(Distance) * Node.Transmission;
            if (Gain > Result.Gain)
            { Result.Gain = Gain; Result.LowPassHz = Node.LowPass; Result.DistanceCm = Distance; Result.Connections = Node.Hops; }
            continue;
        }
        if (Node.Hops == MaximumHops || Node.UpperGain <= Result.Gain) continue;
        for (int32 Index : Map.Regions[Node.Region].Connections)
        {
            const auto& Edge = Map.Connections[Index];
            const int32 Next = Edge.RegionA == Node.Region ? Edge.RegionB : Edge.RegionB == Node.Region ? Edge.RegionA : INDEX_NONE;
            if (Next == INDEX_NONE) continue;
            bool Seen = false;
            for (int32 Hop = 0; Hop <= Node.Hops; ++Hop) Seen |= Node.Visited[Hop] == Next;
            if (Seen) continue;
            const auto& Material = OpenStates[Index] ? Edge.OpenMaterial : Edge.ClosedMaterial;
            FNode Candidate = Node;
            Candidate.Region = Next; Candidate.Position = Edge.Position;
            Candidate.Distance += FVector::Distance(Node.Position, Edge.Position);
            Candidate.Transmission *= Material.Transmission;
            Candidate.LowPass = FMath::Min(Candidate.LowPass, Material.LowPassHz);
            Candidate.Visited[++Candidate.Hops] = Next;
            Candidate.UpperGain = Profile.GainAtDistance(Candidate.Distance + FVector::Distance(Edge.Position, Listener)) * Candidate.Transmission;
            if (Candidate.UpperGain <= FMath::Max(.001f, Result.Gain)) continue;
            if (Pending.Num() == MaximumSearchNodes) { Result.bSearchLimitReached = true; continue; }
            Pending.Add(Candidate);
        }
    }
    Result.bSearchLimitReached |= Expanded == MaximumSearchNodes && !Pending.IsEmpty();
    return Result;
}

#if WITH_EDITOR
EDataValidationResult URetakeAcousticMapData::IsDataValid(FDataValidationContext& Context) const
{
    FString Error;
    if (!Validate(Error)) { Context.AddError(FText::FromString(Error)); return EDataValidationResult::Invalid; }
    return EDataValidationResult::Valid;
}
#endif
