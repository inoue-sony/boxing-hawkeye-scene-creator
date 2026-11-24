#include "HUMAClothNativeBake.h"

#if WITH_EDITOR
#include "HUMAClothBindAsset.h"
#include "HUMAClothNumpyPermutation.h"
#include "Async/ParallelFor.h"
#include "Distance/DistPoint3Triangle3.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "MeshQueries.h"
#include "Misc/AutomationTest.h"
#include <algorithm>

namespace
{
constexpr int32 K = 12;
constexpr int32 Levels = 7;
constexpr double H0 = 1.5;
struct FEdge { int32 Vertex; double Length; };
struct FVisit
{
    double Distance;
    int32 Vertex;
    bool operator<(const FVisit& Other) const
    {
        return Distance == Other.Distance ? Vertex < Other.Vertex : Distance < Other.Distance;
    }
};
using FGraph = TArray<TArray<FEdge>>;

template<typename Visitor>
void Walk(const FGraph& Graph, int32 Start, double Limit, Visitor&& Visit)
{
    TArray<FVisit> Queue;
    TMap<int32, double> Distances;
    Queue.HeapPush({0.0, Start});
    Distances.Add(Start, 0.0);
    while (!Queue.IsEmpty())
    {
        FVisit Next;
        Queue.HeapPop(Next, EAllowShrinking::No);
        if (Next.Distance != Distances[Next.Vertex]) continue;
        Visit(Next.Vertex, Next.Distance);
        for (const FEdge& Edge : Graph[Next.Vertex])
        {
            const double Distance = Next.Distance + Edge.Length;
            if (Distance > Limit) continue;
            const double* Old = Distances.Find(Edge.Vertex);
            if (!Old || Distance < *Old)
            {
                Distances.Add(Edge.Vertex, Distance);
                Queue.HeapPush({Distance, Edge.Vertex});
            }
        }
    }
}

class FBodySampleTree
{
    TConstArrayView<FVector> Body;
    TArray<int32> Order;

    void Build(int32 Begin, int32 End, int32 Depth)
    {
        if (Begin >= End) return;
        const int32 Middle = (Begin + End) / 2, Axis = Depth % 3;
        std::nth_element(Order.GetData() + Begin, Order.GetData() + Middle, Order.GetData() + End,
            [&](int32 A, int32 B) { return Body[A][Axis] == Body[B][Axis] ? A < B : Body[A][Axis] < Body[B][Axis]; });
        Build(Begin, Middle, Depth + 1); Build(Middle + 1, End, Depth + 1);
    }

