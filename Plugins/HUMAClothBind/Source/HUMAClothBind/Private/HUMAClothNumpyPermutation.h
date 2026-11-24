#pragma once

#include <cstdint>

namespace HUMACloth
{
// PCG64 XSL-RR, initialized to NumPy SeedSequence(0)'s state, not an asset-specific order.
// NumPy Generator.permutation uses low/high uint32 buffering and mask rejection
// (random_interval), rather than Generator.integers' bounded-integer algorithm.
class FNumpySeedZero
{
    std::uint64_t Low = 0x09585eb7a69561e3ULL, High = 0x1aa1b5345996452dULL;
    std::uint32_t Buffered = 0;
    bool HasBuffered = false;

    static std::uint64_t MultiplyHigh(std::uint64_t A, std::uint64_t B)
    {
        const std::uint64_t A0 = std::uint32_t(A), A1 = A >> 32;
        const std::uint64_t B0 = std::uint32_t(B), B1 = B >> 32;
        const std::uint64_t First = A0 * B0;
        const std::uint64_t Second = A1 * B0 + (First >> 32);
        const std::uint64_t Third = A0 * B1 + std::uint32_t(Second);
        return A1 * B1 + (Second >> 32) + (Third >> 32);
    }

public:
    std::uint32_t Next32()
    {
        if (HasBuffered)
        {
            HasBuffered = false;
            return Buffered;
        }
        constexpr std::uint64_t MultLow = 4865540595714422341ULL, MultHigh = 2549297995355413924ULL;
        const std::uint64_t ProductLow = Low * MultLow;
        High = High * MultLow + Low * MultHigh + MultiplyHigh(Low, MultLow) + 0x418ddadb3af71a82ULL;
        Low = ProductLow + 0x588133bc447873a9ULL;
        High += Low < ProductLow ? 1 : 0;
        const std::uint64_t Value = High ^ Low;
        const unsigned Rotation = unsigned(High >> 58);
        const std::uint64_t Rotated = Rotation ? (Value >> Rotation) | (Value << (64 - Rotation)) : Value;
        Buffered = std::uint32_t(Rotated >> 32);
        HasBuffered = true;
        return std::uint32_t(Rotated);
    }

    std::uint32_t Interval(std::uint32_t Maximum)
    {
        std::uint32_t Mask = Maximum;
        Mask |= Mask >> 1; Mask |= Mask >> 2; Mask |= Mask >> 4;
        Mask |= Mask >> 8; Mask |= Mask >> 16;
        std::uint32_t Value;
        do { Value = Next32() & Mask; } while (Value > Maximum);
        return Value;
    }
};

template<typename Swap>
void NumpyPermutation(std::int32_t Count, Swap&& SwapElements)
{
    FNumpySeedZero Random;
    for (std::int32_t Index = Count - 1; Index > 0; --Index)
        SwapElements(Index, std::int32_t(Random.Interval(std::uint32_t(Index))));
}
}