    void Query(const FVector& Point, int32 Begin, int32 End, int32 Depth, TArray<FVisit>& Nearest) const
    {
        if (Begin >= End) return;
        const int32 Middle = (Begin + End) / 2, Vertex = Order[Middle];
        const FVisit Candidate{(Point - Body[Vertex]).SizeSquared(), Vertex};
        int32 Slot = 0;
        while (Slot < Nearest.Num() && Nearest[Slot] < Candidate) ++Slot;
        if (Slot <= K)
        {
            Nearest.Insert(Candidate, Slot);
            if (Nearest.Num() > K + 1) Nearest.Pop(EAllowShrinking::No);
        }
        const double Delta = Point[Depth % 3] - Body[Vertex][Depth % 3];
        const bool Left = Delta <= 0;
        Query(Point, Left ? Begin : Middle + 1, Left ? Middle : End, Depth + 1, Nearest);
        if (Nearest.Num() < K + 1 || Delta * Delta <= Nearest.Last().Distance)
            Query(Point, Left ? Middle + 1 : Begin, Left ? End : Middle, Depth + 1, Nearest);
    }

public:
    FBodySampleTree(TConstArrayView<FVector> Positions, TArray<int32> Samples) : Body(Positions), Order(MoveTemp(Samples))
    {
        Build(0, Order.Num(), 0);
    }
    TArray<FVisit> Query(const FVector& Point) const
    {
        TArray<FVisit> Result;
        Result.Reserve(K + 2);
        Query(Point, 0, Order.Num(), 0, Result);
        for (FVisit& Visit : Result) Visit.Distance = FMath::Sqrt(Visit.Distance);
        return Result;
    }
};

bool BuildWeights(UHUMAClothBindAsset& Output, const TArray<FIntVector>& FirstTriangles,
    TConstArrayView<double> Distances, float SigmaMin, float DistanceScale, TArray<int32>& Active, FString& Error)
{
    const int32 Count = Output.ClothRestPositions.Num(), BodyCount = Output.RestBody.Num();
    FGraph Graph;
    Graph.SetNum(BodyCount);
    TSet<uint64> Seen;
    for (int32 Offset = 0; Offset < Output.BodyQuads.Num(); Offset += 4)
        for (int32 Corner = 0; Corner < 4; ++Corner)
        {
            const int32 First = Output.BodyQuads[Offset + Corner], Second = Output.BodyQuads[Offset + (Corner + 1) % 4];
            if (First == Second) continue;
            const uint64 Key = (uint64(FMath::Min(First, Second)) << 32) | uint32(FMath::Max(First, Second));
            if (Seen.Contains(Key)) continue;
            Seen.Add(Key);
            const double Length = FMath::Max((Output.RestBody[First] - Output.RestBody[Second]).Size(), 1.e-4);
            Graph[First].Add({Second, Length}); Graph[Second].Add({First, Length});
        }
    TArray<int32> Low, High, Order;
    TArray<double> Fraction;
    for (int32 Vertex = 0; Vertex < BodyCount; ++Vertex) Order.Add(Vertex);
    HUMACloth::NumpyPermutation(BodyCount, [&](int32 A, int32 B) { Order.Swap(A, B); });
    for (int32 Vertex = 0; Vertex < Count; ++Vertex)
    {
        const double Sigma = FMath::Max(double(SigmaMin), double(DistanceScale) * Distances[Vertex]);
        const double Value = FMath::Clamp(FMath::Log2(1.2 * Sigma / H0) + 1.0, 0.0, double(Levels - 1));
        Low.Add(FMath::FloorToInt(Value)); High.Add(FMath::Min(Low.Last() + 1, Levels - 1));
        Fraction.Add(Low.Last() == High.Last() ? 0.0 : Value - Low.Last());
    }
    Output.Slots = 2 * K;
    Output.InfluenceAnchors.Init(0, Count * Output.Slots);
    TArray<double> Weights;
    Weights.Init(0.0, Count * Output.Slots);
    for (int32 Level = 0; Level < Levels; ++Level)
    {
        if (!Low.Contains(Level) && !High.Contains(Level)) continue;
        const double H = H0 * FMath::Pow(2.0, FMath::Max(0, Level - 1));
        TArray<int32> Samples;
        TBitArray<> Covered(false, BodyCount);
        for (int32 Index = 0; Index < BodyCount; ++Index)
        {
            const int32 Vertex = Level == 0 ? Index : Order[Index];
            if (Level > 0 && Covered[Vertex]) continue;
            if (Level > 0) Walk(Graph, Vertex, H, [&](int32 Reached, double) { Covered[Reached] = true; });
            if (FirstTriangles[Vertex].X != INDEX_NONE) Samples.Add(Vertex);
        }
        if (Samples.IsEmpty()) { Error = TEXT("Body field has no usable samples."); return false; }
        const FBodySampleTree Tree(Output.RestBody, MoveTemp(Samples));
        ParallelFor(Count, [&](int32 Vertex)
        {
            if (Low[Vertex] != Level && High[Vertex] != Level) return;
            const TArray<FVisit> Nearest = Tree.Query(Output.ClothRestPositions[Vertex]);
            const double First = Nearest[0].Distance;
            const double D13 = Nearest.Num() > K ? Nearest[K].Distance : TNumericLimits<double>::Max();
            const double Support = FMath::Max(FMath::Min(D13, FMath::Max(3.5 * H, First + H)),
                First + FMath::Max(H * 1.e-3, 1.e-6));
            double Local[K] = {}, Sum = 0;
            for (int32 Neighbor = 0; Neighbor < FMath::Min(K, Nearest.Num()); ++Neighbor)
            {
                const double Ratio = Nearest[Neighbor].Distance / Support;
                Local[Neighbor] = Ratio < 1.0 ? FMath::Square(1.0 - Ratio * Ratio) : 0.0;
                Sum += Local[Neighbor];
            }
            for (int32 Slot = 0; Slot < 2; ++Slot)
            {
                if ((Slot == 0 ? Low[Vertex] : High[Vertex]) != Level) continue;
                const double Scale = Slot == 0 ? 1.0 - Fraction[Vertex] : Fraction[Vertex];
                for (int32 Neighbor = 0; Neighbor < K; ++Neighbor)
                {
                    const int32 Index = Vertex * 2 * K + Slot * K + Neighbor;
                    Output.InfluenceAnchors[Index] = Nearest.IsValidIndex(Neighbor) ? Nearest[Neighbor].Vertex : 0;
                    Weights[Index] = Local[Neighbor] / Sum * Scale;
                }
            }
        });
    }
    TBitArray<> Used(false, BodyCount);
    Output.InfluenceWeights.SetNumUninitialized(Weights.Num());
    for (int32 Vertex = 0; Vertex < Count; ++Vertex)
    {
        double Sum = 0;
        for (int32 Slot = 0; Slot < Output.Slots; ++Slot) Sum += Weights[Vertex * Output.Slots + Slot];
        if (!FMath::IsFinite(Sum) || Sum <= 0) { Error = TEXT("Body field has no finite influence support."); return false; }
        for (int32 Slot = 0; Slot < Output.Slots; ++Slot)
        {
            const int32 Index = Vertex * Output.Slots + Slot;
            Output.InfluenceWeights[Index] = float(Weights[Index] / Sum);
            if (Weights[Index] > 0) Used[Output.InfluenceAnchors[Index]] = true;
        }
    }
    TArray<int32> Mapping;
    Mapping.Init(INDEX_NONE, BodyCount);
    for (int32 Vertex = 0; Vertex < BodyCount; ++Vertex)
        if (Used[Vertex]) Mapping[Vertex] = Active.Add(Vertex);
    for (int32 Index = 0; Index < Weights.Num(); ++Index)
        Output.InfluenceAnchors[Index] = Weights[Index] > 0 ? Mapping[Output.InfluenceAnchors[Index]] : 0;
    return true;
}

void BuildClothGraph(const UHUMAClothBindAsset& Output, TConstArrayView<int32> IDs,
    TArray<int32>& RenderToGraph, TArray<int32>& Active, FGraph& Graph)
{
    const int32 Count = Output.ClothRestPositions.Num();
    TArray<FVector> Keys;
    TArray<int32> Order;
    for (int32 Index = 0; Index < Count; ++Index)
    {
        const FVector Position = Output.ClothRestPositions[Index] * 1.e5;
        Keys.Add(IDs.IsEmpty() ? FVector(FMath::RoundHalfToEven(Position.X),
            FMath::RoundHalfToEven(Position.Y), FMath::RoundHalfToEven(Position.Z)) : FVector(double(IDs[Index]), 0, 0));
        Order.Add(Index);
    }
    Order.Sort([&](int32 A, int32 B)
    {
        for (int32 Axis = 0; Axis < 3; ++Axis)
            if (Keys[A][Axis] != Keys[B][Axis]) return Keys[A][Axis] < Keys[B][Axis];
        return A < B;
    });
    RenderToGraph.SetNum(Count);
    for (int32 Index : Order)
    {
        if (Active.IsEmpty() || Keys[Index] != Keys[Active.Last()]) Active.Add(Index);
        RenderToGraph[Index] = Active.Num() - 1;
    }
    Graph.SetNum(Active.Num());
    TSet<uint64> Seen;
    for (int32 Offset = 0; Offset < Output.ClothTriangles.Num(); Offset += 3)
        for (int32 Corner = 0; Corner < 3; ++Corner)
        {
            const int32 A = RenderToGraph[Output.ClothTriangles[Offset + Corner]];
            const int32 B = RenderToGraph[Output.ClothTriangles[Offset + (Corner + 1) % 3]];
            if (A == B) continue;
            const uint64 Key = (uint64(FMath::Min(A, B)) << 32) | uint32(FMath::Max(A, B));
            if (Seen.Contains(Key)) continue;
            Seen.Add(Key);
            const double Length = FMath::Max((Output.ClothRestPositions[Active[A]] - Output.ClothRestPositions[Active[B]]).Size(), 1.e-4);
            Graph[A].Add({B, Length}); Graph[B].Add({A, Length});
        }
}

bool BuildClothWeights(UHUMAClothBindAsset& Output, const FGraph& Graph,
    const TArray<int32>& RenderToGraph, const TArray<int32>& Active,
    TConstArrayView<double> Distances, float SigmaMin, float DistanceScale, FString& Error)
{
    const int32 Count = Active.Num();
    TArray<int32> Low, High, Order;
    TArray<double> Fraction, Weights;
    TArray<int32> Anchors;
    Anchors.Init(0, Count * 2 * K); Weights.Init(0.0, Anchors.Num());
    for (int32 Vertex = 0; Vertex < Count; ++Vertex)
    {
        Order.Add(Vertex);
        const double Sigma = FMath::Max(double(SigmaMin), double(DistanceScale) * Distances[Active[Vertex]]);
        const double Value = FMath::Clamp(FMath::Log2(1.2 * Sigma / H0) + 1.0, 0.0, double(Levels - 1));
        Low.Add(FMath::FloorToInt(Value)); High.Add(FMath::Min(Low.Last() + 1, Levels - 1));
        Fraction.Add(Low.Last() == High.Last() ? 0.0 : Value - Low.Last());
    }
    HUMACloth::NumpyPermutation(Count, [&](int32 A, int32 B) { Order.Swap(A, B); });
    for (int32 Level = 0; Level < Levels; ++Level)
    {
        if (!Low.Contains(Level) && !High.Contains(Level)) continue;
        const double H = H0 * FMath::Pow(2.0, FMath::Max(0, Level - 1)), Radius = Level == 0 ? 1.0 : 3.5 * H;
        TArray<TArray<FVisit>> Nearest;
        Nearest.SetNum(Count);
        TBitArray<> Covered(false, Count);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            const int32 Sample = Level == 0 ? Index : Order[Index];
            if (Level > 0 && Covered[Sample]) continue;
            if (Level == 0) { Nearest[Sample].Add({0.0, Sample}); continue; }
            Walk(Graph, Sample, H, [&](int32 Vertex, double) { Covered[Vertex] = true; });
            Walk(Graph, Sample, Radius, [&](int32 Vertex, double Distance)
            {
                auto& Row = Nearest[Vertex];
                const FVisit Candidate{Distance, Sample};
                int32 Slot = 0;
                while (Slot < Row.Num() && Row[Slot] < Candidate) ++Slot;
                if (Slot <= K)
                {
                    Row.Insert(Candidate, Slot);
                    if (Row.Num() > K + 1) Row.Pop(EAllowShrinking::No);
                }
            });
        }
        for (int32 Vertex = 0; Vertex < Count; ++Vertex)
        {
            if (Low[Vertex] != Level && High[Vertex] != Level) continue;
            const auto& Row = Nearest[Vertex];
            if (Row.IsEmpty()) { Error = TEXT("Cloth graph has no reachable anchor."); return false; }
            const double Support = FMath::Max(Row.Num() > K ? FMath::Min(Row[K].Distance, Radius) : Radius, 1.e-9);
            double Local[K] = {}, Sum = 0;
            for (int32 Neighbor = 0; Neighbor < FMath::Min(K, Row.Num()); ++Neighbor)
            {
                const double Ratio = Row[Neighbor].Distance / Support;
                Local[Neighbor] = Ratio < 1.0 ? FMath::Square(1.0 - Ratio * Ratio) : 0.0;
                Sum += Local[Neighbor];
            }
            if (Sum <= 1.e-12) { Local[0] = 1.0; Sum = 1.0; }
            for (int32 Slot = 0; Slot < 2; ++Slot)
            {
                if ((Slot == 0 ? Low[Vertex] : High[Vertex]) != Level) continue;
                const double Scale = Slot == 0 ? 1.0 - Fraction[Vertex] : Fraction[Vertex];
                for (int32 Neighbor = 0; Neighbor < K; ++Neighbor)
                {
                    const int32 Index = Vertex * 2 * K + Slot * K + Neighbor;
                    Anchors[Index] = Row.IsValidIndex(Neighbor) ? Row[Neighbor].Vertex : 0;
                    Weights[Index] = Local[Neighbor] / Sum * Scale;
                }
            }
        }
    }
    Output.Slots = 2 * K;
    Output.InfluenceAnchors.SetNum(Output.ClothRestPositions.Num() * Output.Slots);
    Output.InfluenceWeights.SetNum(Output.InfluenceAnchors.Num());
    for (int32 Vertex = 0; Vertex < RenderToGraph.Num(); ++Vertex)
    {
        double Sum = 0;
        for (int32 Slot = 0; Slot < Output.Slots; ++Slot) Sum += Weights[RenderToGraph[Vertex] * Output.Slots + Slot];
        if (!FMath::IsFinite(Sum) || Sum <= 0) { Error = TEXT("Cloth graph has invalid influence support."); return false; }
        for (int32 Slot = 0; Slot < Output.Slots; ++Slot)
        {
            const int32 From = RenderToGraph[Vertex] * Output.Slots + Slot, To = Vertex * Output.Slots + Slot;
            Output.InfluenceAnchors[To] = Anchors[From];
            Output.InfluenceWeights[To] = float(Weights[From] / Sum);
        }
    }
    return true;
}

FVector Normalize(const FVector& Value, const FVector& Fallback)
{
    return Value.SizeSquared() > 1.e-16 ? Value / Value.Size() : Fallback;
}

void StoreAnchor(UHUMAClothBindAsset& Output, int32 Index, const FIntVector& Triangle,
    const FVector& Bary, const TArray<FVector>& Normals)
{
    const FVector A = Output.RestBody[Triangle.X], B = Output.RestBody[Triangle.Y], C = Output.RestBody[Triangle.Z];
    const FVector Normal = Normalize(Normals[Triangle.X] * Bary.X + Normals[Triangle.Y] * Bary.Y + Normals[Triangle.Z] * Bary.Z, FVector::ZAxisVector);
    FVector Tangent = B - A;
    Tangent -= Normal * FVector::DotProduct(Tangent, Normal);
    if (Tangent.SizeSquared() <= 1.e-16) { Tangent = C - A; Tangent -= Normal * FVector::DotProduct(Tangent, Normal); }
    if (Tangent.SizeSquared() <= 1.e-16) Tangent = FVector::CrossProduct(Normal, FMath::Abs(Normal.Z) > 0.8 ? FVector::XAxisVector : FVector::YAxisVector);
    Tangent = Normalize(Tangent, FVector::XAxisVector);
    const FVector Bitangent = Normalize(FVector::CrossProduct(Normal, Tangent), FVector::YAxisVector);
    Output.AnchorTriangles[Index] = Triangle;
    Output.AnchorBarycentrics[Index] = Bary;
    Output.AnchorRestPositions[Index] = A * Bary.X + B * Bary.Y + C * Bary.Z;
    Output.AnchorRestTangents[Index] = Normalize(FVector::CrossProduct(Bitangent, Normal), Tangent);
    Output.AnchorRestBitangents[Index] = Bitangent;
    Output.AnchorRestNormals[Index] = Normal;
}
}

bool BakeHUMANativeLOD(UHUMAClothBindAsset& Output, TConstArrayView<int32> BodyTriangles,
    TConstArrayView<int32> ClothVertexIDs, float SigmaMin, float DistanceScale, FString& Error,
    const UHUMAClothBindAsset* SharedWeights)
{
    if (!FMath::IsFinite(SigmaMin) || SigmaMin <= 0.f || !FMath::IsFinite(DistanceScale) || DistanceScale < 0.f ||
        Output.RestBody.IsEmpty() || Output.ClothRestPositions.IsEmpty() || BodyTriangles.Num() % 3 != 0 ||
        Output.BodyQuads.IsEmpty() || Output.BodyQuads.Num() % 4 != 0 || Output.ClothTriangles.Num() % 3 != 0 ||
        (!ClothVertexIDs.IsEmpty() && ClothVertexIDs.Num() != Output.ClothRestPositions.Num()))
    { Error = TEXT("Invalid native bake parameters or array sizes."); return false; }
    for (int32 Index : BodyTriangles)
        if (!Output.RestBody.IsValidIndex(Index)) { Error = TEXT("Invalid native body triangle."); return false; }
    for (int32 Index : Output.BodyQuads)
        if (!Output.RestBody.IsValidIndex(Index)) { Error = TEXT("Invalid native body quad."); return false; }
    for (int32 Index : Output.ClothTriangles)
        if (!Output.ClothRestPositions.IsValidIndex(Index)) { Error = TEXT("Invalid native garment triangle."); return false; }
    for (const FVector& Position : Output.RestBody)
        if (Position.ContainsNaN()) { Error = TEXT("Non-finite body position."); return false; }
    TMap<int32, FVector> IDPositions;
    for (int32 Index = 0; Index < Output.ClothRestPositions.Num(); ++Index)
    {
        const FVector Position = Output.ClothRestPositions[Index];
        if (Position.ContainsNaN()) { Error = TEXT("Non-finite garment position."); return false; }
        if (!ClothVertexIDs.IsEmpty())
        {
            const int32 ID = ClothVertexIDs[Index];
            const FVector* Existing = IDPositions.Find(ID);
            if (ID < 0 || (Existing && !Existing->Equals(Position, 1.e-3)))
            { Error = TEXT("Garment import IDs disagree with vertex positions."); return false; }
            IDPositions.Add(ID, Position);
        }
    }
    if (SharedWeights && (SharedWeights->Slots != 24 || SharedWeights->ClothRestPositions != Output.ClothRestPositions ||
        !SharedWeights->Validate(Error)))
    { if (Error.IsEmpty()) Error = TEXT("Body LODs require identical garment geometry and weights."); return false; }
    Output.BindMethod = SharedWeights ? SharedWeights->BindMethod : Output.GetRequestedBindMethod();
    Output.NormalizeBindMethod();
    if (Output.BindMethod != TEXT("body_field") && Output.BindMethod != TEXT("cloth_graph"))
    { Error = TEXT("Unsupported native Bind method."); return false; }
    Output.BakeMethod = Output.BindMethod == TEXT("body_field") ? EHUMAClothBindMethod::BodyField : EHUMAClothBindMethod::ClothGraph;
    Output.bBakeMethodInitialized = true;
    using namespace UE::Geometry;
    FDynamicMesh3 Mesh;
    for (const FVector& Position : Output.RestBody) Mesh.AppendVertex(Position);
    TArray<FIntVector> FirstTriangles;
    FirstTriangles.Init(FIntVector(INDEX_NONE), Output.RestBody.Num());
    // Quad fan order is the canonical Maya body_faces order, including padded triangles.
    for (int32 Offset = 0; Offset < Output.BodyQuads.Num(); Offset += 4)
        for (int32 Fan = 0; Fan < 2; ++Fan)
        {
            const int32* Quad = &Output.BodyQuads[Offset];
            const FIntVector Triangle(Quad[0], Quad[Fan ? 2 : 1], Quad[Fan ? 3 : 2]);
            if (FVector::CrossProduct(Output.RestBody[Triangle.Y] - Output.RestBody[Triangle.X],
                Output.RestBody[Triangle.Z] - Output.RestBody[Triangle.X]).SizeSquared() <= 1.e-20) continue;
            for (int32 Vertex : {Triangle.X, Triangle.Y, Triangle.Z})
                if (FirstTriangles[Vertex].X == INDEX_NONE) FirstTriangles[Vertex] = Triangle;
            if (Output.BindMethod == TEXT("body_field") && Mesh.AppendTriangle(Triangle.X, Triangle.Y, Triangle.Z) < 0)
            { Error = TEXT("Body bind topology is not a manifold triangle mesh."); return false; }
        }
    if (Output.BindMethod == TEXT("cloth_graph"))
        for (int32 Offset = 0; Offset < BodyTriangles.Num(); Offset += 3)
            if (Mesh.AppendTriangle(BodyTriangles[Offset], BodyTriangles[Offset + 1], BodyTriangles[Offset + 2]) < 0)
            { Error = TEXT("Body bind topology is not a manifold triangle mesh."); return false; }
    if (Mesh.TriangleCount() == 0) { Error = TEXT("Body bind topology has no usable triangles."); return false; }
    FDynamicMeshAABBTree3 Tree(&Mesh, true);
    TArray<FVector> Normals;
    Normals.Init(FVector::ZeroVector, Output.RestBody.Num());
    for (int32 Corner = 0; Corner < 4; ++Corner)
        for (int32 Offset = 0; Offset < Output.BodyQuads.Num(); Offset += 4)
        {
            const int32* Quad = &Output.BodyQuads[Offset];
            bool Duplicate = false;
            if (Output.BindMethod != TEXT("body_field"))
                for (int32 Previous = 0; Previous < Corner; ++Previous) Duplicate |= Quad[Previous] == Quad[Corner];
            if (!Duplicate) Normals[Quad[Corner]] += FVector::CrossProduct(Output.RestBody[Quad[2]] - Output.RestBody[Quad[0]], Output.RestBody[Quad[3]] - Output.RestBody[Quad[1]]);
        }
    for (FVector& Normal : Normals) Normal = Normalize(Normal, FVector::ZAxisVector);
    TArray<int32> Active;
    TArray<int32> RenderToGraph;
    FGraph ClothGraph;
    if (Output.BindMethod == TEXT("cloth_graph"))
        BuildClothGraph(Output, ClothVertexIDs, RenderToGraph, Active, ClothGraph);
    if (SharedWeights)
    {
        Output.Slots = SharedWeights->Slots;
        Output.InfluenceAnchors = SharedWeights->InfluenceAnchors;
        Output.InfluenceWeights = SharedWeights->InfluenceWeights;
    }
    else
    {
        TArray<double> Distances;
        Distances.SetNumUninitialized(Output.ClothRestPositions.Num());
        ParallelFor(Distances.Num(), [&](int32 Index)
        {
            double Squared;
            Tree.FindNearestTriangle(Output.ClothRestPositions[Index], Squared);
            Distances[Index] = FMath::Sqrt(Squared);
        });
        if (Output.BindMethod == TEXT("body_field"))
        {
            if (!BuildWeights(Output, FirstTriangles, Distances, SigmaMin, DistanceScale, Active, Error)) return false;
        }
        else if (!BuildClothWeights(Output, ClothGraph, RenderToGraph, Active, Distances, SigmaMin, DistanceScale, Error)) return false;
    }
    const int32 Count = SharedWeights ? SharedWeights->AnchorRestPositions.Num() : Active.Num();
    if (Output.BindMethod == TEXT("cloth_graph") && Count != Active.Num())
    { Error = TEXT("Shared cloth graph anchor count differs from welded garment vertices."); return false; }
    Output.AnchorTriangles.SetNum(Count); Output.AnchorBarycentrics.SetNum(Count);
    Output.AnchorRestPositions.SetNum(Count); Output.AnchorRestTangents.SetNum(Count);
    Output.AnchorRestBitangents.SetNum(Count); Output.AnchorRestNormals.SetNum(Count);
    ParallelFor(Count, [&](int32 Index)
    {
        if (SharedWeights || Output.BindMethod == TEXT("cloth_graph"))
        {
            const FVector Position = Output.BindMethod == TEXT("cloth_graph")
                ? Output.ClothRestPositions[Active[Index]] : SharedWeights->AnchorRestPositions[Index];
            double Squared;
            const int32 TriangleID = Tree.FindNearestTriangle(Position, Squared);
            auto Query = TMeshQueries<FDynamicMesh3>::TriangleDistance(Mesh, TriangleID, Position);
            Query.GetSquared();
            const auto Triangle = Mesh.GetTriangle(TriangleID);
            StoreAnchor(Output, Index, FIntVector(Triangle.A, Triangle.B, Triangle.C), FVector(Query.TriangleBaryCoords), Normals);
        }
        else
        {
            const int32 Vertex = Active[Index];
            const FIntVector Triangle = FirstTriangles[Vertex];
            StoreAnchor(Output, Index, Triangle, FVector(Triangle.X == Vertex, Triangle.Y == Vertex, Triangle.Z == Vertex), Normals);
        }
    });
    return Output.RunReferenceChecks();
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMANumpyPermutationTest, "HUMAClothBind.NativeBake.NumpyPermutation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHUMANumpyPermutationTest::RunTest(const FString&)
{
    const TArray<TArray<int32>> Vectors = {{}, {0}, {0,1}, {2,0,1}, {4,6,2,7,3,5,9,0,8,1},
        {2,10,3,11,0,4,7,5,16,12,13,6,9,14,8,1,15},
        {2,11,25,21,10,4,29,16,23,6,18,26,3,30,8,0,19,12,20,13,7,5,17,14,27,22,9,28,24,1,15,31}};
    for (const TArray<int32>& Expected : Vectors)
    {
        TArray<int32> Order;
        for (int32 Index = 0; Index < Expected.Num(); ++Index) Order.Add(Index);
        HUMACloth::NumpyPermutation(Order.Num(), [&](int32 A, int32 B) { Order.Swap(A, B); });
        TestTrue(*FString::Printf(TEXT("NumPy seed0 permutation(%d)"), Order.Num()), Order == Expected);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMANativeBakeTest, "HUMAClothBind.NativeBake.Kernel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHUMANativeBakeTest::RunTest(const FString&)
{
    UHUMAClothBindAsset* Asset = NewObject<UHUMAClothBindAsset>();
    Asset->FormatVersion = 1;
    Asset->RestBody = {FVector(0,0,0), FVector(10,0,0), FVector(10,10,0), FVector(0,10,0)};
    Asset->CanonicalToRender = {0,1,2,3}; Asset->BodyQuads = {0,1,2,3};
    Asset->ClothRestPositions = {FVector(1,1,1), FVector(2,1,1), FVector(1,2,1), FVector(1,1,1), FVector(2,1,1), FVector(1,2,1)};
    Asset->ClothTriangles = {0,1,2,3,4,5}; Asset->ClothUVs.Init(FVector2D::ZeroVector, 6);
    const TArray<int32> Triangles = {0,1,2,0,2,3};
    FString Error;
    TestTrue(TEXT("Native rest and rigid checks"), BakeHUMANativeLOD(*Asset, Triangles, {}, 2.f, 1.f, Error));
    TestEqual(TEXT("New default method"), Asset->BindMethod, FString(TEXT("body_field")));
    Asset->BindMethod = TEXT("body_field_v1");
    Asset->PostLoad();
    TestEqual(TEXT("0.3.0 field asset load alias"), Asset->BindMethod, FString(TEXT("body_field")));
    TestTrue(TEXT("Valid normalized weights"), Asset->Validate(Error));
    for (int32 Vertex = 0; Vertex < 3; ++Vertex)
        for (int32 Slot = 0; Slot < Asset->Slots; ++Slot)
        {
            TestEqual(TEXT("Independent of garment component"), Asset->InfluenceAnchors[Vertex * 24 + Slot], Asset->InfluenceAnchors[(Vertex + 3) * 24 + Slot]);
            TestEqual(TEXT("Identical spatial weights"), Asset->InfluenceWeights[Vertex * 24 + Slot], Asset->InfluenceWeights[(Vertex + 3) * 24 + Slot]);
        }
    const TArray<int32> FirstAnchors = Asset->InfluenceAnchors;
    const TArray<float> FirstWeights = Asset->InfluenceWeights;
    TestTrue(TEXT("Repeat bake"), BakeHUMANativeLOD(*Asset, Triangles, {}, 2.f, 1.f, Error));
    TestTrue(TEXT("Deterministic native anchors"), FirstAnchors == Asset->InfluenceAnchors && FirstWeights == Asset->InfluenceWeights);
    TestFalse(TEXT("Invalid sigma rejected"), BakeHUMANativeLOD(*Asset, Triangles, {}, 0.f, 1.f, Error));
    const TArray<int32> BadIDs = {0,0,2,3,4,5};
    TestFalse(TEXT("Inconsistent import IDs rejected"), BakeHUMANativeLOD(*Asset, Triangles, BadIDs, 2.f, 1.f, Error));
    Asset->BindMethod = TEXT("garment_graph_v1");
    Asset->PostLoad();
    TestEqual(TEXT("0.3.0 legacy asset load alias"), Asset->BindMethod, FString(TEXT("cloth_graph")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHUMAClothGraphBakeTest, "HUMAClothBind.NativeBake.ClothGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FHUMAClothGraphBakeTest::RunTest(const FString&)
{
    auto* Asset = NewObject<UHUMAClothBindAsset>();
    Asset->FormatVersion = 1;
    Asset->BakeMethod = EHUMAClothBindMethod::ClothGraph;
    Asset->RestBody = {FVector(0,0,0), FVector(10,0,0), FVector(10,10,0), FVector(0,10,0)};
    Asset->CanonicalToRender = {0,1,2,3}; Asset->BodyQuads = {0,1,2,3};
    Asset->ClothRestPositions = {FVector(1,1,1), FVector(2,1,1), FVector(1,2,1),
        FVector(1,1,1), FVector(2,1,1), FVector(1,2,1)};
    Asset->ClothTriangles = {0,1,2,3,4,5}; Asset->ClothUVs.Init(FVector2D::ZeroVector, 6);
    const TArray<int32> Triangles = {0,1,2,0,2,3}, IDs = {0,1,2,3,4,5};
    FString Error;
    TestTrue(TEXT("Cloth graph rest/rigid checks"), BakeHUMANativeLOD(*Asset, Triangles, IDs, 2.f, 1.f, Error));
    TestEqual(TEXT("Selected graph method baked"), Asset->BindMethod, FString(TEXT("cloth_graph")));
    TestEqual(TEXT("Import IDs keep overlapping disconnected components separate"), Asset->AnchorTriangles.Num(), 6);
    for (int32 Vertex = 0; Vertex < 6; ++Vertex)
        for (int32 Slot = 0; Slot < Asset->Slots; ++Slot)
        {
            const int32 Index = Vertex * Asset->Slots + Slot;
            if (Asset->InfluenceWeights[Index] > 0)
                TestEqual(TEXT("Graph support never crosses disconnected components"), Asset->InfluenceAnchors[Index] / 3, Vertex / 3);
        }
    const auto Weights = Asset->InfluenceWeights;
    const auto Anchors = Asset->InfluenceAnchors;
    TestTrue(TEXT("Repeat graph bake"), BakeHUMANativeLOD(*Asset, Triangles, IDs, 2.f, 1.f, Error));
    TestTrue(TEXT("Graph bake deterministic"), Weights == Asset->InfluenceWeights && Anchors == Asset->InfluenceAnchors);
    auto* Child = DuplicateObject<UHUMAClothBindAsset>(Asset, GetTransientPackage());
    Child->RestBody[2].Z += 0.3;
    TestTrue(TEXT("Graph child LOD shares weights"), BakeHUMANativeLOD(*Child, Triangles, IDs, 2.f, 1.f, Error, Asset));
    TestTrue(TEXT("Graph child weights unchanged"), Child->InfluenceWeights == Weights);
    TestTrue(TEXT("Coincident render splits weld"), BakeHUMANativeLOD(*Asset, Triangles, {}, 2.f, 1.f, Error));
    TestEqual(TEXT("Welded graph anchor count"), Asset->AnchorTriangles.Num(), 3);
    Asset->bBakeMethodInitialized = false;
    Asset->PostLoad();
    TestTrue(TEXT("Legacy graph asset retains method"), Asset->BakeMethod == EHUMAClothBindMethod::ClothGraph);
    Asset->BakeMethod = EHUMAClothBindMethod::BodyField;
    Asset->PostLoad();
    TestTrue(TEXT("Explicit pending selection survives loading"), Asset->BakeMethod == EHUMAClothBindMethod::BodyField);
    TestFalse(TEXT("Pending selection invalidates bake"), Asset->IsBakeCurrent(Error));
    return true;
}
#endif
#endif
