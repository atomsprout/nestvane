#include "AArch64.hpp"
#include "GfxstreamPipe.hpp"
#include "ProbeLinux.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <unistd.h>


using namespace nestvane;

int main() {
    constexpr uint64_t base = 0x2000;
    Machine machine(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
    {
        Machine pairs(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        constexpr uint64_t address = base + 0x80;
        pairs.cpu.x[8] = address;
        assert(pairs.store64(address, 0x1122334455667788) && pairs.store64(address + 8, 0x99aabbccddeeff00));
        assert(executeStartupInstruction(pairs, 0xc87fd115)); // ldaxp x21, x20, [x8]
        assert(pairs.cpu.x[21] == 0x1122334455667788 && pairs.cpu.x[20] == 0x99aabbccddeeff00);
        pairs.cpu.x[21] = 1;
        pairs.cpu.x[20] = 2;
        assert(executeStartupInstruction(pairs, 0xc8245115)); // stxp w4, x21, x20, [x8]
        uint64_t first = 0, second = 0;
        assert(pairs.cpu.x[4] == 0 && pairs.load64(address, first) && pairs.load64(address + 8, second));
        assert(first == 1 && second == 2);
        assert(executeStartupInstruction(pairs, 0xc87fd115));
        assert(pairs.store64(address + 8, 3));
        assert(executeStartupInstruction(pairs, 0xc8245115));
        assert(pairs.cpu.x[4] == 1 && pairs.load64(address + 8, second) && second == 3);
        assert(executeStartupInstruction(pairs, 0x887f5115)); // ldxp w21, w20, [x8]
        assert(pairs.cpu.x[21] == 1 && pairs.cpu.x[20] == 0);
        pairs.cpu.x[20] = 0xbadcafe512345678ULL;
        assert(executeStartupInstruction(pairs, 0x8824d115)); // stlxp w4, w21, w20, [x8]
        assert(pairs.cpu.x[4] == 0 && pairs.load64(address, first) && first == 0x1234567800000001ULL);
        pairs.cpu.x[8] = address + 8;
        assert(executeStartupInstruction(pairs, 0xc87fd115));
        assert(pairs.cpu.stop == Stop::fault && pairs.cpu.faultAddress == address + 8);
    }
    {
        Machine permute(Image{base, {}, {}});
        const std::array<uint32_t, 4> left{10, 20, 30, 40}, right{1, 2, 3, 4}, expected{20, 2, 40, 4};
        std::memcpy(permute.cpu.v[2].data(), left.data(), sizeof(left));
        std::memcpy(permute.cpu.v[1].data(), right.data(), sizeof(right));
        assert(executeStartupInstruction(permute, 0x4e816841)); // trn2 v1.4s, v2.4s, v1.4s
        assert(std::memcmp(permute.cpu.v[1].data(), expected.data(), sizeof(expected)) == 0);
        const std::array<uint16_t, 4> lowerLeft{4, 5, 6, 7}, lowerRight{8, 9, 10, 11};
        const std::array<uint16_t, 8> lowerExpected{4, 8, 6, 10, 0, 0, 0, 0};
        std::memcpy(permute.cpu.v[4].data(), lowerLeft.data(), sizeof(lowerLeft));
        std::memcpy(permute.cpu.v[5].data(), lowerRight.data(), sizeof(lowerRight));
        permute.cpu.v[3].fill(0xff);
        assert(executeStartupInstruction(permute, 0x0e452883)); // trn1 v3.4h, v4.4h, v5.4h
        assert(std::memcmp(permute.cpu.v[3].data(), lowerExpected.data(), sizeof(lowerExpected)) == 0);
        const std::array<uint32_t, 4> compare{0xbf800000, 0x80000000, 0x3f800000, 0x7fc00000};
        const std::array<uint32_t, 4> compareExpected{0, UINT32_MAX, UINT32_MAX, 0};
        std::memcpy(permute.cpu.v[3].data(), compare.data(), sizeof(compare));
        assert(executeStartupInstruction(permute, 0x6ea0c863)); // fcmge v3.4s, v3.4s, #0.0
        assert(std::memcmp(permute.cpu.v[3].data(), compareExpected.data(), sizeof(compareExpected)) == 0);
        const std::array<uint32_t, 4> inverted{UINT32_MAX, 0, 0, UINT32_MAX};
        assert(executeStartupInstruction(permute, 0x6e205863)); // mvn v3.16b, v3.16b
        assert(std::memcmp(permute.cpu.v[3].data(), inverted.data(), sizeof(inverted)) == 0);
        const float one = 1.0f, four = 4.0f;
        std::memcpy(permute.cpu.v[1].data(), &one, sizeof(one));
        std::memcpy(permute.cpu.v[3].data(), &four, sizeof(four));
        permute.cpu.v[2].fill(0xff);
        assert(executeStartupInstruction(permute, 0x7ea3d422)); // fabd s2, s1, s3
        const std::array<float, 4> absoluteDifference{3.0f, 0, 0, 0};
        assert(std::memcmp(permute.cpu.v[2].data(), absoluteDifference.data(), sizeof(absoluteDifference)) == 0);
        const float positiveZero = 0.0f;
        std::memcpy(permute.cpu.v[4].data(), &positiveZero, sizeof(positiveZero));
        permute.cpu.v[6].fill(0xff);
        assert(executeStartupInstruction(permute, 0x1e248866)); // fnmul s6, s3, s4
        const std::array<float, 4> negativeZero{-0.0f, 0, 0, 0};
        assert(std::memcmp(permute.cpu.v[6].data(), negativeZero.data(), sizeof(negativeZero)) == 0);
        const double factor = -3.25;
        std::memcpy(permute.cpu.v[7].data(), &factor, sizeof(factor));
        permute.cpu.v[17].fill(0xff);
        assert(executeStartupInstruction(permute, 0x1e6788f1)); // fnmul d17, d7, d7
        const std::array<double, 2> negativeProduct{-10.5625, 0};
        assert(std::memcmp(permute.cpu.v[17].data(), negativeProduct.data(), sizeof(negativeProduct)) == 0);
        const std::array<uint64_t, 2> compareLeft{0x8000000000000000, 3};
        const std::array<uint64_t, 2> compareRight{0x8000000000000000, 5};
        const std::array<uint64_t, 2> compareEqual{UINT64_MAX, 0}, compareUnequal{};
        std::memcpy(permute.cpu.v[1].data(), compareLeft.data(), sizeof(compareLeft));
        std::memcpy(permute.cpu.v[0].data(), compareRight.data(), sizeof(compareRight));
        assert(executeStartupInstruction(permute, 0x7ee08c20)); // cmeq d0, d1, d0
        assert(std::memcmp(permute.cpu.v[0].data(), compareEqual.data(), sizeof(compareEqual)) == 0);
        assert(executeStartupInstruction(permute, 0x7ee08c20));
        assert(std::memcmp(permute.cpu.v[0].data(), compareUnequal.data(), sizeof(compareUnequal)) == 0);
        const std::array<float, 4> floorSingles{-2.5f, -0.0f, 17, 42}, floorSinglesResult{-3, -0.0f, 0, 0};
        std::memcpy(permute.cpu.v[1].data(), floorSingles.data(), sizeof(floorSingles));
        assert(executeStartupInstruction(permute, 0x0e219821)); // frintm v1.2s, v1.2s
        assert(std::memcmp(permute.cpu.v[1].data(), floorSinglesResult.data(), sizeof(floorSinglesResult)) == 0);
        const std::array<double, 2> floorDoubles{3.9, -0.2}, floorDoublesResult{3, -1};
        std::memcpy(permute.cpu.v[5].data(), floorDoubles.data(), sizeof(floorDoubles));
        assert(executeStartupInstruction(permute, 0x4e6198a4)); // frintm v4.2d, v5.2d
        assert(std::memcmp(permute.cpu.v[4].data(), floorDoublesResult.data(), sizeof(floorDoublesResult)) == 0);
        const std::array<float, 4> roundSingles{-0.5f, 1.5f, 17, 42}, roundSinglesResult{-1, 2, 0, 0};
        std::memcpy(permute.cpu.v[0].data(), roundSingles.data(), sizeof(roundSingles));
        assert(executeStartupInstruction(permute, 0x2e218800)); // frinta v0.2s, v0.2s
        assert(std::memcmp(permute.cpu.v[0].data(), roundSinglesResult.data(), sizeof(roundSinglesResult)) == 0);
        const std::array<uint64_t, 2> negateDoubles{0, 0xfff0000000000000ULL};
        const std::array<uint64_t, 2> negateDoublesResult{0x8000000000000000ULL, 0x7ff0000000000000ULL};
        std::memcpy(permute.cpu.v[0].data(), negateDoubles.data(), sizeof(negateDoubles));
        assert(executeStartupInstruction(permute, 0x6ee0f800)); // fneg v0.2d, v0.2d
        assert(std::memcmp(permute.cpu.v[0].data(), negateDoublesResult.data(), sizeof(negateDoublesResult)) == 0);
        const std::array<float, 4> negateSingles{-0.0f, 2.5f, 17, 42}, negateSinglesResult{0.0f, -2.5f, 0, 0};
        std::memcpy(permute.cpu.v[4].data(), negateSingles.data(), sizeof(negateSingles));
        assert(executeStartupInstruction(permute, 0x2ea0f883)); // fneg v3.2s, v4.2s
        assert(std::memcmp(permute.cpu.v[3].data(), negateSinglesResult.data(), sizeof(negateSinglesResult)) == 0);
        const std::array<float, 4> widenInput{1.75f, -0.0f, -11.5f, 23.25f};
        const std::array<double, 2> widenedLow{1.75, -0.0}, widenedHigh{-11.5, 23.25};
        std::memcpy(permute.cpu.v[2].data(), widenInput.data(), sizeof(widenInput));
        std::memcpy(permute.cpu.v[4].data(), widenInput.data(), sizeof(widenInput));
        assert(executeStartupInstruction(permute, 0x0e617842)); // fcvtl v2.2d, v2.2s
        assert(std::memcmp(permute.cpu.v[2].data(), widenedLow.data(), sizeof(widenedLow)) == 0);
        assert(executeStartupInstruction(permute, 0x4e617883)); // fcvtl2 v3.2d, v4.4s
        assert(std::memcmp(permute.cpu.v[3].data(), widenedHigh.data(), sizeof(widenedHigh)) == 0);
        const std::array<float, 4> narrowedLow{1.75f, -0.0f, 0, 0}, narrowedHigh{42, 12, 1.75f, -0.0f};
        const std::array<float, 4> narrowDestination{42, 12, 79, 22};
        assert(executeStartupInstruction(permute, 0x0e616842)); // fcvtn v2.2s, v2.2d
        assert(std::memcmp(permute.cpu.v[2].data(), narrowedLow.data(), sizeof(narrowedLow)) == 0);
        std::memcpy(permute.cpu.v[1].data(), narrowDestination.data(), sizeof(narrowDestination));
        std::memcpy(permute.cpu.v[2].data(), widenedLow.data(), sizeof(widenedLow));
        assert(executeStartupInstruction(permute, 0x4e616841)); // fcvtn2 v1.4s, v2.2d
        assert(std::memcmp(permute.cpu.v[1].data(), narrowedHigh.data(), sizeof(narrowedHigh)) == 0);
        const std::array<uint32_t, 4> minLeft{0, 0x7fc00042, 14, 19}, minRight{0x80000000, 0x3f800000, 0, 0};
        const std::array<uint32_t, 4> minOutput{0x80000000, 0x3f800000, 0, 0};
        const std::array<uint32_t, 4> maxOutput{0, 0x3f800000, 0, 0};
        std::memcpy(permute.cpu.v[2].data(), minLeft.data(), sizeof(minLeft));
        std::memcpy(permute.cpu.v[3].data(), minRight.data(), sizeof(minRight));
        assert(executeStartupInstruction(permute, 0x0ea3c442)); // fminnm v2.2s, v2.2s, v3.2s
        assert(std::memcmp(permute.cpu.v[2].data(), minOutput.data(), sizeof(minOutput)) == 0);
        std::memcpy(permute.cpu.v[0].data(), minLeft.data(), sizeof(minLeft));
        assert(executeStartupInstruction(permute, 0x0e20c440)); // fmaxnm v0.2s, v2.2s, v0.2s
        assert(std::memcmp(permute.cpu.v[0].data(), maxOutput.data(), sizeof(maxOutput)) == 0);
        const std::array<int64_t, 2> positiveAndNegative{1, INT64_MIN}, zeroAndPositive{0, INT64_MAX};
        const std::array<uint64_t, 2> firstGreater{UINT64_MAX, 0}, secondGreater{0, UINT64_MAX};
        std::memcpy(permute.cpu.v[2].data(), positiveAndNegative.data(), sizeof(positiveAndNegative));
        assert(executeStartupInstruction(permute, 0x4ee08843)); // cmgt v3.2d, v2.2d, #0
        assert(std::memcmp(permute.cpu.v[3].data(), firstGreater.data(), sizeof(firstGreater)) == 0);
        std::memcpy(permute.cpu.v[2].data(), zeroAndPositive.data(), sizeof(zeroAndPositive));
        assert(executeStartupInstruction(permute, 0x4ee08843));
        assert(std::memcmp(permute.cpu.v[3].data(), secondGreater.data(), sizeof(secondGreater)) == 0);
        std::memcpy(permute.cpu.v[1].data(), positiveAndNegative.data(), sizeof(positiveAndNegative));
        assert(executeStartupInstruction(permute, 0x6ee09823)); // cmle v3.2d, v1.2d, #0
        assert(std::memcmp(permute.cpu.v[3].data(), secondGreater.data(), sizeof(secondGreater)) == 0);
        std::memcpy(permute.cpu.v[1].data(), zeroAndPositive.data(), sizeof(zeroAndPositive));
        assert(executeStartupInstruction(permute, 0x6ee09823));
        assert(std::memcmp(permute.cpu.v[3].data(), firstGreater.data(), sizeof(firstGreater)) == 0);
        const std::array<uint16_t, 8> reductionInput{0xffff, 0x7fff, 3, 0xfffe, 0, 1, 2, 0};
        const std::array<uint16_t, 8> reductionOutput{3};
        std::memcpy(permute.cpu.v[2].data(), reductionInput.data(), sizeof(reductionInput));
        assert(executeStartupInstruction(permute, 0x2e71a842)); // uminv h2, v2.4h
        assert(std::memcmp(permute.cpu.v[2].data(), reductionOutput.data(), sizeof(reductionOutput)) == 0);
        const int32_t negativeLane = -123;
        std::memcpy(permute.cpu.v[0].data(), &negativeLane, sizeof(negativeLane));
        assert(executeStartupInstruction(permute, 0x4e042c0b)); // smov x11, v0.s[0]
        assert(permute.cpu.x[11] == uint64_t(-123));
        permute.cpu.v[2][15] = 0x80;
        assert(executeStartupInstruction(permute, 0x0e1f2c49)); // smov w9, v2.b[15]
        assert(permute.cpu.x[9] == 0xffffff80);
        const std::array<float, 4> ceilSingles{-0.5f, 2.1f, 17, 42}, ceilSinglesResult{-0.0f, 3, 0, 0};
        std::memcpy(permute.cpu.v[1].data(), ceilSingles.data(), sizeof(ceilSingles));
        assert(executeStartupInstruction(permute, 0x0ea18821)); // frintp v1.2s, v1.2s
        assert(std::memcmp(permute.cpu.v[1].data(), ceilSinglesResult.data(), sizeof(ceilSinglesResult)) == 0);
        const std::array<uint16_t, 8> roundedInput{0, 127, 128, 255, 256, 383, 384, 65535};
        const std::array<uint16_t, 8> roundedShift{0, 0, 1, 1, 1, 1, 2, 256};
        std::memcpy(permute.cpu.v[4].data(), roundedInput.data(), sizeof(roundedInput));
        assert(executeStartupInstruction(permute, 0x6f182485)); // urshr v5.8h, v4.8h, #8
        assert(std::memcmp(permute.cpu.v[5].data(), roundedShift.data(), sizeof(roundedShift)) == 0);
        assert(executeStartupInstruction(permute, 0x2e254082)); // raddhn v2.8b, v4.8h, v5.8h
        const std::array<uint8_t, 16> narrowedSum{0, 0, 1, 1, 1, 2, 2, 1};
        assert(permute.cpu.v[2] == narrowedSum);
        assert(executeStartupInstruction(permute, 0x6e254082)); // raddhn2 v2.16b, v4.8h, v5.8h
        const std::array<uint8_t, 16> duplicatedSum{0, 0, 1, 1, 1, 2, 2, 1, 0, 0, 1, 1, 1, 2, 2, 1};
        assert(permute.cpu.v[2] == duplicatedSum);
        const std::array<float, 4> squareInput{2.25f, -0.0f, 6.0f, 8.0f};
        const std::array<float, 4> squareRoot{1.5f, -0.0f, 0.0f, 0.0f};
        std::memcpy(permute.cpu.v[0].data(), squareInput.data(), sizeof(squareInput));
        assert(executeStartupInstruction(permute, 0x2ea1f800)); // fsqrt v0.2s, v0.2s
        assert(std::memcmp(permute.cpu.v[0].data(), squareRoot.data(), sizeof(squareRoot)) == 0);
        const float toCeil = -4.75f, outOfRange = 0x1p31f;
        std::memcpy(permute.cpu.v[1].data(), &toCeil, sizeof(toCeil));
        assert(executeStartupInstruction(permute, 0x1e280028)); // fcvtps w8, s1
        assert(permute.cpu.x[8] == uint32_t(-4));
        assert(executeStartupInstruction(permute, 0x1e300034)); // fcvtms w20, s1
        assert(permute.cpu.x[20] == uint32_t(-5));
        std::memcpy(permute.cpu.v[1].data(), &outOfRange, sizeof(outOfRange));
        assert(executeStartupInstruction(permute, 0x1e280028));
        assert(permute.cpu.x[8] == INT32_MAX);
        const double half = -2.5, beyondSignedMax = 0x1p63;
        std::memcpy(permute.cpu.v[5].data(), &half, sizeof(half));
        assert(executeStartupInstruction(permute, 0x9e6400a9)); // fcvtas x9, d5
        assert(permute.cpu.x[9] == uint64_t(-3));
        assert(executeStartupInstruction(permute, 0x9e7000aa)); // fcvtms x10, d5
        assert(permute.cpu.x[10] == uint64_t(-3));
        std::memcpy(permute.cpu.v[5].data(), &beyondSignedMax, sizeof(beyondSignedMax));
        assert(executeStartupInstruction(permute, 0x9e6400a9));
        assert(permute.cpu.x[9] == INT64_MAX);
        permute.cpu.v[1] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        const std::array<uint16_t, 8> wideLower{0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700, 0x800};
        const std::array<uint16_t, 8> wideUpper{0x900, 0xa00, 0xb00, 0xc00, 0xd00, 0xe00, 0xf00, 0x1000};
        assert(executeStartupInstruction(permute, 0x2e213820)); // shll v0.8h, v1.8b, #8
        assert(std::memcmp(permute.cpu.v[0].data(), wideLower.data(), sizeof(wideLower)) == 0);
        assert(executeStartupInstruction(permute, 0x6e213820)); // shll2 v0.8h, v1.16b, #8
        assert(std::memcmp(permute.cpu.v[0].data(), wideUpper.data(), sizeof(wideUpper)) == 0);
        const std::array<int16_t, 8> shiftInput{-1, 3, -32768, 32767, -5, 4, -300, 1024};
        const std::array<int32_t, 4> shiftLower{-1, 3, -32768, 32767}, shiftUpper{-40, 32, -2400, 8192};
        std::memcpy(permute.cpu.v[3].data(), shiftInput.data(), sizeof(shiftInput));
        std::memcpy(permute.cpu.v[8].data(), shiftInput.data(), sizeof(shiftInput));
        assert(executeStartupInstruction(permute, 0x0f10a462)); // sshll v2.4s, v3.4h, #0
        assert(std::memcmp(permute.cpu.v[2].data(), shiftLower.data(), sizeof(shiftLower)) == 0);
        assert(executeStartupInstruction(permute, 0x4f13a507)); // sshll2 v7.4s, v8.8h, #3
        assert(std::memcmp(permute.cpu.v[7].data(), shiftUpper.data(), sizeof(shiftUpper)) == 0);
        const auto minNumber = [&](uint32_t a, uint32_t b, uint32_t expectedBits) {
            std::memcpy(permute.cpu.v[6].data(), &a, sizeof(a));
            std::memcpy(permute.cpu.v[0].data(), &b, sizeof(b));
            assert(executeStartupInstruction(permute, 0x1e2078c0)); // fminnm s0, s6, s0
            assert(std::memcmp(permute.cpu.v[0].data(), &expectedBits, sizeof(expectedBits)) == 0);
        };
        minNumber(0, 0x80000000, 0x80000000);
        minNumber(0x7fc00042, 0x3f800000, 0x3f800000);
        minNumber(0x7f800042, 0x3f800000, 0x7fc00042);
        const std::array<uint32_t, 4> vectorFloat{0xbff00000, 0x4f000000, UINT32_MAX, UINT32_MAX};
        const std::array<uint32_t, 4> vectorInteger{UINT32_MAX, INT32_MAX, 0, 0};
        std::memcpy(permute.cpu.v[0].data(), vectorFloat.data(), sizeof(vectorFloat));
        assert(executeStartupInstruction(permute, 0x0ea1b800)); // fcvtzs v0.2s, v0.2s
        assert(std::memcmp(permute.cpu.v[0].data(), vectorInteger.data(), sizeof(vectorInteger)) == 0);
        const std::array<uint32_t, 4> unsignedLanes{0, 17, 0x80000000, UINT32_MAX};
        const std::array<float, 4> convertedUnsigned{0, 17, 0x1p31f, 0x1p32f};
        std::memcpy(permute.cpu.v[0].data(), unsignedLanes.data(), sizeof(unsignedLanes));
        assert(executeStartupInstruction(permute, 0x6e21d800)); // ucvtf v0.4s, v0.4s
        assert(std::memcmp(permute.cpu.v[0].data(), convertedUnsigned.data(), sizeof(convertedUnsigned)) == 0);
        const std::array<int64_t, 2> signedLanes{-9, 100};
        const std::array<double, 2> convertedSigned{-9, 100};
        std::memcpy(permute.cpu.v[4].data(), signedLanes.data(), sizeof(signedLanes));
        assert(executeStartupInstruction(permute, 0x4e61d883)); // scvtf v3.2d, v4.2d
        assert(std::memcmp(permute.cpu.v[3].data(), convertedSigned.data(), sizeof(convertedSigned)) == 0);
        const std::array<float, 4> twoSingles{1, 1, 0, 0}, fourSingles{-2, -2, -2, -2};
        const std::array<double, 2> twoDoubles{0.75, 0.75};
        assert(executeStartupInstruction(permute, 0x0f03f600)); // fmov v0.2s, #1.0
        assert(std::memcmp(permute.cpu.v[0].data(), twoSingles.data(), sizeof(twoSingles)) == 0);
        assert(executeStartupInstruction(permute, 0x4f04f401)); // fmov v1.4s, #-2.0
        assert(std::memcmp(permute.cpu.v[1].data(), fourSingles.data(), sizeof(fourSingles)) == 0);
        assert(executeStartupInstruction(permute, 0x6f03f502)); // fmov v2.2d, #0.75
        assert(std::memcmp(permute.cpu.v[2].data(), twoDoubles.data(), sizeof(twoDoubles)) == 0);
        const std::array<uint32_t, 4> mslByte{0xf2ff, 0xf2ff, 0xf2ff, 0xf2ff};
        const std::array<uint32_t, 4> mslHalfword{0x12ffff, 0x12ffff, 0x12ffff, 0x12ffff};
        const std::array<uint32_t, 4> mslInverted{0xfffffe00, 0xfffffe00, 0, 0};
        assert(executeStartupInstruction(permute, 0x4f07c640)); // movi v0.4s, #242, msl #8
        assert(std::memcmp(permute.cpu.v[0].data(), mslByte.data(), sizeof(mslByte)) == 0);
        assert(executeStartupInstruction(permute, 0x4f00d642)); // movi v2.4s, #18, msl #16
        assert(std::memcmp(permute.cpu.v[2].data(), mslHalfword.data(), sizeof(mslHalfword)) == 0);
        assert(executeStartupInstruction(permute, 0x2f00c423)); // mvni v3.2s, #1, msl #8
        assert(std::memcmp(permute.cpu.v[3].data(), mslInverted.data(), sizeof(mslInverted)) == 0);
        const std::array<int16_t, 8> firstSigned{-5, 2, -11, 32767, -5, -7, 0, -32768};
        const std::array<int16_t, 8> secondSigned{3, -2, -9, -32768, -6, 11, 0, -4};
        const std::array<int16_t, 8> maxSigned{3, 2, -9, 32767, -5, 11, 0, -4};
        const std::array<int16_t, 8> minSigned{-5, -2, -11, -32768};
        std::memcpy(permute.cpu.v[7].data(), firstSigned.data(), sizeof(firstSigned));
        std::memcpy(permute.cpu.v[1].data(), secondSigned.data(), sizeof(secondSigned));
        assert(executeStartupInstruction(permute, 0x4e6164e7)); // smax v7.8h, v7.8h, v1.8h
        assert(std::memcmp(permute.cpu.v[7].data(), maxSigned.data(), sizeof(maxSigned)) == 0);
        std::memcpy(permute.cpu.v[2].data(), firstSigned.data(), sizeof(firstSigned));
        std::memcpy(permute.cpu.v[3].data(), secondSigned.data(), sizeof(secondSigned));
        assert(executeStartupInstruction(permute, 0x0e636c44)); // smin v4.4h, v2.4h, v3.4h
        assert(std::memcmp(permute.cpu.v[4].data(), minSigned.data(), sizeof(minSigned)) == 0);
        const std::array<uint16_t, 8> unsignedLeft{0, 256, 128, 65535}, unsignedRight{1, 2, 512, 65535};
        const std::array<int32_t, 4> unsignedDifference{-1, 254, -384, 0}, highDifference{1, -18, 0, -32764};
        std::memcpy(permute.cpu.v[7].data(), unsignedLeft.data(), sizeof(unsignedLeft));
        std::memcpy(permute.cpu.v[6].data(), unsignedRight.data(), sizeof(unsignedRight));
        assert(executeStartupInstruction(permute, 0x2e6620f0)); // usubl v16.4s, v7.4h, v6.4h
        assert(std::memcmp(permute.cpu.v[16].data(), unsignedDifference.data(), sizeof(unsignedDifference)) == 0);
        assert(executeStartupInstruction(permute, 0x4e632041)); // ssubl2 v1.4s, v2.8h, v3.8h
        assert(std::memcmp(permute.cpu.v[1].data(), highDifference.data(), sizeof(highDifference)) == 0);
        const std::array<uint32_t, 4> words{UINT32_MAX, 0x80000000, 3, 7};
        const std::array<uint32_t, 4> factors{3, 8, UINT32_MAX, 5};
        const std::array<uint32_t, 4> indexedProduct{UINT32_MAX - 2, 0x80000000, 9, 21};
        std::memcpy(permute.cpu.v[16].data(), words.data(), sizeof(words));
        std::memcpy(permute.cpu.v[5].data(), factors.data(), sizeof(factors));
        assert(executeStartupInstruction(permute, 0x4f858210)); // mul v16.4s, v16.4s, v5.s[0]
        assert(std::memcmp(permute.cpu.v[16].data(), indexedProduct.data(), sizeof(indexedProduct)) == 0);
        std::memcpy(permute.cpu.v[2].data(), words.data(), sizeof(words));
        std::memcpy(permute.cpu.v[31].data(), factors.data(), sizeof(factors));
        const std::array<uint32_t, 4> highWordProduct{UINT32_MAX - 4, 0x80000000, 15, 35};
        assert(executeStartupInstruction(permute, 0x4fbf8841)); // mul v1.4s, v2.4s, v31.s[3]
        assert(std::memcmp(permute.cpu.v[1].data(), highWordProduct.data(), sizeof(highWordProduct)) == 0);
        const std::array<uint32_t, 4> indexedSum{UINT32_MAX - 3, 0, 12, 28};
        assert(executeStartupInstruction(permute, 0x6f850841)); // mla v1.4s, v2.4s, v5.s[2]
        assert(std::memcmp(permute.cpu.v[1].data(), indexedSum.data(), sizeof(indexedSum)) == 0);
        const std::array<uint16_t, 8> halfwords{2, 3, 32768, 65535, 1, 2, 3, 4};
        const std::array<uint16_t, 8> halfwordFactors{1, 2, 4, 5, 6, 8, 9, 3};
        const std::array<uint16_t, 8> halfwordProduct{6, 9, 32768, 65533};
        const std::array<uint16_t, 8> indexedDifference{65532, 65530, 0, 2, 65531, 65526, 65521, 65516};
        std::memcpy(permute.cpu.v[2].data(), halfwords.data(), sizeof(halfwords));
        std::memcpy(permute.cpu.v[3].data(), halfwordFactors.data(), sizeof(halfwordFactors));
        std::memcpy(permute.cpu.v[5].data(), halfwordFactors.data(), sizeof(halfwordFactors));
        assert(executeStartupInstruction(permute, 0x0f738841)); // mul v1.4h, v2.4h, v3.h[7]
        assert(std::memcmp(permute.cpu.v[1].data(), halfwordProduct.data(), sizeof(halfwordProduct)) == 0);
        assert(executeStartupInstruction(permute, 0x6f754041)); // mls v1.8h, v2.8h, v5.h[3]
        assert(std::memcmp(permute.cpu.v[1].data(), indexedDifference.data(), sizeof(indexedDifference)) == 0);
        const std::array<int16_t, 8> signedProductsLeft{-2, 32767, -32768, 5, 2, -7, 5, -1};
        const std::array<int16_t, 8> signedProductsRight{3, -2, -32768, -5, -2, -3, 4, -32768};
        const std::array<int32_t, 4> signedAccumulators{-1, 2, -1, 0};
        const std::array<int32_t, 4> signedProductSum{-7, -65532, 1073741823, -25};
        const std::array<int32_t, 4> signedHighProduct{-4, 21, 20, 32768};
        std::memcpy(permute.cpu.v[17].data(), signedProductsLeft.data(), sizeof(signedProductsLeft));
        std::memcpy(permute.cpu.v[2].data(), signedProductsRight.data(), sizeof(signedProductsRight));
        std::memcpy(permute.cpu.v[5].data(), signedAccumulators.data(), sizeof(signedAccumulators));
        assert(executeStartupInstruction(permute, 0x0e628225)); // smlal v5.4s, v17.4h, v2.4h
        assert(std::memcmp(permute.cpu.v[5].data(), signedProductSum.data(), sizeof(signedProductSum)) == 0);
        assert(executeStartupInstruction(permute, 0x4e62c225)); // smull2 v5.4s, v17.8h, v2.8h
        assert(std::memcmp(permute.cpu.v[5].data(), signedHighProduct.data(), sizeof(signedHighProduct)) == 0);
        const std::array<uint32_t, 4> highProductsLeft{10, 20, UINT32_MAX, 0x80000000};
        const std::array<uint32_t, 4> highProductsRight{30, 40, UINT32_MAX, 0x80000001};
        const std::array<uint64_t, 2> unsignedAccumulators{10, 0x100000000};
        const std::array<uint64_t, 2> unsignedProductSum{0xfffffffe0000000b, 0x4000000180000000};
        std::memcpy(permute.cpu.v[3].data(), highProductsLeft.data(), sizeof(highProductsLeft));
        std::memcpy(permute.cpu.v[4].data(), highProductsRight.data(), sizeof(highProductsRight));
        std::memcpy(permute.cpu.v[8].data(), unsignedAccumulators.data(), sizeof(unsignedAccumulators));
        assert(executeStartupInstruction(permute, 0x6ea48068)); // umlal2 v8.2d, v3.4s, v4.4s
        assert(std::memcmp(permute.cpu.v[8].data(), unsignedProductSum.data(), sizeof(unsignedProductSum)) == 0);
        assert(executeStartupInstruction(permute, 0x6ea4a068)); // umlsl2 v8.2d, v3.4s, v4.4s
        assert(std::memcmp(permute.cpu.v[8].data(), unsignedAccumulators.data(), sizeof(unsignedAccumulators)) == 0);
        const std::array<float, 4> leftAdd{1, -2, 3, 4}, rightAdd{3, 1, 6, -4}, expectedSum{4, -1, 9, 0};
        std::memcpy(permute.cpu.v[0].data(), leftAdd.data(), sizeof(leftAdd));
        std::memcpy(permute.cpu.v[2].data(), rightAdd.data(), sizeof(rightAdd));
        assert(executeStartupInstruction(permute, 0x4e22d400)); // fadd v0.4s, v0.4s, v2.4s
        assert(std::memcmp(permute.cpu.v[0].data(), expectedSum.data(), sizeof(expectedSum)) == 0);
        const std::array<float, 4> pairFloat{-2, 5.5f, 7, 8}, sumFloat{3.5f};
        std::memcpy(permute.cpu.v[7].data(), pairFloat.data(), sizeof(pairFloat));
        assert(executeStartupInstruction(permute, 0x7e30d8e7)); // faddp s7, v7.2s
        assert(std::memcmp(permute.cpu.v[7].data(), sumFloat.data(), sizeof(sumFloat)) == 0);
        const std::array<double, 2> pairDouble{-5.25, 4}, sumDouble{-1.25};
        std::memcpy(permute.cpu.v[1].data(), pairDouble.data(), sizeof(pairDouble));
        assert(executeStartupInstruction(permute, 0x7e70d822)); // faddp d2, v1.2d
        assert(std::memcmp(permute.cpu.v[2].data(), sumDouble.data(), sizeof(sumDouble)) == 0);
        const std::array<uint32_t, 4> comparison{UINT32_MAX, 0, 0, 0};
        const std::array<uint32_t, 4> against{0x40400000, 0, 0x41100000, 0x7fc00000};
        std::memcpy(permute.cpu.v[1].data(), against.data(), sizeof(against));
        assert(executeStartupInstruction(permute, 0x6ea1e402)); // fcmgt v2.4s, v0.4s, v1.4s
        assert(std::memcmp(permute.cpu.v[2].data(), comparison.data(), sizeof(comparison)) == 0);
        permute.cpu.v[3] = {0, 1, 2, 0, 0, 8, 0, 16, 0, 0, 0, 0, 16, 0, 0, 2};
        const std::array<uint8_t, 16> nonzeroBytes{0, 255, 255, 0, 0, 255, 0, 255, 0, 0, 0, 0, 255, 0, 0, 255};
        assert(executeStartupInstruction(permute, 0x4e238c67)); // cmtst v7.16b, v3.16b, v3.16b
        assert(permute.cpu.v[7] == nonzeroBytes);
        permute.cpu.nzcv = 0xa;
        assert(executeStartupInstruction(permute, 0xd53b4213)); // mrs x19, NZCV
        assert(permute.cpu.x[19] == 0xa0000000 && permute.cpu.nzcv == 0xa);
        permute.cpu.x[4] = 0xffffffff5000000f;
        assert(executeStartupInstruction(permute, 0xd51b4204)); // msr NZCV, x4
        assert(permute.cpu.nzcv == 5);
        const auto stack = permute.cpu.sp;
        assert(executeStartupInstruction(permute, 0xd53b421f)); // mrs xzr, NZCV
        assert(permute.cpu.sp == stack && permute.cpu.nzcv == 5);
        assert(executeStartupInstruction(permute, 0xd51b421f)); // msr NZCV, xzr
        assert(permute.cpu.nzcv == 0);
    }
    {
        constexpr uint64_t boundary = base + pageSize;
        Machine adjacent(Image{base, {{base, std::vector<uint8_t>(pageSize), 6},
                                     {boundary, std::vector<uint8_t>(pageSize), 6}}, {}});
        constexpr uint64_t expected = 0x1122334455667788;
        uint64_t loaded = 0;
        assert(adjacent.store64(boundary - 1, expected));
        assert(adjacent.load64(boundary - 1, loaded) && loaded == expected);
        auto nextPage = std::find_if(adjacent.segments.begin(), adjacent.segments.end(),
                                     [&](const auto& segment) { return segment.address == boundary; });
        assert(nextPage != adjacent.segments.end());
        nextPage->flags = 4;
        assert(!adjacent.store64(boundary - 1, UINT64_MAX));
        assert(adjacent.load64(boundary - 1, loaded) && loaded == expected);
        nextPage->flags = 0;
        assert(!adjacent.load64(boundary - 1, loaded) && loaded == expected);
    }
    ProbeLinux linux(1, 0);
    linux.addDirectory("/test", 0755);
    const auto call = [&](uint64_t number, std::array<uint64_t, 6> args = {}) {
        std::copy(args.begin(), args.end(), machine.cpu.x.begin());
        machine.cpu.x[8] = number;
        assert(linux.syscall(machine));
        return machine.cpu.x[0];
    };
    const auto put = [&](uint64_t address, const char* data) {
        assert(machine.write(address, {reinterpret_cast<const uint8_t*>(data), std::strlen(data) + 1}));
    };
    const auto load = [&](uint64_t address) {
        uint64_t value;
        assert(machine.load64(address, value));
        return value;
    };
    {
        constexpr uint64_t committedLength = 1024 * 1024;
        const auto reservation = call(222, {0, committedLength + 2 * pageSize, 0, 0x4022, UINT32_MAX, 0});
        const auto committed = reservation + pageSize;
        const auto dirty = linux.dirtyAnonymousBytes();
        assert(!machine.read(committed, 1, 4));
        assert(call(226, {committed, committedLength, 3}) == 0);
        assert(!machine.read(reservation, 1, 4) && !machine.read(committed + committedLength, 1, 4));
        assert(load(committed) == 0 && linux.dirtyAnonymousBytes() == dirty);
        assert(machine.store64(committed, 42) && load(committed) == 42);
        assert(linux.dirtyAnonymousBytes() > dirty && linux.dirtyAnonymousBytes() < dirty + committedLength);
        assert(call(215, {reservation, committedLength + 2 * pageSize}) == 0);
        assert(linux.dirtyAnonymousBytes() == dirty);
    }
    {
        static std::array<uint8_t, pageSize * 3 + 32> contents{};
        contents[pageSize] = 0x37;
        contents[2 * pageSize] = 0x72;
        contents[3 * pageSize] = 0x2a;
        linux.mount("/test/resources.apk", contents);
        put(base + 512, "/test/resources.apk");
        const auto file = call(56, {uint32_t(-100), base + 512, 0});
        assert(file < 256);
        const auto view = call(222, {0, pageSize * 2, 1, 1, file, pageSize});
        assert(machine.read(view, 1, 4) == contents.data() + pageSize);
        assert(call(226, {view + pageSize, pageSize, 0}) == 0);
        assert(!machine.read(view + pageSize, 1, 4));
        assert(call(226, {view + pageSize, pageSize, 1}) == 0);
        assert(machine.read(view + pageSize, 1, 4) == contents.data() + pageSize * 2);
        assert(call(226, {view, pageSize, 3}) == uint64_t(-13));
        assert(!machine.store64(view, 0));
        const auto privateView = call(222, {0, pageSize * 2, 1, 2, file, pageSize});
        assert(machine.read(privateView, 1, 4) == contents.data() + pageSize);
        assert(call(226, {privateView, pageSize, 3}) == 0);
        assert(*machine.read(privateView, 1, 4) == 0x37 && machine.store64(privateView, 0x99));
        assert(contents[pageSize] == 0x37 && *machine.read(view, 1, 4) == 0x37);
        assert(machine.read(privateView + pageSize, 1, 4) == contents.data() + pageSize * 2);
        assert(call(215, {privateView, pageSize * 2}) == 0);
        const auto tail = call(222, {0, pageSize * 3, 1, 1, file, pageSize * 2});
        assert(machine.read(tail, 1, 4) == contents.data() + pageSize * 2);
        assert(*machine.read(tail + pageSize, 1, 4) == 0x2a && *machine.read(tail + pageSize + 32, 1, 4) == 0);
        assert(!machine.read(tail + pageSize * 2, 1, 4));
        assert(call(57, {file}) == 0 && machine.read(view, 1, 4) == contents.data() + pageSize);
        assert(call(215, {view, pageSize * 2}) == 0 && call(215, {tail, pageSize * 3}) == 0);
    }
    {
        using namespace std::chrono;
        struct EchoGraphicsPipe : GraphicsPipe {
            std::vector<uint8_t> queued;
            int64_t read(std::span<uint8_t> bytes) override {
                if (queued.empty()) return -11;
                const auto count = std::min(queued.size(), bytes.size());
                std::copy_n(queued.begin(), count, bytes.begin());
                queued.erase(queued.begin(), queued.begin() + count);
                return count;
            }
            int64_t write(std::span<const uint8_t> bytes) override {
                queued.insert(queued.end(), bytes.begin(), bytes.end());
                return bytes.size();
            }
            uint16_t pollEvents() const override { return 4 | (queued.empty() ? 0 : 1); }
        };
        constexpr uint64_t cwd = uint32_t(-100), path = base + 512, buffer = base + 1024, event = base + 768;
        put(path, "/dev/goldfish_pipe_dprctd");
        assert(call(56, {cwd, path, 0x802}) == uint64_t(-2));
        unsigned connected = 0;
        linux.setGraphicsPipeFactory([&](std::string_view service) -> std::unique_ptr<GraphicsPipe> {
            assert(service == "opengles");
            ++connected;
            return std::make_unique<EchoGraphicsPipe>();
        });
        const auto channel = call(56, {cwd, path, 0x802});
        const auto duplicate = call(23, {channel});
        const auto poll = call(20, {0});
        assert(channel < 256 && duplicate < 256 && poll < 256 && connected == 0);
        assert(call(63, {channel, buffer, 2}) == uint64_t(-11) && !linux.blocked());
        put(buffer, "pipe:");
        assert(call(64, {channel, buffer, 5}) == 5 && connected == 0);
        put(buffer, "opengles");
        assert(machine.store64(buffer + 9, 0x0201));
        assert(call(64, {duplicate, buffer, 11}) == 9 && connected == 1);
        assert(machine.store64(event, 1) && machine.store64(event + 8, 77));
        assert(call(21, {poll, 1, duplicate, event}) == 0);
        assert(call(22, {poll, event, 1, 0}) == 0);
        assert(call(22, {poll, event, 1, 100}) == poll && linux.blocked());
        assert(linux.schedule(machine, steady_clock::now() + seconds(1)));
        assert(call(64, {channel, buffer + 9, 2}) == 2);
        assert(call(22, {poll, event, 1, 100}) == 1 && load(event) == 1 && load(event + 8) == 77);
        assert(call(63, {duplicate, 0, 2}) == uint64_t(-14));
        assert(call(63, {duplicate, buffer, 1}) == 1 && *machine.read(buffer, 1, 4) == 1);
        assert(call(63, {channel, buffer, 1}) == 1 && *machine.read(buffer, 1, 4) == 2);
        assert(call(63, {duplicate, buffer, 1}) == uint64_t(-11));
        assert(call(57, {duplicate}) == 0 && call(57, {channel}) == 0 && call(57, {poll}) == 0);
        const auto other = call(56, {cwd, path, 0x802});
        put(buffer, "pipe:qemud");
        assert(call(64, {other, buffer, 11}) == uint64_t(-2) && connected == 1);
        assert(call(64, {other, buffer, 1}) == uint64_t(-5));
        assert(call(57, {other}) == 0);
        static std::vector<uint32_t> released;
        linux.setGraphicsPipeFactory([](std::string_view service) -> std::unique_ptr<GraphicsPipe> {
            assert(service == "refcount");
            return std::make_unique<GfxstreamRefcountPipe>(+[](uint32_t handle) { released.push_back(handle); });
        });
        const auto ownerPipe = call(56, {cwd, path, 0x802});
        const auto copiedPipe = call(23, {ownerPipe});
        put(buffer, "pipe:refcount");
        assert(call(64, {copiedPipe, buffer, 14}) == 14);
        assert(machine.store64(buffer, 0x67452301));
        assert(call(63, {ownerPipe, buffer, 4}) == uint64_t(-5));
        assert(call(64, {ownerPipe, buffer, 2}) == 2);
        assert(call(64, {copiedPipe, buffer + 2, 4}) == 2);
        assert(call(64, {ownerPipe, buffer, 4}) == uint64_t(-22));
        assert(call(57, {ownerPipe}) == 0 && released.empty());
        assert(call(57, {copiedPipe}) == 0 && released == std::vector<uint32_t>{0x67452301});

        const auto partialPipe = call(56, {cwd, path, 0x802});
        put(buffer, "pipe:refcount");
        assert(call(64, {partialPipe, buffer, 14}) == 14);
        assert(call(64, {partialPipe, buffer, 2}) == 2);
        assert(call(57, {partialPipe}) == 0 && released.size() == 1);

        put(path, "/dev/goldfish_address_space");
        const auto owner = call(56, {cwd, path, 2}), importer = call(56, {cwd, path, 2});
        assert(owner < 256 && importer < 256 && owner != importer);
        constexpr uint64_t wireBase = 0x800000000ULL;
        const auto ping = [&](uint64_t fd, uint64_t offset, uint64_t size, uint64_t metadata) {
            assert(machine.store64(buffer, offset) && machine.store64(buffer + 8, size) &&
                   machine.store64(buffer + 16, metadata) && machine.store64(buffer + 24, 40) &&
                   machine.store64(buffer + 32, 0));
            return call(29, {fd, 0xc028470c, buffer});
        };
        assert(ping(owner, 0, 0, 6) == 0 && load(buffer) == wireBase && load(buffer + 16) == 0);
        assert(ping(owner, 0, 4097, 1) == 0 && load(buffer + 16) == 0);
        const auto offset = load(buffer), length = load(buffer + 8);
        assert(offset % 16384 == 0 && length == 16384);
        assert(call(222, {0, length, 3, 1, importer, offset}) == uint64_t(-1));
        assert(call(222, {0, length, 3, 1, owner, offset}) == uint64_t(-1));
        const auto claim = [&](uint64_t fd, uint64_t size) {
            assert(machine.store64(buffer, offset) && machine.store64(buffer + 8, size));
            return call(29, {fd, 0xc010470d, buffer});
        };
        assert(claim(importer, length + 1) == uint64_t(-22));
        assert(claim(owner, length) == 0 && claim(importer, length) == 0 && claim(importer, length) == 0);
        const auto left = call(222, {0, length, 2, 1, owner, offset});
        const auto right = call(222, {0, length, 2, 1, importer, offset});
        assert(left < (uint64_t{1} << 45) && right < (uint64_t{1} << 45) && left != right);
        assert(machine.store64(left + pageSize, 0x09375674a1b2c3d4) && load(right + pageSize) == 0x09375674a1b2c3d4);
        assert(call(226, {right + pageSize, pageSize, 0}) == 0);
        uint64_t protectedValue = 0;
        assert(!machine.load64(right + pageSize, protectedValue));
        assert(call(226, {right + pageSize, pageSize, 2}) == 0);
        assert(load(right + pageSize) == 0x09375674a1b2c3d4);
        assert(machine.store64(buffer, offset) && call(29, {importer, 0xc008470e, buffer}) == 0);
        const auto third = call(222, {0, pageSize, 3, 1, importer, offset + pageSize});
        assert(third < (uint64_t{1} << 45) && load(third) == load(right + pageSize));
        assert(ping(owner, offset, 0, 2) == 0 && load(buffer) == wireBase + offset && load(buffer + 16) == 0);
        assert(claim(importer, length) == uint64_t(-6));
        assert(call(57, {owner}) == 0 && call(57, {importer}) == 0);
        assert(machine.store64(third, 99) && load(left + pageSize) == 99);
        assert(call(215, {left, length}) == 0 && call(215, {right, length}) == 0 && call(215, {third, pageSize}) == 0);

        const auto normal = call(56, {cwd, path, 2});
        assert(normal < 256 && ping(normal, 0, 0, 5) == 0 && load(buffer + 16) == 0);
        assert(machine.store64(buffer, pageSize));
        assert(call(29, {normal, 0xc018470a, buffer}) == 0);
        const auto directSize = load(buffer), directOffset = load(buffer + 8);
        assert(directSize >= pageSize && load(buffer + 16) == wireBase + directOffset);
        assert(ping(normal, directOffset, directSize, 1) == 0 && load(buffer + 16) == 0);
        const auto direct = call(222, {0, pageSize, 3, 1, normal, directOffset});
        assert(direct < (uint64_t{1} << 45) && load(direct) == 0);
        assert(ping(normal, directOffset, 0, 2) == 0 && load(buffer + 16) == 0);
        assert(machine.store64(buffer, directOffset) && call(29, {normal, 0xc008470b, buffer}) == 0);
        assert(call(222, {0, pageSize, 3, 1, normal, directOffset}) == uint64_t(-1));
        assert(call(215, {direct, pageSize}) == 0 && call(57, {normal}) == 0);
        linux.setGraphicsPipeFactory({});
    }
    {
        using namespace std::chrono;
        constexpr uint64_t pair = base + 512, timeout = base + 544, length = base + 576;
        assert(call(199, {1, 1, 0, pair}) == 0);
        const auto sender = uint32_t(load(pair)), receiver = uint32_t(load(pair) >> 32);
        assert(machine.store64(timeout, 0) && machine.store64(timeout + 8, 1'000'000));
        assert(call(208, {receiver, 1, 20, timeout, 16}) == uint64_t(-33));
        assert(machine.store64(timeout + 8, 20'000));
        assert(call(208, {receiver, 1, 20, timeout, 8}) == uint64_t(-22));
        assert(call(208, {receiver, 1, 20, timeout, 16}) == 0);
        assert(machine.store64(length, 16) && machine.store64(timeout + 8, 0));
        assert(call(209, {receiver, 1, 66, timeout, length}) == 0);
        assert(load(length) == 16 && load(timeout) == 0 && load(timeout + 8) == 20'000);
        assert(call(207, {receiver, base, 1, 0x40}) == uint64_t(-11) && !linux.blocked());
        assert(call(63, {receiver, base, 1}) == receiver && linux.blocked());
        assert(linux.schedule(machine, steady_clock::now() + seconds(1)));
        assert(call(63, {receiver, base, 1}) == uint64_t(-11) && !linux.blocked());
        assert(call(63, {receiver, base, 1}) == receiver && linux.blocked());
        put(base + 16, "x");
        assert(call(64, {sender, base + 16, 1}) == 1 && !linux.blocked());
        assert(call(63, {receiver, base, 1}) == 1 && *machine.read(base, 1, 4) == 'x');
        constexpr uint64_t buffer = base + 1024, vector = base + 640;
        assert(machine.store64(timeout, 0) && machine.store64(timeout + 8, 30'000));
        assert(call(208, {sender, 1, 21, timeout, 16}) == 0);
        assert(machine.store64(length, 16) && machine.store64(timeout + 8, 0));
        assert(call(209, {sender, 1, 67, timeout, length}) == 0);
        assert(load(length) == 16 && load(timeout) == 0 && load(timeout + 8) == 30'000);
        assert(machine.store64(length, 16) && call(209, {sender, 1, 20, timeout, length}) == 0);
        assert(load(timeout) == 0 && load(timeout + 8) == 0);
        assert(machine.store64(length, 1024) && call(208, {sender, 1, 7, length, 4}) == 0);
        assert(call(64, {sender, buffer, 2048}) == 2048);
        assert(call(206, {sender, buffer, 1, 0x40}) == uint64_t(-11) && !linux.blocked());
        assert(call(206, {sender, buffer, 1}) == sender && linux.blocked());
        assert(linux.schedule(machine, steady_clock::now() + seconds(1)));
        assert(call(206, {sender, buffer, 1}) == uint64_t(-11) && !linux.blocked());
        assert(call(64, {sender, buffer, 1}) == sender && linux.blocked());
        assert(call(63, {receiver, buffer, 1}) == 1 && !linux.blocked());
        assert(call(64, {sender, buffer, 1}) == 1);
        assert(machine.store64(vector, buffer) && machine.store64(vector + 8, 1));
        assert(call(66, {sender, vector, 1}) == sender && linux.blocked());
        assert(linux.schedule(machine, steady_clock::now() + seconds(1)));
        assert(call(66, {sender, vector, 1}) == uint64_t(-11) && !linux.blocked());
        assert(call(57, {sender}) == 0 && call(57, {receiver}) == 0);
    }
    {
        using namespace std::chrono;
        assert(machine.store64(base, 1) && machine.store64(base + 8, 50'000'000));
        assert(call(101, {base, 0}) == base && linux.blocked());
        assert(!linux.schedule(machine, steady_clock::now() + milliseconds(10)) && linux.blocked());
        assert(linux.schedule(machine, steady_clock::now() + seconds(2)) && !linux.blocked());
        assert(call(101, {base, 0}) == 0);
        machine.cpu.retired += 4096;
    }
    {
        constexpr uint64_t cwd = uint32_t(-100), event = base + 512, buffer = base + 1024;
        linux.addBootTmpfs("/watch", 0755, 0, 6);
        put(base, "/watch");
        assert(call(26, {0x1}) == uint64_t(-22));
        const auto monitor = call(26, {0x80800});
        const auto watch = call(27, {monitor, base, 0x3ca});
        assert(monitor < 256 && watch == 1);
        assert(call(27, {monitor, base, 0x100003ca}) == uint64_t(-17));
        assert(call(63, {monitor, buffer, 128}) == uint64_t(-11));
        const auto poll = call(20, {0x80000});
        assert(machine.store64(event, 1) && machine.store64(event + 8, 123));
        assert(call(21, {poll, 1, monitor, event}) == 0);
        assert(call(22, {poll, buffer, 1, 0}) == 0);
        put(base, "/watch/one");
        const auto file = call(56, {cwd, base, 0x42, 0600});
        assert(file < 256 && call(64, {file, base, 2}) == 2 && call(57, {file}) == 0);
        assert(call(22, {poll, buffer, 1, 0}) == 1 && load(buffer) == 1 && load(buffer + 8) == 123);
        assert(call(63, {monitor, buffer, 16}) == uint64_t(-22));
        assert(call(63, {monitor, buffer, 32}) == 32);
        assert(load(buffer) == (uint64_t{0x100} << 32 | watch));
        assert(std::memcmp(machine.read(buffer + 16, 4, 4), "one", 4) == 0);
        assert(call(63, {monitor, buffer, 128}) == 64);
        assert(load(buffer) == (uint64_t{2} << 32 | watch));
        assert(load(buffer + 32) == (uint64_t{8} << 32 | watch));
        put(base + 128, "/watch/two");
        assert(call(38, {cwd, base, cwd, base + 128}) == 0);
        assert(call(63, {monitor, buffer, 128}) == 64);
        assert(load(buffer) == (uint64_t{64} << 32 | watch));
        assert(load(buffer + 32) == (uint64_t{128} << 32 | watch));
        assert(uint32_t(load(buffer + 8)) && load(buffer + 8) == load(buffer + 40));
        assert(call(35, {cwd, base + 128, 0}) == 0 && call(28, {monitor, watch}) == 0);
        assert(call(63, {monitor, buffer, 128}) == 48);
        assert(load(buffer) == (uint64_t{512} << 32 | watch));
        assert(load(buffer + 32) == (uint64_t{0x8000} << 32 | watch));
        assert(call(63, {monitor, buffer, 128}) == uint64_t(-11));
        assert(call(57, {monitor}) == 0 && call(57, {poll}) == 0);
    }
    {
        put(base, "/proc/meminfo");
        assert(call(48, {uint32_t(-100), base, 4}) == 0);
        assert(call(79, {uint32_t(-100), base, base + 512, 0}) == 0);
        assert(load(base + 512 + 48) == 0);
        assert(call(56, {uint32_t(-100), base, 1}) == uint64_t(-30));
        const auto readMemInfo = [&] {
            const auto fd = call(56, {uint32_t(-100), base, 0x80000});
            assert(fd < 256);
            const auto length = call(63, {fd, base + 512, 512});
            assert(length > 0 && length < 512);
            const auto* bytes = machine.read(base + 512, length, 4);
            assert(bytes && call(63, {fd, base + 512, 512}) == 0);
            assert(call(57, {fd}) == 0);
            return std::string(reinterpret_cast<const char*>(bytes), length);
        };
        assert(call(179, {base + 512}) == 0);
        const auto total = load(base + 512 + 32);
        const auto freeBefore = load(base + 512 + 40);
        const auto initial = readMemInfo();
        assert(initial.starts_with("MemTotal: " + std::to_string(total / 1024) + " kB\n"));
        assert(initial.find("MemFree: " + std::to_string(freeBefore / 1024) + " kB\n") != std::string::npos);
        const auto mapped = call(222, {0, pageSize, 3, 0x22, uint32_t(-1), 0});
        assert(mapped != 0 && mapped < uint64_t(-4095) && machine.store64(mapped, 1));
        assert(call(179, {base + 512}) == 0);
        const auto freeAfter = load(base + 512 + 40);
        assert(freeAfter < freeBefore);
        const auto after = readMemInfo();
        assert(after.find("MemFree: " + std::to_string(freeAfter / 1024) + " kB\n") != std::string::npos);
        assert(after.find("SwapTotal: 0 kB\n") != std::string::npos);
        assert(call(215, {mapped, pageSize}) == 0);
    }
    {
        static constexpr std::array<uint8_t, 2> input{'o', 'k'};
        linux.mount("/test/canonical/input", input);
        linux.alias("/test/linked", "/test/canonical");
        put(base, "/test/linked/input");
        assert(call(48, {uint32_t(-100), base, 4}) == 0);
        assert(call(79, {uint32_t(-100), base, base + 128, 0}) == 0);
        assert(load(base + 128 + 48) == input.size());
    }
    {
        constexpr auto attr = base + 96, key = base + 384, data = base + 392, output = base + 400;
        const auto create = [&](uint32_t entries, uint32_t flags = 0) {
            const std::array<uint32_t, 5> args{2, 4, 8, entries, flags};
            assert(machine.write(attr, {reinterpret_cast<const uint8_t*>(args.data()), sizeof(args)}));
            return call(280, {0, attr, sizeof(args)});
        };
        const auto fd = create(2);
        assert(fd < 256 && call(25, {fd, 1}) == 1);
        const auto invoke = [&](uint64_t command, uint64_t descriptor, uint64_t index,
                                uint64_t value, uint64_t flags = 0) {
            assert(machine.store64(attr, descriptor) && machine.store64(attr + 8, index));
            assert(machine.store64(attr + 16, value) && machine.store64(attr + 24, flags));
            return call(280, {command, attr, 32});
        };
        assert(machine.store64(key, 1) && machine.store64(output, UINT64_MAX));
        assert(invoke(1, fd, key, output) == 0 && load(output) == 0);
        constexpr uint64_t expected = 0x1122334455667788ULL;
        assert(machine.store64(data, expected));
        assert(invoke(2, fd, key, data) == 0 && invoke(2, fd, key, data, 1) == uint64_t(-17));
        assert(invoke(2, fd, key, UINT64_MAX, 2) == uint64_t(-14));
        const auto dup = call(25, {fd, 0, 0});
        assert(dup < 256 && dup != fd && call(57, {fd}) == 0);
        assert(invoke(1, fd, key, output) == uint64_t(-9));
        assert(invoke(1, dup, key, output) == 0 && load(output) == expected);
        assert(invoke(3, dup, key, 0) == uint64_t(-22));
        assert(invoke(4, dup, 0, output) == 0 && (load(output) & UINT32_MAX) == 0);
        assert(invoke(4, dup, key, output) == uint64_t(-2));
        assert(machine.store64(key, 2));
        assert(invoke(1, dup, key, output) == uint64_t(-2));
        assert(invoke(2, dup, key, data, 2) == uint64_t(-7));
        assert(invoke(4, dup, key, output) == 0 && (load(output) & UINT32_MAX) == 0);
        assert(call(57, {dup}) == 0);
        assert(machine.store64(key, 0));
        const auto readOnly = create(2, 8);
        assert(readOnly < 256 && invoke(1, readOnly, key, output) == 0);
        assert(invoke(2, readOnly, key, data) == uint64_t(-1));
        assert(call(57, {readOnly}) == 0);
        assert(create(0) == uint64_t(-22) && create(UINT32_MAX) == uint64_t(-12));
        const std::array<uint32_t, 5> hashArgs{1, 16, 8, 2, 1}; // BPF_F_NO_PREALLOC
        assert(machine.write(attr, {reinterpret_cast<const uint8_t*>(hashArgs.data()), sizeof(hashArgs)}));
        const auto hash = call(280, {0, attr, sizeof(hashArgs)});
        assert(hash < 256);
        constexpr auto hashKey = base + 512, hashOut = base + 544;
        assert(machine.store64(hashKey, 1) && machine.store64(hashKey + 8, 7));
        assert(invoke(1, hash, hashKey, output) == uint64_t(-2));
        assert(invoke(2, hash, hashKey, data, 2) == uint64_t(-2));
        assert(invoke(2, hash, hashKey, data, 1) == 0);
        assert(invoke(2, hash, hashKey, data, 1) == uint64_t(-17));
        assert(machine.store64(hashKey + 8, 8) && invoke(2, hash, hashKey, data, 1) == 0);
        assert(machine.store64(hashKey + 8, 9) && invoke(2, hash, hashKey, data, 1) == uint64_t(-7));
        assert(invoke(4, hash, 0, hashOut) == 0 && load(hashOut) == 1 && load(hashOut + 8) == 7);
        assert(invoke(4, hash, hashOut, hashOut) == 0 && load(hashOut + 8) == 8);
        assert(invoke(4, hash, hashOut, hashOut) == uint64_t(-2));
        assert(invoke(2, hash, hashOut, UINT64_MAX, 2) == uint64_t(-14));
        assert(invoke(1, hash, hashOut, output) == 0 && load(output) == expected);
        assert(invoke(3, hash, hashOut, 0) == 0);
        assert(invoke(1, hash, hashOut, output) == uint64_t(-2));
        assert(invoke(3, hash, hashOut, 0) == uint64_t(-2));
        assert(call(57, {hash}) == 0);
        for (const uint32_t type : {5, 6}) {
            const std::array<uint32_t, 5> percpuArgs{type, 4, 5, 2, 0};
            assert(machine.write(attr, {reinterpret_cast<const uint8_t*>(percpuArgs.data()), sizeof(percpuArgs)}));
            const auto perCpu = call(280, {0, attr, sizeof(percpuArgs)});
            assert(perCpu < 256 && machine.store64(key, 1));
            assert(invoke(2, perCpu, key, data) == 0);
            assert(machine.store64(output + 8, 0xfeed));
            assert(invoke(1, perCpu, key, output) == 0 && load(output) == expected && load(output + 8) == 0xfeed);
            assert(call(57, {perCpu}) == 0);
        }
        put(base, "/sys/devices/system/cpu/possible");
        const auto possible = call(56, {uint32_t(-100), base, 0});
        assert(possible < 256 && call(63, {possible, output, 8}) == 2);
        assert((load(output) & 0xffff) == 0x0a30);
        assert(call(57, {possible}) == 0);
    }
    {
        constexpr auto from = base + 0x600, to = base + 0x680, attr = base + 0x700;
        constexpr auto key = base + 0x740, value = base + 0x748, output = base + 0x750;
        constexpr auto fs = base + 32, mountpoint = base + 64;
        put(fs, "bpf");
        put(mountpoint, "/sys/fs/bpf");
        assert(call(40, {fs, mountpoint, fs, 14}) == 0);
        put(from, "/sys/fs/bpf/nestvane_map");
        put(to, "/sys/fs/bpf/nestvane_moved");
        const std::array<uint32_t, 5> create{2, 4, 8, 2, 0};
        assert(machine.write(attr, {reinterpret_cast<const uint8_t*>(create.data()), sizeof(create)}));
        const auto map = call(280, {0, attr, sizeof(create)});
        assert(map < 256 && machine.store64(key, 1) && machine.store64(value, 42));
        const auto elem = [&](uint64_t command, uint64_t fd, uint64_t slot) {
            assert(machine.store64(attr, fd) && machine.store64(attr + 8, key));
            assert(machine.store64(attr + 16, slot) && machine.store64(attr + 24, 0));
            return call(280, {command, attr, 32});
        };
        const auto object = [&](uint64_t command, uint64_t path, uint64_t fdAndFlags) {
            assert(machine.store64(attr, path) && machine.store64(attr + 8, fdAndFlags));
            assert(machine.store64(attr + 16, 0));
            return call(280, {command, attr, 24});
        };
        assert(elem(2, map, value) == 0);
        assert(object(6, from, map) == 0 && object(6, from, map) == uint64_t(-17));
        assert(call(56, {uint32_t(-100), from, 0}) == uint64_t(-5));
        assert(call(79, {uint32_t(-100), from, base + 0x780, 0}) == 0);
        assert((load(base + 0x790) & 0777) == 0600);
        assert(call(57, {map}) == 0);
        assert(call(38, {uint32_t(-100), from, uint32_t(-100), to}) == 0);
        assert(object(7, from, 0) == uint64_t(-2));
        const auto reopened = object(7, to, 8ULL << 32);
        assert(reopened < 256 && call(25, {reopened, 1}) == 1);
        assert(call(35, {uint32_t(-100), to, 0}) == 0 && object(7, to, 0) == uint64_t(-2));
        assert(elem(1, reopened, output) == 0 && load(output) == 42);
        assert(elem(2, reopened, value) == uint64_t(-1));
        assert(call(57, {reopened}) == 0);
        assert(call(39, {mountpoint, 0}) == 0);
    }
    {
        constexpr auto attr = base + 96, code = base + 256, license = base + 280, address = base + 384;
        constexpr auto message = base + 512, buffers = base + 600, packet = base + 640;
        put(license, "Apache 2.0");
        const auto program = [&](uint64_t first) {
            assert(machine.store64(code, first) && machine.store64(code + 8, 0x95)); // EXIT
            const std::array<uint64_t, 8> request{(2ULL << 32) | 1, code, license, 0, 0, 0, 0, 0};
            assert(machine.write(attr, {reinterpret_cast<const uint8_t*>(request.data()), sizeof(request)}));
            return call(280, {5, attr, sizeof(request)}); // BPF_PROG_LOAD
        };
        assert(program(0xff) == uint64_t(-22));
        assert(program(0x05 | (uint64_t{3} << 16)) == uint64_t(-22)); // Out-of-range jump.
        constexpr auto fs = base + 32, mountpoint = base + 64, pinned = base + 880;
        put(fs, "bpf");
        put(mountpoint, "/sys/fs/bpf");
        put(pinned, "/sys/fs/bpf/prog_nestvane_filter");
        assert(call(40, {fs, mountpoint, fs, 14}) == 0);
        const auto uevent = call(198, {16, 2 | 0x80800, 15});
        assert(uevent < 256);
        assert(machine.store64(address, 16) && machine.store64(address + 8, 1));
        assert(call(200, {uevent, address, 12}) == 0);
        put(base, "/dev/loop-control");
        const auto control = call(56, {uint32_t(-100), base, 2});
        assert(control < 256);
        for (const uint64_t limit : {0, 16}) {
            const auto loaded = program(0xb7 | (limit << 32)); // MOV64 r0, immediate
            assert(loaded < 256 && machine.store64(attr, pinned));
            assert(machine.store64(attr + 8, loaded) && machine.store64(attr + 16, 0));
            assert(call(280, {6, attr, 24}) == 0 && call(57, {loaded}) == 0); // BPF_OBJ_PIN
            assert(machine.store64(attr + 8, 8ULL << 32)); // BPF_F_RDONLY
            const auto reopened = call(280, {7, attr, 24}); // BPF_OBJ_GET
            assert(reopened < 256 && call(25, {reopened, 1}) == 1);
            assert(call(35, {uint32_t(-100), pinned, 0}) == 0);
            assert(machine.store64(address, reopened));
            assert(call(208, {uevent, 1, 50, address, 4}) == 0 && call(57, {reopened}) == 0);
            assert(call(29, {control, 0x4c80, 31 + limit}) == 31 + limit); // LOOP_CTL_ADD
            const std::array<uint64_t, 7> header{0, 0, buffers, 1, 0, 0, 0};
            assert(machine.store64(buffers, packet) && machine.store64(buffers + 8, 256));
            assert(machine.write(message, {reinterpret_cast<const uint8_t*>(header.data()), sizeof(header)}));
            assert(call(212, {uevent, message, 0}) == (limit ? limit : uint64_t(-11)));
        }
        assert(call(57, {control}) == 0 && call(57, {uevent}) == 0);
        assert(call(39, {mountpoint, 0}) == 0);
    }
    {
        constexpr auto attr = base + 96;
        put(base, "/sys/fs/bpf/map_missing");
        assert(machine.store64(attr, base) && machine.store64(attr + 8, 0));
        assert(machine.store64(attr + 16, 0));
        assert(call(280, {7, attr, 24}) == uint64_t(-2));
        put(base, "/sys/fs/bpf");
        assert(call(280, {7, attr, 24}) == uint64_t(-13));
        assert(machine.store64(attr + 8, 1));
        assert(call(280, {7, attr, 24}) == uint64_t(-22));
        assert(call(280, {7, UINT64_MAX, 24}) == uint64_t(-14));
    }
    {
        constexpr auto buffer = base + 96;
        put(base, "/proc/sys/kernel/random/boot_id");
        const auto bootId = call(56, {uint32_t(-100), base, 0});
        assert(bootId < 256 && call(63, {bootId, buffer, 37}) == 37);
        const auto* uuid = machine.read(buffer, 36, 4);
        assert(uuid);
        const auto path = "/dev/ashmem" + std::string(reinterpret_cast<const char*>(uuid), 36);
        assert(call(57, {bootId}) == 0);
        put(base, path.c_str());
        assert(call(79, {uint32_t(-100), base, buffer, 0}) == 0 && load(buffer + 32) == 0x100a03);
        const auto fd = call(56, {uint32_t(-100), base, 2});
        assert(fd < 256);
        assert(call(80, {fd, buffer}) == 0 && load(buffer + 32) == 0x100a03);
        put(base, "shared-memory-regression");
        assert(call(29, {fd, 0x41007701, base}) == 0);
        assert(call(29, {fd, 0x81007702, buffer}) == 0);
        assert(std::memcmp(machine.read(buffer, 25, 4), "shared-memory-regression", 25) == 0);
        assert(call(29, {fd, 0x40087703, pageSize}) == 0);
        assert(call(29, {fd, 0x7704}) == pageSize);
        const auto writable = call(222, {0, pageSize, 3, 1, fd});
        assert(writable < (1ULL << 37));
        assert(call(29, {fd, 0x40087703, pageSize * 2}) == uint64_t(-22));
        assert(call(29, {fd, 0x40087705, 1}) == 0);
        assert(call(222, {0, pageSize, 3, 1, fd}) == uint64_t(-1));
        const auto shared = call(222, {0, pageSize, 1, 1, fd});
        assert(shared < (1ULL << 37));
        assert(machine.store64(writable, 0x1234) && load(shared) == 0x1234);
        assert(call(226, {shared, pageSize, 3}) == uint64_t(-13));
        assert(call(233, {shared, pageSize, 8}) == uint64_t(-22));
        assert(call(57, {fd}) == 0 && load(shared) == 0x1234);
        assert(call(215, {writable, pageSize}) == 0 && call(215, {shared, pageSize}) == 0);
    }
    {
        constexpr uint64_t size = 4 * 1024 * 1024, residency = base + 96;
        const auto mapping = call(222, {0, size, 3, 0x22, UINT64_MAX});
        assert(mapping < (1ULL << 37));
        assert(call(232, {mapping, pageSize, residency}) == 0 && *machine.read(residency, 1, 4) == 0);
        assert(machine.store64(mapping, 42));
        assert(call(233, {mapping, pageSize, 8}) == 0);
        assert(call(232, {mapping, pageSize, residency}) == 0 && *machine.read(residency, 1, 4) == 1);
        assert(call(215, {mapping, size}) == 0);
        assert(call(232, {mapping, pageSize, residency}) == uint64_t(-12));
    }
    {
        ProbeLinux guest(1, 0);
        guest.addBootTmpfs("/metadata", 0755, 0, 6);
        guest.addBootTmpfs("/scratch", 0777, 0, 6);
        constexpr std::array<uint8_t, 2> defaults{'o', 'k'};
        guest.mount("/system/etc/example", defaults);
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args = {}) {
            std::copy(args.begin(), args.end(), task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        const auto path = [&](std::string_view value) {
            const std::string text(value);
            assert(task.write(base, {reinterpret_cast<const uint8_t*>(text.c_str()), text.size() + 1}));
        };
        constexpr uint64_t cwd = uint32_t(-100), scratch = base + 256;
        path("/metadata/maps");
        assert(invoke(34, {cwd, base, 0775}) == 0);
        assert(invoke(54, {cwd, base, 0, 1000, 0}) == 0);
        assert(invoke(53, {cwd, base, 0775, 0}) == 0); // Restore group write removed by umask.
        path("/metadata/private");
        assert(invoke(34, {cwd, base, 0700}) == 0);
        assert(task.store64(scratch, 1000));
        assert(invoke(159, {1, scratch}) == 0);
        assert(invoke(149, {2000, 2000, 2000}) == 0 && invoke(147, {2000, 2000, 2000}) == 0);
        path("/metadata/denied");
        assert(invoke(56, {cwd, base, 0x41, 0600}) == uint64_t(-13));
        path("/metadata/private/denied");
        assert(invoke(56, {cwd, base, 0x41, 0600}) == uint64_t(-13));
        path("/metadata/maps/test");
        const auto file = invoke(56, {cwd, base, 0x42, 0600});
        path("/system/etc/example");
        const auto input = invoke(56, {cwd, base, 0});
        assert(input < 256 && file < 256);
        assert(invoke(285, {input, 0, file, 0, 2}) == uint64_t(-18));
        assert(invoke(285, {file, 0, file, 0, 2}) == uint64_t(-95));
        assert(invoke(285, {input, 0xdead, file, 0, 2}) == uint64_t(-14));
        assert(invoke(285, {input, 0, input, 0, 2}) == uint64_t(-9));
        assert(invoke(285, {input, 0, file, 0, 2, 1}) == uint64_t(-22));
        uint64_t contents = 0;
        assert(task.store64(scratch + 8, 1));
        assert(invoke(71, {file, input, 0xdead, 2}) == uint64_t(-14));
        assert(invoke(71, {file, input, scratch + 8, 2}) == 1);
        assert(invoke(62, {input, 0, 1}) == 0);
        assert(task.load64(scratch + 8, contents) && contents == 2);
        assert(invoke(71, {file, input, 0, 2}) == 2);
        assert(invoke(71, {file, input, 0, 2}) == 0);
        assert(invoke(67, {file, scratch + 8, 3, 0}) == 3);
        assert(std::memcmp(task.read(scratch + 8, 3, 4), "kok", 3) == 0);
        assert(invoke(62, {file, 0, 0}) == 0);
        assert(invoke(64, {file, scratch, 8}) == 8 && invoke(57, {file}) == 0);
        path("/metadata/maps/test");
        const auto reopened = invoke(56, {cwd, base, 0});
        assert(reopened < 256 && invoke(63, {reopened, scratch, 8}) == 8);
        assert(task.load64(scratch, contents) && contents == 1000);
        constexpr char across[] = "/scratch/test";
        constexpr char moved[] = "/metadata/maps/moved";
        assert(task.write(scratch, {reinterpret_cast<const uint8_t*>(across), sizeof(across)}));
        assert(invoke(38, {cwd, base, cwd, scratch}) == uint64_t(-18));
        assert(task.write(scratch, {reinterpret_cast<const uint8_t*>(moved), sizeof(moved)}));
        assert(invoke(38, {cwd, base, cwd, scratch}) == 0);
        assert(invoke(56, {cwd, base, 0}) == uint64_t(-2));
        path(moved);
        assert(invoke(35, {cwd, base, 0}) == 0);
    }
    {
        ProbeLinux guest(1, 0);
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto clone = [&](uint64_t flags, uint64_t stack) {
            task.cpu.x[0] = flags;
            task.cpu.x[1] = stack;
            task.cpu.x[2] = task.cpu.x[4] = base;
            task.cpu.x[3] = 0;
            task.cpu.x[8] = 220;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        for (uint64_t tid = 2; tid <= 256; ++tid)
            assert(clone(0x3d0f00, base + 256) == tid);
        assert(clone(0x3d0f00, base + 256) == uint64_t(-11));
        assert(clone(0x1200011, 0) == uint64_t(-11));
    }
    {
        ProbeLinux guest(1, 0);
        guest.addBootCgroups();
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args = {}) {
            std::copy(args.begin(), args.end(), task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        const auto path = [&](std::string_view text) {
            const std::string value(text);
            assert(task.write(base, {reinterpret_cast<const uint8_t*>(value.c_str()), value.size() + 1}));
        };
        constexpr uint64_t cwd = uint32_t(-100), output = base + 256;
        path("/sys/fs/cgroup/system");
        assert(invoke(43, {base, output}) == 0);
        uint64_t magic = 0;
        assert(task.load64(output, magic) && magic == 0x63677270);
        path("/sys/fs/cgroup/system/uid_1000");
        assert(invoke(34, {cwd, base, 0775}) == 0);
        path("/sys/fs/cgroup/system/uid_1000/cgroup.procs");
        const auto procs = invoke(56, {cwd, base, 2});
        assert(procs < 256);
        path("0\n");
        assert(invoke(64, {procs, base, 2}) == 2);
        path("/proc/self/cgroup");
        const auto proc = invoke(56, {cwd, base, 0});
        constexpr std::string_view expected = "0::/system/uid_1000\n";
        assert(invoke(63, {proc, output, 128}) == expected.size());
        assert(std::memcmp(task.read(output, expected.size(), 4), expected.data(), expected.size()) == 0);
        assert(invoke(220, {0x1200011, 0, 0, 0, base + 128}) == 2);
        assert(invoke(63, {procs, output, 128}) == 4);
        assert(std::memcmp(task.read(output, 4, 4), "1\n2\n", 4) == 0);
        path("/sys/fs/cgroup/system/uid_1000");
        assert(invoke(35, {cwd, base, 0x200}) == uint64_t(-16));
    }
    {
        ProbeLinux guest(1, 1000);
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args = {}) {
            std::copy(args.begin(), args.end(), task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        // Deny getuid only on AArch64. Allow other calls, including fork.
        const std::array<uint32_t, 12> filter{
            0x20, 4, 0x03000015, 0xc00000b7, 0x20, 0,
            0x01000015, 174, 0x06, 0x00050001, 0x06, 0x7fff0000,
        };
        constexpr auto instructions = base + 64;
        assert(task.store64(base, filter.size() / 2));
        assert(task.store64(base + 8, instructions));
        assert(task.write(instructions, {reinterpret_cast<const uint8_t*>(filter.data()), sizeof(filter)}));
        assert(invoke(167, {22, 2, base}) == uint64_t(-13));
        assert(invoke(167, {38, 1}) == 0 && invoke(167, {39}) == 1);
        assert(invoke(167, {22, 2, base + 4096}) == uint64_t(-14));
        assert(task.store64(instructions, filter.size() * (uint64_t{1} << 32) + 0x05));
        assert(invoke(167, {22, 2, base}) == uint64_t(-22));
        assert(task.write(instructions, {reinterpret_cast<const uint8_t*>(filter.data()), sizeof(filter)}));
        assert(invoke(167, {22, 2, base}) == 0 && invoke(167, {21}) == 2);
        assert(task.store64(instructions + 32, 0x7fff000000000006)); // Installed code is a snapshot.
        assert(invoke(174) == uint64_t(-1) && invoke(172) == 1);
        assert(invoke(220, {0x1200011, 0, 0, 0, base + 128}) == 2);
        assert(guest.schedule(task) && guest.threadId() == 2);
        assert(invoke(167, {39}) == 1 && invoke(174) == uint64_t(-1));
    }
    {
        ProbeLinux guest(1, 1000);
        guest.setBootContext("u:r:zygote:s0");
        assert(guest.loadCurrentContextPolicy("u:r:zygote:s0 u:r:system_server_startup:s0\n"));
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args = {}) {
            std::copy(args.begin(), args.end(), task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        constexpr char current[] = "/proc/thread-self/attr/current";
        constexpr char validator[] = "/sys/fs/selinux/context";
        constexpr char denied[] = "u:r:init:s0";
        constexpr char allowed[] = "u:r:system_server_startup:s0";
        assert(task.write(base, {reinterpret_cast<const uint8_t*>(current), sizeof(current)}));
        const auto attribute = invoke(56, {uint32_t(-100), base, 2});
        assert(attribute < 256);
        assert(task.write(base, {reinterpret_cast<const uint8_t*>(validator), sizeof(validator)}));
        const auto validate = invoke(56, {uint32_t(-100), base, 2});
        assert(validate < 256);
        assert(task.write(base, {reinterpret_cast<const uint8_t*>(denied), sizeof(denied)}));
        assert(invoke(64, {validate, base, sizeof(denied)}) == uint64_t(-22));
        assert(invoke(64, {attribute, base, sizeof(denied)}) == uint64_t(-13));
        assert(task.write(base, {reinterpret_cast<const uint8_t*>(allowed), sizeof(allowed)}));
        assert(invoke(64, {validate, base, sizeof(allowed)}) == sizeof(allowed));
        assert(invoke(64, {attribute, base, sizeof(allowed)}) == sizeof(allowed));
        assert(invoke(63, {attribute, base, sizeof(allowed)}) == sizeof(allowed));
        assert(std::memcmp(task.read(base, sizeof(allowed), 4), allowed, sizeof(allowed)) == 0);
    }
    {
        constexpr auto alt = base + 2 * pageSize;
        const std::array<uint32_t, 2> instructions{0xb9400022, 0xd65f03c0}; // ldr w2,[x1]; ret
        std::vector<uint8_t> code(sizeof(instructions));
        std::memcpy(code.data(), instructions.data(), code.size());
        Machine task(Image{base, {{base, std::move(code), 5}, {base + pageSize, std::vector<uint8_t>(3 * pageSize), 6}}, {}});
        ProbeLinux guest(1, 1000);
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args = {}) {
            std::copy(args.begin(), args.end(), task.cpu.x.begin());
            task.cpu.x[8] = number;
            assert(guest.syscall(task));
            return task.cpu.x[0];
        };
        constexpr auto parameters = base + pageSize;
        const std::array<uint64_t, 4> action{base + 4, 0x08000004, 0, 1ULL << (12 - 1)};
        assert(task.write(parameters, {reinterpret_cast<const uint8_t*>(action.data()), sizeof(action)}));
        assert(invoke(134, {11, parameters, 0, 8}) == 0);
        const std::array<uint64_t, 3> stack{alt, 0, 2 * pageSize};
        assert(task.write(parameters, {reinterpret_cast<const uint8_t*>(stack.data()), sizeof(stack)}));
        assert(invoke(132, {parameters}) == 0);
        task.cpu.sp = alt - 16;
        task.cpu.nzcv = 9;
        task.cpu.v[4][7] = 168;
        task.cpu.x[17] = 0x1234;
        task.cpu.x[2] = 0x5678;
        task.cpu.x[1] = 0;
        const auto before = task.cpu;
        assert(executeStartupInstruction(task, instructions[0]));
        assert(task.cpu.stop == Stop::fault && guest.deliverMemoryFault(task));
        assert(task.cpu.pc == base + 4 && task.cpu.x[0] == 11);
        assert(task.cpu.sp >= alt && task.cpu.sp < alt + stack[2] && !(task.cpu.sp & 15));
        assert(task.cpu.x[1] == task.cpu.sp && task.cpu.x[2] == task.cpu.sp + 128);
        uint64_t value = 0;
        assert(task.load64(task.cpu.x[1] + 16, value) && value == 0); // si_addr
        assert(task.load64(task.cpu.x[1] + 8, value) && value == 1); // SEGV_MAPERR
        assert(task.load64(task.cpu.x[2] + 176 + 264, value) && value == base);
        assert(task.store64(task.cpu.x[2] + 176 + 264, base + 4)); // Change resume PC in ucontext.
        const auto trampoline = task.cpu.x[30];
        assert(executeStartupInstruction(task, instructions[1]));
        assert(task.cpu.pc == trampoline);
        uint32_t mov;
        const auto* entry = task.read(trampoline, sizeof(mov), 1);
        assert(entry != nullptr);
        std::memcpy(&mov, entry, sizeof(mov));
        const auto decoded = decode(mov);
        if (decoded.op == Op::unsupported) assert(executeStartupInstruction(task, mov));
        else executeDecoded(task, decoded);
        assert(task.cpu.x[8] == 139 && guest.syscall(task) && guest.restoredSignalContext());
        assert(task.cpu.pc == base + 4 && task.cpu.sp == before.sp && task.cpu.nzcv == before.nzcv);
        assert(task.cpu.x == before.x && task.cpu.v == before.v);
        assert(invoke(135, {0, 0, parameters, 8}) == 0);
        assert(task.load64(parameters, value) && value == 0);
    }
    assert(call(143, {0, 9999}) == 0 && call(176) == 0 && call(177) == 9999);
    assert(call(145, {UINT32_MAX, 9999}) == 0 && call(174) == 0 && call(175) == 9999);
    assert(call(145, {123, UINT32_MAX}) == uint64_t(-1));
    assert(call(143, {UINT32_MAX, 123}) == uint64_t(-1));
    assert(call(143, {UINT32_MAX, 0}) == 0 && call(177) == 0);
    assert(call(145, {UINT32_MAX, 0}) == 0 && call(175) == 0);
    assert(call(143, {0, 0}) == 0 && call(145, {0, 0}) == 0);
    {
        ProbeLinux guarded(1, 0);
        Machine guardedMachine(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        const auto invoke = [&](uint64_t number, std::array<uint64_t, 6> args = {}) {
            std::copy(args.begin(), args.end(), guardedMachine.cpu.x.begin());
            guardedMachine.cpu.x[8] = number;
            assert(guarded.syscall(guardedMachine));
            return guardedMachine.cpu.x[0];
        };
        const auto area = invoke(222, {0, pageSize, 1, 0x22, uint64_t(-1), 0});
        assert(area < (uint64_t{1} << 45));
        assert(invoke(167, {56}) == 0 && invoke(167, {55, 2}) == uint64_t(-22));
        assert(invoke(167, {55, 3}) == 0 && invoke(167, {56}) == 3);
        assert(invoke(167, {55, 0}) == uint64_t(-1));
        assert(invoke(226, {area, pageSize, 5}) == uint64_t(-13));
        assert(invoke(222, {0, pageSize, 7, 0x22, uint64_t(-1), 0}) == uint64_t(-13));
        assert(invoke(220, {0x1200011, 0, 0, 0, base}) == 2);
        assert(guarded.schedule(guardedMachine) && guarded.processId() == 2);
        assert(invoke(167, {56}) == 0 && invoke(226, {area, pageSize, 5}) == 0);
    }
    assert(machine.store64(base + 0x680, 0) && machine.store64(base + 0x688, 0));
    assert(machine.store64(base + 0x690, 0));
    assert(call(73, {base + 0x690, 1, base + 0x680}) == 0);
    assert(machine.store64(base + 0x690, 1ULL << 32));
    assert(call(73, {base + 0x690, 1, base + 0x680}) == 1);
    assert(load(base + 0x690) >> 48 == 1);
    assert(machine.store64(base + 512, 120));
    assert(call(170, {0, base + 512}) == 0);
    assert(call(169, {0, base + 520}) == 0 && load(base + 520) == 120);
    assert(machine.store64(base + 512, 901));
    assert(call(170, {0, base + 512}) == uint64_t(-22));
    assert(call(169, {0, base + 520}) == 0 && load(base + 520) == 120);

    assert(call(30, {1, 0, 0x4002}) == 0);
    assert(call(31, {1, 1}) == 0x4002);
    assert(call(30, {1, 1, 0x4008}) == uint64_t(-22));
    assert(call(31, {1, 0}) == 0x4002);

    put(base, "tmpfs");
    put(base + 16, "/test");
    put(base + 40, "mode=0750,uid=0,gid=0");
    put(base + 72, "/test/sample");
    assert(call(56, {uint32_t(-100), base + 72, 0x42, 0600}) == uint64_t(-30));
    assert(call(40, {base, base + 16, base, 14, base + 40}) == 0);
    assert(call(43, {base + 16, base + 512}) == 0);
    assert(load(base + 512 + 80) == 0x2e);
    auto file = call(56, {uint32_t(-100), base + 72, 0x42, 0600});
    assert(file == 3);
    put(base + 340, "disk");
    assert(call(68, {file, base + 340, 4, 4}) == 4);
    assert(call(62, {file, 0, 1}) == 0);
    assert(call(67, {file, base + 512, 8, 0}) == 8 && (load(base + 512) & UINT32_MAX) == 0);
    assert(std::memcmp(machine.read(base + 516, 4, 4), "disk", 4) == 0);
    assert(call(68, {file, base + 340, 4, UINT64_MAX}) == uint64_t(-22));
    assert(machine.store64(base + 520, 0x40000000));
    assert(call(29, {file, 0x80086601, base + 520}) == 0 && load(base + 520) == 0);
    assert(call(29, {file, 0x40086602, base + 520}) == uint64_t(-95));
    assert(call(29, {file, 0x40806685, base + 520}) == uint64_t(-25)); // FS_IOC_ENABLE_VERITY
    assert(call(29, {file, 0xc0046686, base + 520}) == uint64_t(-25)); // FS_IOC_MEASURE_VERITY
    assert(call(29, {file, 0xc0286687, base + 520}) == uint64_t(-25)); // FS_IOC_READ_VERITY_METADATA
    assert(load(base + 520) == 0);
    assert(call(233, {base, pageSize, UINT32_MAX}) == uint64_t(-22));
    assert(call(57, {file}) == 0);
    assert(call(39, {base + 16}) == uint64_t(-16));

    {
        linux.captureAndroidLogs = true;
        const auto logSocket = call(198, {1, 2, 0}); // AF_UNIX SOCK_DGRAM
        assert(logSocket < 256);
        constexpr char address[] = "\x01\0/dev/socket/logdw";
        constexpr char tag[] = "Watchdog";
        constexpr char message[] = "*** WATCHDOG KILLING SYSTEM PROCESS: test";
        const auto write = [&](uint64_t destination, const auto& data) {
            assert(machine.write(destination, {reinterpret_cast<const uint8_t*>(&data), sizeof(data)}));
        };
        write(base + 256, address);
        assert(call(203, {logSocket, base + 256, sizeof(address)}) == 0);
        write(base + 288, tag);
        write(base + 320, message);
        const std::array<std::array<uint64_t, 2>, 4> vectors{{
            {base, 1}, {base, 1}, {base + 288, sizeof(tag)}, {base + 320, sizeof(message)},
        }};
        write(base + 384, vectors);
        assert(call(66, {logSocket, base + 384, vectors.size()}) == 2 + sizeof(tag) + sizeof(message));
        assert(linux.watchdogReport.find("guest task 1/1 ") != std::string::npos);
        assert(call(57, {logSocket}) == 0);
        linux.captureAndroidLogs = false;
    }
    const auto poll = call(20, {0x80000});
    const auto event = call(19, {0, 0x80800});
    assert(poll == 3 && event == 4);
    assert(machine.store64(base + 256, 1));
    assert(machine.store64(base + 264, 0xdecafe));
    assert(call(21, {poll, 1, event, base + 256}) == 0);
    assert(call(22, {poll, base + 512, 1}) == 0);
    assert(call(63, {event, base + 520, 8}) == uint64_t(-11));
    assert(machine.store64(base + 280, 3));
    assert(call(64, {event, base + 280, 8}) == 8);
    assert(call(22, {poll, base + 512, 1}) == 1);
    assert(load(base + 512) == 1 && load(base + 520) == 0xdecafe);
    assert(call(63, {event, base + 512, 8}) == 8 && load(base + 512) == 3);
    assert(call(22, {poll, base + 512, 1}) == 0);

    for (const uint64_t clock : {1, 9}) {
        const auto timer = call(85, {clock, 0x80800});
        assert(timer < 256 && call(21, {poll, 1, timer, base + 256}) == 0);
        assert(call(63, {timer, base + 512, 8}) == uint64_t(-11));
        const std::array<uint64_t, 4> expiredTimer{0, 0, 0, 1};
        assert(machine.write(base + 280, {reinterpret_cast<const uint8_t*>(expiredTimer.data()), sizeof expiredTimer}));
        assert(call(86, {timer, 1, base + 280, base + 0x380}) == 0);
        assert(load(base + 0x390) == 0 && load(base + 0x398) == 0);
        assert(call(22, {poll, base + 512, 1}) == 1);
        assert(call(63, {timer, base + 512, 8}) == 8 && load(base + 512) == 1);
        assert(call(22, {poll, base + 512, 1}) == 0);
        assert(call(57, {timer}) == 0);
    }
    {
        ProbeLinux unprivileged(1, 1000);
        Machine task(Image{base, {{base, std::vector<uint8_t>(pageSize), 6}}, {}});
        task.cpu.x[0] = 9;
        task.cpu.x[8] = 85;
        assert(unprivileged.syscall(task) && task.cpu.x[0] == uint64_t(-1));
    }
    {
        const auto touch = std::make_shared<GuestTouch>();
        linux.setGuestTouch(touch);
        touch->submit(100, 100, true); // A tap on the boot screen has no guest reader.
        put(base + 704, "/dev/input");
        const auto directory = call(56, {uint32_t(-100), base + 704, 0x4000});
        assert(directory < 256);
        const auto listing = call(61, {directory, base + 768, 192});
        const auto* entries = machine.read(base + 768, listing, 4);
        assert(entries && std::string_view(reinterpret_cast<const char*>(entries), listing).find("event0") != std::string_view::npos);
        assert(call(57, {directory}) == 0);
        put(base + 704, "/dev/input/event0");
        const auto input = call(56, {uint32_t(-100), base + 704, 0x80802});
        assert(input < 256 && call(56, {uint32_t(-100), base + 704, 0x802}) == uint64_t(-16));
        assert(call(29, {input, 0x80044501, base + 760}) == 0 && (load(base + 760) & UINT32_MAX) == 0x010001);
        assert(call(29, {input, 0x80604521, base + 768}) == 96);
        assert(*machine.read(base + 768 + GuestTouch::touchKey / 8, 1, 4) == 1 << (GuestTouch::touchKey % 8));
        assert(machine.store64(base + 688, 0x20000001) && machine.store64(base + 696, 42));
        assert(call(21, {poll, 1, input, base + 688}) == 0);
        assert(call(63, {input, base + 768, 96}) == uint64_t(-11));
        assert(call(22, {poll, base + 512, 1, UINT32_MAX}) == poll && linux.blocked());
        touch->submit(123, 456, true);
        assert(linux.schedule(machine, std::chrono::steady_clock::now() + std::chrono::seconds(1)));
        assert(!linux.blocked());
        assert(call(22, {poll, base + 512, 1}) == 1 && load(base + 512) == 1 && load(base + 520) == 42);
        assert(call(63, {input, 0, 96}) == uint64_t(-14));
        assert(call(63, {input, base + 768, 96}) == 96);
        const auto event = [&](size_t index) {
            GuestTouch::Event data{};
            const auto* bytes = machine.read(base + 768 + index * sizeof(data), sizeof(data), 4);
            assert(bytes);
            std::memcpy(&data, bytes, sizeof(data));
            return data;
        };
        assert(event(0).type == 3 && event(0).code == 0 && event(0).value == 123);
        assert(event(1).type == 3 && event(1).code == 1 && event(1).value == 456);
        assert(event(2).type == 1 && event(2).code == GuestTouch::touchKey && event(2).value == 1);
        assert(event(3).type == 0 && event(3).code == 0);
        assert(call(22, {poll, base + 512, 1}) == 0);
        touch->submit(800, 920, false);
        assert(call(63, {input, base + 768, 96}) == 96);
        assert(event(2).code == GuestTouch::touchKey && event(2).value == 0);
        assert(call(57, {input}) == 0);
        linux.setGuestTouch({});
    }

    assert(call(199, {1, 5 | 0x800, 0, base + 512}) == 0);
    auto sockets = load(base + 512);
    auto first = uint32_t(sockets);
    auto second = uint32_t(sockets >> 32);
    assert(first == 5 && second == 6);
    assert(call(21, {poll, 1, second, base + 256}) == 0);
    put(base + 296, "guest packet");
    assert(call(64, {first, base + 296, 12}) == 12);
    assert(call(22, {poll, base + 512, 1}) == 1);
    assert(load(base + 512) == 1);
    assert(call(63, {second, base + 512, 32}) == 12);
    assert(std::memcmp(machine.read(base + 512, 12, 4), "guest packet", 12) == 0);
    const std::array<uint64_t, 7> sendHeader{0, 0, base + 640, 2, 0, 0, 0};
    std::array<uint64_t, 4> sendVectors{base + 296, 6, 0, 6};
    assert(machine.write(base + 576, {reinterpret_cast<const uint8_t*>(sendHeader.data()), sizeof(sendHeader)}));
    assert(machine.write(base + 640, {reinterpret_cast<const uint8_t*>(sendVectors.data()), sizeof(sendVectors)}));
    assert(call(211, {first, base + 576, 0x4040}) == uint64_t(-14));
    assert(call(22, {poll, base + 512, 1}) == 0);
    sendVectors[2] = base + 302;
    assert(machine.write(base + 640, {reinterpret_cast<const uint8_t*>(sendVectors.data()), sizeof(sendVectors)}));
    assert(call(211, {first, base + 576, 0x4040}) == 12);
    assert(call(22, {poll, base + 512, 1}) == 1);
    assert(call(207, {second, base + 512, 32, 0x40}) == 12);
    assert(std::memcmp(machine.read(base + 512, 12, 4), "guest packet", 12) == 0);
    assert(call(207, {second, base + 512, 32, 0x40}) == uint64_t(-11));
    assert(call(211, {first, base + 576, 0x4040}) == 12);
    const std::array<uint64_t, 7> receiveHeader{0, 0, base + 736, 2, base + 832, 24, 0};
    std::array<uint64_t, 4> receiveVectors{base + 800, 4, 0, 4};
    assert(machine.write(base + 672, {reinterpret_cast<const uint8_t*>(receiveHeader.data()), sizeof(receiveHeader)}));
    assert(machine.write(base + 736, {reinterpret_cast<const uint8_t*>(receiveVectors.data()), sizeof(receiveVectors)}));
    assert(call(212, {second, base + 672, 0x40}) == uint64_t(-14));
    receiveVectors[2] = base + 808;
    assert(machine.write(base + 736, {reinterpret_cast<const uint8_t*>(receiveVectors.data()), sizeof(receiveVectors)}));
    assert(call(212, {second, base + 672, 0x62}) == 12); // Peek with full datagram length.
    assert(load(base + 712) == 0 && load(base + 720) == 0x20);
    assert(call(212, {second, base + 672, 0x40}) == 8);
    assert(std::memcmp(machine.read(base + 800, 4, 4), "gues", 4) == 0);
    assert(std::memcmp(machine.read(base + 808, 4, 4), "t pa", 4) == 0);
    assert(call(212, {second, base + 672, 0x40}) == uint64_t(-11));
    assert(call(57, {first}) == 0);
    assert(call(22, {poll, base + 512, 1}) == 1 && load(base + 512) == 16);
    assert(call(63, {second, base + 512, 32}) == 0);
    linux.addDirectory("/dev/socket", 0755);
    const auto listener = call(198, {1, 1 | 0x800});
    assert(machine.store64(base + 512, 4));
    assert(call(209, {listener, 1, 39, base + 520, base + 512}) == 0);
    assert((load(base + 520) & 0xffffffff) == 1);
    assert(call(209, {listener, 1, 38, base + 520, base + 512}) == 0);
    assert((load(base + 520) & 0xffffffff) == 0);

    put(base + 770, "/dev/socket/test");
    const std::array<uint8_t, 2> unixFamily{1, 0};
    assert(machine.write(base + 768, unixFamily));
    assert(call(200, {listener, base + 768, 19}) == 0);
    assert(machine.store64(base + 512, 2));
    assert(call(204, {listener, base + 800, base + 512}) == 0 && load(base + 512) == 19);
    assert(std::memcmp(machine.read(base + 800, 2, 4), unixFamily.data(), 2) == 0);
    assert(call(201, {listener, 8}) == 0);
    assert(call(21, {poll, 1, listener, base + 256}) == 0);
    const auto client = call(198, {1, 1});
    assert(call(205, {client, base + 800, base + 512}) == uint64_t(-107));
    assert(call(203, {client, base + 768, 19}) == 0);
    assert(call(205, {client, base + 800, base + 512}) == 0 && load(base + 512) == 19);
    assert(std::memcmp(machine.read(base + 800, 19, 4), machine.read(base + 768, 19, 4), 19) == 0);
    assert(call(22, {poll, base + 512, 2}) == 2); // pending accept and the old socket hangup
    const auto accepted = call(242, {listener, 0, 0, 0x80000});
    assert(accepted < 256);
    assert(call(204, {accepted, base + 800, base + 512}) == 0 && load(base + 512) == 19);
    assert(call(205, {accepted, base + 800, base + 512}) == 0 && load(base + 512) == 2);
    assert(call(206, {client, base + 296, 5}) == 5);
    assert(call(206, {client, base + 301, 7}) == 7);
    assert(call(207, {accepted, base + 512, 12, 0x100}) == 12);
    assert(std::memcmp(machine.read(base + 512, 12, 4), "guest packet", 12) == 0);
    assert(call(206, {accepted, base + 296, 3}) == 3);
    assert(call(207, {client, base + 512, 3, 0x100}) == 3);
    assert(call(206, {client, base + 296, 3}) == 3);
    assert(call(210, {client, 1}) == 0);
    assert(call(206, {client, base + 296, 1}) == uint64_t(-32));
    assert(call(207, {accepted, base + 512, 6, 0x100}) == 3);
    assert(call(63, {accepted, base + 512, 3}) == 0);
    assert(call(206, {accepted, base + 296, 3}) == 3);
    assert(call(63, {client, base + 512, 3}) == 3);


    {
        using namespace std::chrono;
        const auto pendingClient = call(198, {1, 1});
        const auto blockedClient = call(198, {1, 1});
        assert(call(201, {listener, 1}) == 0);
        assert(call(203, {pendingClient, base + 768, 19}) == 0);
        assert(machine.store64(base + 832, 0) && machine.store64(base + 840, 30'000));
        assert(call(208, {blockedClient, 1, 67, base + 832, 16}) == 0);
        assert(call(203, {blockedClient, base + 768, 19}) == blockedClient && linux.blocked());
        assert(linux.schedule(machine, steady_clock::now() + seconds(1)));
        assert(call(203, {blockedClient, base + 768, 19}) == uint64_t(-11) && !linux.blocked());
        assert(call(203, {blockedClient, base + 768, 19}) == blockedClient && linux.blocked());
        const auto firstConnection = call(202, {listener, 0});
        assert(firstConnection < 256 && !linux.blocked());
        assert(call(203, {blockedClient, base + 768, 19}) == 0);
        const auto secondConnection = call(202, {listener, 0});
        assert(secondConnection < 256);
        for (const auto fd : {pendingClient, blockedClient, firstConnection, secondConnection})
            assert(call(57, {fd}) == 0);
    }

    // Test the clone ABI with the same registers Bionic supplies to the kernel.
    assert(call(140, {0, 0, 3}) == 0);
    machine.cpu.pc = base;
    const auto statScratch = call(222, {0, pageSize, 3, 0x22, uint64_t(-1), 0});
    assert(statScratch < (uint64_t{1} << 45));
    const auto childTid = call(220, {0x3d0f00, base + 0x900, base + 0x810, base + 0xa00, base + 0x810});
    assert(childTid == 2 && load(base + 0x810) == childTid);
    const auto statThreads = [&](const char* path, std::string_view prefix) {
        put(statScratch, path);
        const auto fd = call(56, {uint32_t(-100), statScratch, 0});
        assert(fd < 256);
        const auto size = call(63, {fd, statScratch + 64, 500});
        assert(size && size <= 500 && call(57, {fd}) == 0);
        const auto* content = reinterpret_cast<const char*>(machine.read(statScratch + 64, size, 4));
        const std::string line(content, size);
        assert(line.starts_with(prefix));
        const auto commEnd = line.rfind(')');
        assert(commEnd != std::string::npos);
        std::istringstream fields(line.substr(commEnd + 2));
        std::string field;
        for (unsigned number = 3; number <= 20; ++number) assert(fields >> field);
        return std::stoi(field);
    };
    assert(statThreads("/proc/1/task/1/stat", "1 (") == 2);
    assert(statThreads("/proc/self/task/2/stat", "2 (") == 2);
    put(statScratch, "/proc/self/fd");
    const auto fdDirectory = call(56, {uint32_t(-100), statScratch, 0x4000});
    assert(fdDirectory < 256);
    const auto dirBytes = call(61, {fdDirectory, statScratch + 64, 512});
    assert(dirBytes && dirBytes <= 512);
    const auto* records = machine.read(statScratch + 64, dirBytes, 4);
    assert(records);
    bool sawDirectory = false;
    for (uint64_t position = 0; position < dirBytes;) {
        const auto* record = records + position;
        uint16_t size = 0;
        std::memcpy(&size, record + 16, sizeof(size));
        assert(size >= 24 && size <= dirBytes - position);
        if (std::string_view(reinterpret_cast<const char*>(record + 19)) == std::to_string(fdDirectory))
            sawDirectory = record[18] == 10;
        position += size;
    }
    assert(sawDirectory && call(57, {fdDirectory}) == 0);
    machine.cpu.pc += 4;
    assert(linux.schedule(machine));
    assert(linux.threadId() == 2 && machine.cpu.x[0] == 0 && machine.cpu.pc == base + 4);
    assert(machine.cpu.sp == base + 0x900 && machine.cpu.threadPointer == base + 0xa00);
    assert(call(172) == 1 && call(178) == 2);
    assert(call(141, {0, childTid}) == 17); // Inherits nice 3.
    assert(call(140, {0, childTid, 5}) == 0);
    assert(call(141, {0, 0}) == 15 && call(141, {0, 1}) == 17);
    assert(machine.store64(base + 0x800, 5));
    assert(call(98, {base + 0x800, 128, 4}) == uint64_t(-11) && !linux.blocked());
    call(98, {base + 0x800, 128, 5});
    assert(linux.blocked() && linux.schedule(machine) && linux.threadId() == 1);
    assert(call(98, {base + 0x800, 132, 0, 1, base + 0x804, 4}) == uint64_t(-11));
    assert(call(98, {base + 0x800, 132, 0, 1, base + 0x804, 5}) == 1);
    assert(call(98, {base + 0x800, 129, 1}) == 0);
    assert(call(98, {base + 0x804, 129, 1}) == 1);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.threadId() == 2 && machine.cpu.x[0] == 0);
    assert(machine.cpu.pc == base + 8);

    constexpr uint64_t userSignal = 10, signalMask = 1ULL << (userSignal - 1);
    assert(machine.store64(base + 0x820, signalMask));
    assert(machine.store64(base + 0x8c0, 0) && machine.store64(base + 0x8c8, 0));
    assert(call(137, {base + 0x820, 0, base + 0x8c0, 8}) == uint64_t(-11));
    call(137, {base + 0x820, base + 0xe00, 0, 8});
    assert(linux.blocked() && linux.schedule(machine) && linux.threadId() == 1);
    assert(call(131, {2, childTid, userSignal}) == uint64_t(-3));
    assert(call(131, {1, childTid, userSignal}) == 0);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.threadId() == childTid);
    assert(call(137, {base + 0x820, base + 0xe00, 0, 8}) == userSignal);
    assert(load(base + 0xe00) == userSignal && uint32_t(load(base + 0xe08)) == uint32_t(-6));
    call(93);
    assert(linux.blocked() && linux.schedule(machine) && linux.threadId() == 1);
    assert(load(base + 0x810) == 0);
    assert(call(141, {0, childTid}) == uint64_t(-3));
    assert(statThreads("/proc/1/task/1/stat", "1 (") == 1);
    put(statScratch, "/proc/1/task/2/stat");
    assert(call(56, {uint32_t(-100), statScratch, 0}) == uint64_t(-2));
    assert(call(215, {statScratch, pageSize}) == 0);

    linux.addDirectory("/view", 0755);
    linux.addDirectory("/other", 0755);
    put(base + 100, "/view");
    put(base + 120, "/view/sample");
    put(base + 144, "/other");
    put(base + 160, "/other/example");
    put(base + 184, "/proc/self/ns/mnt");
    assert(call(40, {base + 16, base + 100, 0, 0x5000}) == 0);
    const auto viaBind = call(56, {uint32_t(-100), base + 120, 2});
    assert(viaBind < 256 && call(64, {viaBind, base + 296, 12}) == 12);
    const auto original = call(56, {uint32_t(-100), base + 72, 0});
    assert(original < 256 && call(63, {original, base + 512, 12}) == 12);
    assert(std::memcmp(machine.read(base + 512, 12, 4), "guest packet", 12) == 0);
    const auto firstNamespace = call(56, {uint32_t(-100), base + 184, 0});
    assert(firstNamespace < 256);
    assert(call(97, {0x20000}) == 0);
    const auto secondNamespace = call(56, {uint32_t(-100), base + 184, 0});
    assert(secondNamespace < 256 && secondNamespace != firstNamespace);
    assert(call(80, {firstNamespace, base + 512}) == 0 && load(base + 520) == 1);
    assert(call(80, {secondNamespace, base + 512}) == 0 && load(base + 520) == 2);
    assert(call(40, {base, base + 144, base, 14, base + 40}) == 0);
    assert(call(56, {uint32_t(-100), base + 160, 0x42, 0600}) < 256);
    assert(call(268, {firstNamespace, 0x20000}) == 0);
    assert(call(56, {uint32_t(-100), base + 160, 0}) == uint64_t(-2));
    assert(call(268, {secondNamespace, 0x20000}) == 0);
    assert(call(56, {uint32_t(-100), base + 160, 0}) < 256);

    assert(machine.store64(base + 0x840, 17));
    assert(machine.store64(base + 0x850, 1ULL << 16));
    assert(call(135, {0, base + 0x850, 0, 8}) == 0);
    const auto signal = call(74, {UINT32_MAX, base + 0x850, 8, 0x800});
    assert(signal < 256);
    const auto childPoll = call(20);
    assert(machine.store64(base + 256, 1));
    assert(call(21, {childPoll, 1, signal, base + 256}) == 0);
    assert(call(63, {signal, base + 0x900, 128}) == uint64_t(-11));
    const auto sharedFutex = call(222, {0, pageSize, 3, 1, viaBind, 0});
    assert(sharedFutex > 0x100000000 && sharedFutex < 0x2000000000);
    assert(machine.store64(sharedFutex, 55));
    constexpr uint64_t lockAddress = base + 0xa40;
    const auto prepareLock = [&](uint16_t type, int64_t start, int64_t length) {
        assert(machine.store64(lockAddress, type));
        assert(machine.store64(lockAddress + 8, uint64_t(start)));
        assert(machine.store64(lockAddress + 16, uint64_t(length)));
        assert(machine.store64(lockAddress + 24, 0));
    };
    prepareLock(1, 10, 20);
    assert(call(25, {viaBind, 6, lockAddress}) == 0);
    prepareLock(2, 15, 5);
    assert(call(25, {viaBind, 6, lockAddress}) == 0);
    constexpr uint64_t heapSize = 96 * 1024 * 1024;
    const auto heap = call(222, {0, heapSize, 3, 0x22, uint64_t(-1), 0});
    const auto heapTail = heap + heapSize - pageSize;
    assert(heap < (uint64_t{1} << 45) && load(heap) == 0 && load(heapTail) == 0);
    assert(machine.store64(heap, 31) && machine.store64(heapTail, 79));
    assert(machine.store64(heap + pageSize, 44) && machine.store64(heap + 2 * pageSize, 55));
    assert(call(233, {heap + pageSize, pageSize, 10}) == 0);
    assert(call(233, {heap + 2 * pageSize, pageSize, 18}) == 0);
    assert(call(226, {heapTail, pageSize, 0}) == 0);
    assert(!machine.read(heapTail, 8, 4));
    const auto child = call(220, {0x1200011, 0, 0, 0, base + 0x848});
    assert(child == 3 && load(base + 0x848) == 0);
    machine.cpu.pc += 4;
    assert(linux.schedule(machine) && linux.processId() == child);
    assert(load(base + 0x848) == child && call(173) == 1);
    assert(load(heap) == 31 && machine.store64(heap, 55));
    assert(!machine.read(heap + pageSize, 8, 4) && load(heap + 2 * pageSize) == 0);
    assert(call(226, {heapTail, pageSize, 3}) == 0 && load(heapTail) == 79);
    assert(call(233, {heapTail, pageSize, 4}) == 0 && load(heapTail) == 0);
    prepareLock(1, 8, 4);
    assert(call(25, {viaBind, 6, lockAddress}) == uint64_t(-11));
    assert(call(25, {viaBind, 5, lockAddress}) == 0);
    assert(load(lockAddress) == 1 && load(lockAddress + 8) == 10);
    assert(load(lockAddress + 16) == 5 && load(lockAddress + 24) == 1);
    prepareLock(1, 20, -5);
    assert(call(25, {viaBind, 6, lockAddress}) == 0);
    const auto childFutex = call(222, {0, pageSize, 3, 1, viaBind, 0});
    assert(childFutex != sharedFutex && load(childFutex) == 55);
    call(98, {childFutex, 0, 55});
    assert(linux.blocked() && linux.schedule(machine) && linux.processId() == 1);
    const auto aliasForLock = call(23, {viaBind});
    assert(aliasForLock < 256 && call(57, {aliasForLock}) == 0);
    assert(call(98, {sharedFutex, 0x81, 1}) == 0);
    assert(call(98, {sharedFutex, 1, 1}) == 1);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.processId() == child && machine.cpu.x[0] == 0);
    prepareLock(1, 8, 4);
    assert(call(25, {viaBind, 6, lockAddress}) == 0);

    assert(machine.store64(base + 0x840, 99));
    assert(call(57, {original}) == 0);
    call(94, {23});
    assert(linux.blocked() && linux.schedule(machine) && linux.processId() == 1);
    assert(load(base + 0x840) == 17 && load(base + 0x848) == 0);
    assert(load(heap) == 31 && !machine.read(heapTail, 8, 4));
    assert(load(heap + pageSize) == 44 && load(heap + 2 * pageSize) == 55);
    assert(call(233, {heap + pageSize, pageSize, 11}) == 0);
    assert(call(233, {heap + 2 * pageSize, pageSize, 19}) == 0);
    assert(call(226, {heapTail, pageSize, 3}) == 0 && load(heapTail) == 79);
    const auto heapMoved = call(216, {heap, heapSize, heapSize + pageSize, 1});
    assert(heapMoved < (uint64_t{1} << 45) && load(heapMoved) == 31);
    assert(load(heapMoved + heapSize - pageSize) == 79 && load(heapMoved + heapSize) == 0);
    assert(call(215, {heapMoved, heapSize + pageSize}) == 0 && !machine.read(heapMoved, 8, 4));
    assert(call(80, {original, base + 512}) == 0);
    assert(call(22, {childPoll, base + 512, 1, 0}) == 1 && load(base + 512) == 1);
    assert(call(63, {signal, base + 0x900, 128}) == 128);
    assert(load(base + 0x900) == 17);
    assert(load(base + 0x908) == (child << 32 | 1));
    assert(load(base + 0x928) == 23);
    assert(call(63, {signal, base + 0x900, 128}) == uint64_t(-11));
    assert(call(95, {1, child, base + 512, 0x1000005}) == 0);
    assert(load(base + 528) == child && load(base + 536) == 23);
    assert(call(260, {child, 0xdead, 1}) == uint64_t(-14));
    assert(call(260, {child, base + 512, 1}) == child && load(base + 512) == 23 * 256);
    assert(call(95, {1, child, base + 512, 5}) == uint64_t(-10));
    assert(call(129, {child, 9}) == uint64_t(-3));
    assert(call(129, {1, 0}) == 0);

    // An early-init action mounts a subdirectory over its parent mount.
    put(base + 100, "/test/sub");
    put(base + 120, "/test/sub/marker");
    put(base + 144, "/test/marker");
    assert(call(34, {uint32_t(-100), base + 100, 0755}) == 0);
    assert(call(56, {uint32_t(-100), base + 120, 0x42, 0600}) < 256);
    assert(call(40, {base + 100, base + 16, 0, 0x5000}) == 0);
    assert(call(56, {uint32_t(-100), base + 144, 0}) < 256);
    assert(call(56, {uint32_t(-100), base + 72, 0}) == uint64_t(-2));

    assert(call(59, {0xdead, 0}) == uint64_t(-14));
    assert(call(59, {base + 512, 0x80800}) == 0);
    const auto pipe = load(base + 512);
    const auto receiver = uint32_t(pipe), sender = uint32_t(pipe >> 32);
    const auto pipePoller = call(20, {0x80000});
    constexpr uint64_t pipeEvent = base + 0x740;
    assert(machine.store64(pipeEvent, 1) && machine.store64(pipeEvent + 8, 0x54));
    assert(pipePoller < 256 && call(21, {pipePoller, 1, receiver, pipeEvent}) == 0);
    assert(call(22, {pipePoller, pipeEvent, 1, 0}) == 0);
    assert(call(63, {receiver, base + 512, 12}) == uint64_t(-11));
    assert(call(64, {sender, base + 296, 12}) == 12);
    const auto duplicate = call(23, {sender});
    assert(duplicate < 256 && call(57, {sender}) == 0);
    assert(call(22, {pipePoller, pipeEvent, 1, 0}) == 1 && load(pipeEvent) == 1 && load(pipeEvent + 8) == 0x54);
    assert(call(63, {receiver, base + 512, 12}) == 12);
    assert(std::memcmp(machine.read(base + 512, 12, 4), "guest packet", 12) == 0);
    assert(call(63, {receiver, base + 512, 12}) == uint64_t(-11));
    assert(call(22, {pipePoller, pipeEvent, 1, 0}) == 0);
    assert(call(57, {duplicate}) == 0);
    assert(call(22, {pipePoller, pipeEvent, 1, 0}) == 1 && load(pipeEvent) == 16);
    assert(call(63, {receiver, base + 512, 12}) == 0);
    assert(call(57, {receiver}) == 0 && call(57, {pipePoller}) == 0);

    put(base + 120, "/system/bin/absent");
    const auto beforeExec = machine.cpu.pc;
    assert(call(221, {base + 120, 0, 0}) == uint64_t(-2));
    assert(!linux.replacedImage() && machine.cpu.pc == beforeExec);
    assert(call(166, {0077}) == 0022 && call(166, {0022}) == 0077);
    assert(machine.store64(base + 512, 1001));
    assert(call(159, {1, base + 512}) == 0 && call(158, {0}) == 1);
    assert(call(158, {1, base + 512}) == 1 && load(base + 512) == 1001);
    assert(call(159, {0, 0}) == 0 && call(158, {0}) == 0);

    put(base + 180, "/test/empty");
    assert(call(35, {uint32_t(-100), base + 180, 0x200}) == uint64_t(-2));
    assert(call(34, {uint32_t(-100), base + 180, 0755}) == 0);
    assert(call(35, {uint32_t(-100), base + 180, 0x200}) == 0);
    assert(call(35, {uint32_t(-100), base + 16, 0x200}) == uint64_t(-16));
    assert(call(35, {uint32_t(-100), base + 144, 0x200}) == uint64_t(-20));

    const auto moveSource = call(222, {0, pageSize, 3, 0x22, uint64_t(-1), 0});
    const auto moveTarget = call(222, {0, pageSize, 3, 0x22, uint64_t(-1), 0});
    assert(machine.store64(moveSource, 0xdecaf));
    assert(call(216, {moveSource, pageSize, pageSize, 3, moveSource}) == uint64_t(-22));
    assert(call(216, {moveSource, pageSize, pageSize, 3, moveTarget}) == moveTarget);
    assert(load(moveTarget) == 0xdecaf && !machine.read(moveSource, 8, 4));

    constexpr uint64_t low = 0x2000000;
    assert(call(222, {low, pageSize, 3, 0x100022, uint64_t(-1), 0}) == low);
    assert(call(222, {low, pageSize, 0, 0x100022, uint64_t(-1), 0}) == uint64_t(-17));
    assert(machine.store64(low, 42));
    const auto grown = call(216, {low, pageSize, pageSize * 2, 1});
    assert(grown == low && load(grown) == 42 && load(grown + pageSize) == 0);
    assert(call(215, {low, pageSize * 2}) == 0 && !machine.read(low, 8, 4));
    assert(call(96, {0}) == 1);
    assert(call(283, {8, 0}) == uint64_t(-1));
    assert(call(283, {16, 0}) == 0 && call(283, {8, 0}) == 0);

    const auto wakeTimer = call(85, {9, 0x80800});
    assert(wakeTimer < 256);
    constexpr auto caps = base + 600;
    assert(machine.store64(base + 512, 0x20080522));
    assert(call(90, {base + 512, caps}) == 0 && load(caps) == UINT64_MAX);
    assert(machine.store64(caps, 0x100'0000'0100ULL));
    assert(machine.store64(caps + 8, 0x100));
    assert(machine.store64(caps + 16, 0));
    assert(call(91, {base + 512, caps}) == 0);
    const std::array<uint64_t, 4> disarmTimer{};
    assert(machine.write(base + 768, {reinterpret_cast<const uint8_t*>(disarmTimer.data()), sizeof(disarmTimer)}));
    assert(call(86, {wakeTimer, 0, base + 768}) == uint64_t(-1));
    assert(call(57, {wakeTimer}) == 0);
    assert(call(167, {47, 2, 8}) == 0 && call(167, {47, 1, 8}) == 1);
    assert(machine.store64(caps, 0x300'0000'0100ULL));
    assert(call(91, {base + 512, caps}) == uint64_t(-1));
    assert(call(90, {base + 512, caps}) == 0 && load(caps) == 0x100'0000'0100ULL);

    const std::array<uint8_t, 1> apexExecutable{0};
    linux.mount("/system/bin/apexd", apexExecutable);
    linux.setBootContext("u:r:init:s0");
    const auto attr = "/proc/" + std::to_string(call(172, {})) + "/attr/current";
    put(base, attr.c_str());
    const auto labelFd = call(56, {uint32_t(-100), base, 0});
    assert(call(63, {labelFd, base + 512, 128}) == sizeof("u:r:init:s0"));
    assert(std::memcmp(machine.read(base + 512, sizeof("u:r:init:s0"), 4),
                       "u:r:init:s0", sizeof("u:r:init:s0")) == 0);
    assert(call(57, {labelFd}) == 0);
    put(base, "/proc/4294967295/attr/current");
    assert(call(56, {uint32_t(-100), base, 0}) == uint64_t(-2));
    put(base, "servicemanager-worker");
    assert(call(167, {15, base}) == 0 && call(167, {16, base + 512}) == 0);
    assert(std::memcmp(machine.read(base + 512, 16, 4), "servicemanager-", 16) == 0);
    assert(linux.loadBootTransition("/system/bin/apexd u:r:init:s0 u:object_r:apexd_exec:s0 2 u:r:apexd:s0"));
    assert(linux.loadPropertyPolicy(
        "100 1\nu:r:init:s0 u:object_r:build_prop:s0 1 ffffffff 0 ffffffff 1 0\n"
        "u:r:apexd:s0 u:object_r:build_prop:s0 0 ffffffff 0 ffffffff 1 0\n"));
    put(base, "/sys/fs/selinux/create");
    const auto create = call(56, {uint32_t(-100), base, 2});
    const char transition[] = "u:r:init:s0 u:object_r:apexd_exec:s0 2";
    put(base + 300, transition);
    assert(call(64, {create, base + 300, sizeof(transition)}) == sizeof(transition));
    assert(call(63, {create, base + 512, 128}) == sizeof("u:r:apexd:s0"));
    assert(std::memcmp(machine.read(base + 512, sizeof("u:r:apexd:s0"), 4),
                       "u:r:apexd:s0", sizeof("u:r:apexd:s0")) == 0);
    assert(call(57, {create}) == 0);
    put(base, "/sys/fs/selinux/access");
    const auto access = call(56, {uint32_t(-100), base, 2});
    const char denial[] = "u:r:apexd:s0 u:object_r:build_prop:s0 100 1";
    put(base + 300, denial);
    assert(call(64, {access, base + 300, sizeof(denial) - 1}) == sizeof(denial) - 1);
    assert(call(63, {access, base + 512, 128}) == 25);
    assert(std::memcmp(machine.read(base + 512, 25, 4), "0 ffffffff 0 ffffffff 1 0", 25) == 0);
    const char unknown[] = "u:r:unknown:s0 u:object_r:build_prop:s0 100 1";
    put(base + 300, unknown);
    assert(call(64, {access, base + 300, sizeof(unknown) - 1}) == uint64_t(-95));

    assert(linux.loadServicePolicy("101 1 2 3\nu:r:servicemanager:s0 "
                                   "u:object_r:service_manager_service:s0 3 ffffffff 0 ffffffff 1 0\n", false));
    const char permitted[] = "u:r:servicemanager:s0 u:object_r:service_manager_service:s0 101 1";
    put(base + 300, permitted);
    assert(call(64, {access, base + 300, sizeof(permitted) - 1}) == sizeof(permitted) - 1);
    assert(call(63, {access, base + 512, 128}) == 25);
    assert(std::memcmp(machine.read(base + 512, 25, 4), "3 ffffffff 0 ffffffff 1 0", 25) == 0);

    assert(linux.loadKeystorePolicy(
        "104 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17\nu:r:vold:s0 u:r:keystore:s0 60 ffffffff 0 ffffffff 1 0\n"
        "u:r:shell:s0 u:r:keystore:s0 0 ffffffff 0 ffffffff 1 0\n"));
    put(base, "/sys/fs/selinux/class/keystore2/index");
    const auto keyClass = call(56, {uint32_t(-100), base, 0});
    assert(keyClass < 256 && call(63, {keyClass, base + 512, 8}) == 3);
    assert(std::memcmp(machine.read(base + 512, 3, 4), "104", 3) == 0);
    assert(call(57, {keyClass}) == 0);
    const char maintenance[] = "u:r:vold:s0 u:r:keystore:s0 104 40";
    put(base + 300, maintenance);
    assert(call(64, {access, base + 300, sizeof(maintenance) - 1}) == sizeof(maintenance) - 1);
    assert(call(63, {access, base + 512, 128}) == 26);
    assert(std::memcmp(machine.read(base + 512, 26, 4), "60 ffffffff 0 ffffffff 1 0", 26) == 0);
    const char shellMaintenance[] = "u:r:shell:s0 u:r:keystore:s0 104 40";
    put(base + 300, shellMaintenance);
    assert(call(64, {access, base + 300, sizeof(shellMaintenance) - 1}) == sizeof(shellMaintenance) - 1);
    assert(call(63, {access, base + 512, 128}) == 25);
    assert(std::memcmp(machine.read(base + 512, 25, 4), "0 ffffffff 0 ffffffff 1 0", 25) == 0);
    const char unrelated[] = "u:r:vold:s0 u:r:keystore:s0 104 20000";
    put(base + 300, unrelated);
    assert(call(64, {access, base + 300, sizeof(unrelated) - 1}) == uint64_t(-95));

    assert(linux.loadKeystorePolicy(
        "105 1 2 3 4 5 6 7 8 9 10 11\nu:r:odsign:s0 u:object_r:odsign_key:s0 24a ffffffff 0 ffffffff 1 0\n"
        "u:r:shell:s0 u:object_r:odsign_key:s0 0 ffffffff 0 ffffffff 1 0\n", true));
    const char signingKey[] = "u:r:odsign:s0 u:object_r:odsign_key:s0 105 40";
    put(base + 300, signingKey);
    assert(call(64, {access, base + 300, sizeof(signingKey) - 1}) == sizeof(signingKey) - 1);
    assert(call(63, {access, base + 512, 128}) == 27);
    assert(std::memcmp(machine.read(base + 512, 27, 4), "24a ffffffff 0 ffffffff 1 0", 27) == 0);
    const char otherKey[] = "u:r:shell:s0 u:object_r:odsign_key:s0 105 40";
    put(base + 300, otherKey);
    assert(call(64, {access, base + 300, sizeof(otherKey) - 1}) == sizeof(otherKey) - 1);
    assert(call(63, {access, base + 512, 128}) == 25);
    assert(std::memcmp(machine.read(base + 512, 25, 4), "0 ffffffff 0 ffffffff 1 0", 25) == 0);

    constexpr uint64_t loopConfig = base + 0xb00;
    std::array<uint8_t, 8192> package{};
    package[4096] = 0x53;
    package[4097] = 0xef;
    linux.mount("/system/apex/test.apex", package);
    put(base + 200, "/dev/loop-control");
    put(base + 240, "/dev/block/loop0");
    put(base + 280, "/system/apex/test.apex");
    const auto control = call(56, {uint32_t(-100), base + 200, 2});
    constexpr uint64_t getUserQuota = 0x80000700;
    assert(call(60, {getUserQuota, base + 240, 0, base + 512}) == uint64_t(-2));
    assert(call(60, {getUserQuota, 0xdead, 0, base + 512}) == uint64_t(-14));
    assert(call(60, {getUserQuota | 3, base + 240, 0, base + 512}) == uint64_t(-22));
    assert(call(29, {control, 0x4c82}) == 0);
    assert(call(60, {getUserQuota, base + 240, 0, base + 512}) == uint64_t(-19));
    const auto block = call(56, {uint32_t(-100), base + 240, 2});
    const auto backing = call(56, {uint32_t(-100), base + 280, 0x10000});
    assert(control < 256 && block < 256 && backing < 256);
    assert(machine.store64(loopConfig, (uint64_t{4096} << 32) | backing));
    assert(machine.store64(loopConfig + 32, 4096)); // lo_offset
    assert(machine.store64(loopConfig + 40, 8192)); // size would exceed backing file
    assert(call(29, {block, 0x4c0a, loopConfig}) == uint64_t(-22));
    assert(machine.store64(loopConfig + 40, 4096));
    assert(call(29, {block, 0x4c0a, loopConfig}) == 0);
    put(base + 400, "/sys/block/loop0/loop/backing_file");
    const auto backingName = call(56, {uint32_t(-100), base + 400, 0});
    assert(backingName < 256);

    assert(call(57, {backing}) == 0);
    assert(call(29, {block, 0x80081272, base + 512}) == 0 && load(base + 512) == 4096);
    assert(call(29, {block, 0x1262, 256}) == 0);
    assert(call(67, {block, base + 512, 2, 0}) == 2 && (load(base + 512) & 0xffff) == 0xef53);
    assert(call(67, {block, base + 512, 2, 4096}) == 0);
    put(base + 340, "/test/invalid-apex");
    put(base + 370, "ext4");
    assert(call(34, {uint32_t(-100), base + 340, 0755}) == 0);
    assert(call(40, {base + 240, base + 340, base + 370, 1, 0}) == uint64_t(-22));
    assert(call(29, {block, 0x4c01}) == 0);
    assert(call(56, {uint32_t(-100), base + 400, 0}) == uint64_t(-2));
    constexpr std::string_view expectedBacking = "/system/apex/test.apex\n";
    assert(call(63, {backingName, base + 512, 128}) == expectedBacking.size());
    assert(std::memcmp(machine.read(base + 512, expectedBacking.size(), 4),
                       expectedBacking.data(), expectedBacking.size()) == 0);
    assert(call(57, {backingName}) == 0);

    assert(call(67, {block, base + 512, 2, 0}) == uint64_t(-6));

    put(base + 200, "/dev/ptmx");
    put(base + 230, "/dev/pts/0");
    const auto master = call(56, {uint32_t(-100), base + 200, 0x80802});
    assert(master < 256);
    assert(call(29, {master, 0x80045430, base + 512}) == 0);
    assert(call(56, {uint32_t(-100), base + 230, 2}) == uint64_t(-5));
    assert(machine.store64(base + 512, 0));
    assert(call(29, {master, 0x40045431, base + 512}) == 0);
    const auto slave = call(56, {uint32_t(-100), base + 230, 2});
    assert(slave < 256 && call(63, {master, base + 512, 20}) == uint64_t(-11));
    put(base + 260, "line\n");
    assert(call(64, {slave, base + 260, 5}) == 5);
    assert(machine.store64(base + 512, (uint64_t{1} << 32) | master));
    assert(machine.store64(base + 600, 0) && machine.store64(base + 608, 0));
    assert(call(73, {base + 512, 1, base + 600}) == 1 && load(base + 512) >> 48 == 1);
    assert(machine.store64(base + 600, 2'147'483));
    assert(call(73, {base + 512, 1, base + 600}) == 1 && load(base + 512) >> 48 == 1);
    assert(machine.store64(base + 600, 0));
    assert(call(63, {master, base + 512, 20}) == 6);
    assert(std::memcmp(machine.read(base + 512, 6, 4), "line\r\n", 6) == 0);
    assert(call(57, {slave}) == 0);
    assert(machine.store64(base + 512, (uint64_t{1} << 32) | master));
    assert(call(73, {base + 512, 1, base + 600}) == 1 && load(base + 512) >> 48 == 16);
    assert(call(63, {master, base + 512, 20}) == uint64_t(-5));

    put(base + 200, "/test/owner");
    assert(call(34, {uint32_t(-100), base + 200, 0755}) == 0);
    assert(call(54, {uint32_t(-100), base + 200, 1000, 1001, 0x100}) == 0);
    assert(call(53, {uint32_t(-100), base + 200, 0710}) == 0);
    assert(call(79, {uint32_t(-100), base + 200, base + 512, 0}) == 0);
    assert(load(base + 528) == (uint64_t{1} << 32 | 0040710));
    assert(load(base + 536) == (uint64_t{1001} << 32 | 1000));

    const auto uevent = call(198, {16, 2 | 0x80800, 15});
    assert(uevent < 256);
    assert(machine.store64(base + 512, 4));
    assert(call(209, {uevent, 1, 39, base + 520, base + 512}) == 0);
    assert((load(base + 520) & 0xffffffff) == 16);
    assert(call(209, {uevent, 1, 38, base + 520, base + 512}) == 0);
    assert((load(base + 520) & 0xffffffff) == 15);

    assert(machine.store64(base + 0xc00, 16));
    assert(machine.store64(base + 0xc08, 1));
    assert(call(200, {uevent, base + 0xc00, 12}) == 0);
    assert(call(208, {uevent, 1, 16, base + 0xc08, 4}) == 0);
    assert(call(212, {uevent, base + 0xc80, 0}) == uint64_t(-11));
    assert(call(29, {control, 0x4c80, 1}) == 1);
    assert(machine.store64(base + 0xc60, base + 0xd00));
    assert(machine.store64(base + 0xc68, 256));
    const std::array<uint64_t, 7> message{base + 0xc10, 12, base + 0xc60, 1, base + 0xc30, 32, 0};
    assert(machine.write(base + 0xc80, {reinterpret_cast<const uint8_t*>(message.data()), sizeof(message)}));
    const auto received = call(212, {uevent, base + 0xc80, 0});
    assert(received > 100 && received < 256);
    assert(load(base + 0xc10) == 16 && (load(base + 0xc18) & 0xffffffff) == 1);
    assert(load(base + 0xc30) == 28 && load(base + 0xc38) == (uint64_t{2} << 32 | 1));
    assert(load(base + 0xc40) == 0 && load(base + 0xc48) == 0);
    const std::string_view packet(reinterpret_cast<const char*>(machine.read(base + 0xd00, received, 4)), received);
    assert(packet.starts_with("add@/devices/virtual/block/loop1"));
    assert(packet.find("DEVNAME=loop1") != std::string_view::npos);
    assert(call(212, {uevent, base + 0xc80, 0}) == uint64_t(-11));

    put(base + 200, "/proc/sys/vm/mmap_rnd_bits");
    const auto bits = call(56, {uint32_t(-100), base + 200, 2});
    assert(bits < 256);
    put(base + 260, "33\n");
    assert(call(64, {bits, base + 260, 3}) == uint64_t(-22));
    put(base + 260, "24\n");
    assert(call(64, {bits, base + 260, 3}) == 3);
    assert(call(62, {bits, 0, 0}) == 0);
    assert(call(63, {bits, base + 512, 16}) == 3);
    assert(std::memcmp(machine.read(base + 512, 3, 4), "24\n", 3) == 0);

    assert(call(167, {27}) == 0);
    assert(call(167, {28, 48}) == 0);
    assert(call(167, {27}) == 48 && call(167, {7}) == 1);
    assert(call(167, {8, 0}) == uint64_t(-1));
    assert(call(167, {28, 0}) == uint64_t(-1));
    assert(call(167, {27}) == 48);

    put(base + 200, "/dev/binder-abi");
    put(base + 230, "binder");
    put(base + 260, "/dev/binder-abi/binder");
    assert(call(34, {uint32_t(-100), base + 200, 0755}) == 0);
    assert(call(40, {base + 230, base + 200, base + 230, 0, 0}) == 0);
    assert(call(43, {base + 260, base + 512}) == 0 && load(base + 512) == 0x6c6f6f70);
    const auto manager = call(56, {uint32_t(-100), base + 260, 2});
    assert(manager < 256);
    assert(call(29, {manager, 0xc0046209, base + 512}) == 0 && uint32_t(load(base + 512)) == 8);
    assert(call(29, {manager, 0x40046207}) == uint64_t(-1));
    linux.setBootContext("u:r:servicemanager:s0");
    assert(call(29, {manager, 0x40046207}) == 0);
    assert(call(29, {manager, 0x40046207}) == uint64_t(-16));
    const auto managerMap = call(222, {0, pageSize, 1, 0x4002, manager, 0});
    assert(managerMap >= 0x100000000 && managerMap < 0x2000000000);
    assert(!machine.store64(managerMap, 0));
    assert(call(226, {managerMap, pageSize, 3}) == uint64_t(-13));

    const auto bindRequest = [&](uint64_t channel, uint32_t operation, uint64_t tag,
                                 std::string_view message, bool receive,
                                 std::span<const uint64_t> offsets = {}, uint64_t handle = 0,
                                 uint64_t extraSize = 0) {
        constexpr auto command = base + 0x600, arguments = base + 0x690, output = base + 0x720;
        constexpr auto payload = base + 0x800, offsetList = base + 0x850;
        const std::array<uint64_t, 9> callData{handle, 0, tag, UINT64_MAX,
                                               message.size(), offsets.size_bytes(), payload, offsetList, extraSize};
        assert(machine.write(command, {reinterpret_cast<const uint8_t*>(&operation), 4}));
        const auto size = (operation >> 16) & 0x3fff;
        if (size) assert(machine.write(command + 4, {reinterpret_cast<const uint8_t*>(callData.data()), size}));
        if (!message.empty())
            assert(machine.write(payload, {reinterpret_cast<const uint8_t*>(message.data()), message.size()}));
        if (!offsets.empty())
            assert(machine.write(offsetList, {reinterpret_cast<const uint8_t*>(offsets.data()), offsets.size_bytes()}));
        const std::array<uint64_t, 6> request{operation ? size + 4 : 0, 0, command,
                                              receive ? 128U : 0U, 0, output};
        assert(machine.write(arguments, {reinterpret_cast<const uint8_t*>(request.data()), sizeof request}));
        return call(29, {channel, 0xc0306201, arguments});
    };
    machine.cpu.pc = base;
    const auto binderChild = call(220, {0x1200011, 0, 0, 0, base + 0x848});
    machine.cpu.pc += 4;
    assert(linux.schedule(machine) && linux.processId() == binderChild);
    linux.setBootContext("u:r:vold:s0");
    assert(call(29, {manager, 0xc0046209, base + 512}) == uint64_t(-1));
    const auto channel = call(56, {uint32_t(-100), base + 260, 2});
    const auto clientMap = call(222, {0, pageSize, 1, 0x4002, channel, 0});
    assert(channel < 256 && clientMap != managerMap);
    assert(bindRequest(channel, 0x40406300, 42, "request", true) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x7206);
    assert(bindRequest(channel, 0, 0, "", true) == channel && linux.blocked());
    assert(linux.schedule(machine) && linux.processId() == 1);
    assert(bindRequest(manager, 0x630c, 0, "", true) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x80407202);
    const auto transaction = machine.read(base + 0x724, 64, 4);
    assert(transaction);
    std::array<uint64_t, 8> fields{};
    std::memcpy(fields.data(), transaction, sizeof fields);
    assert(fields[2] == 42 && fields[3] == binderChild && fields[4] == 7 && fields[6] == managerMap);
    assert(std::memcmp(machine.read(managerMap, 7, 4), "request", 7) == 0);
    assert(bindRequest(manager, 0x40406301, 0, "reply", true) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x7206);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.processId() == binderChild);
    assert(bindRequest(channel, 0, 0, "", true) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x80407203);
    assert(std::memcmp(machine.read(clientMap, 5, 4), "reply", 5) == 0);
    {
        constexpr uint32_t transactionSg = 0x40486311, replySg = 0x40486312;
        constexpr uint64_t bufferType = 0x70742a85, childFlag = uint64_t{1} << 32;
        constexpr uint64_t parentSource = base + 0xa00, childSource = base + 0xa10;
        constexpr std::array<uint64_t, 2> offsets{0, 40};
        std::array<uint64_t, 10> buffers{bufferType, parentSource, 16, 0, 0,
                                        bufferType | childFlag, childSource, 8, 0, 8};
        const std::string_view bytes(reinterpret_cast<const char*>(buffers.data()), sizeof buffers);
        assert(machine.store64(parentSource, 0xabc) && machine.store64(parentSource + 8, childSource));
        assert(machine.store64(childSource, 0x12345678));
        assert(bindRequest(channel, transactionSg, 45, bytes, true, offsets, 0, 16) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x7211);
        buffers[9] = 12;
        assert(bindRequest(channel, transactionSg, 45, bytes, true, offsets, 0, 24) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x7211);
        buffers[9] = 8;
        assert(bindRequest(channel, transactionSg, 45, bytes, true, offsets, 0, 24) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x7206);
        assert(bindRequest(channel, 0, 0, "", true) == channel && linux.blocked());
        assert(linux.schedule(machine) && linux.processId() == 1);
        assert(bindRequest(manager, 0, 0, "", true) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x80407202);
        const auto packet = load(base + 0x724 + 6 * 8);
        const auto parent = load(packet + 8), child = load(packet + 48);
        assert(parent == packet + sizeof(buffers) + sizeof(offsets) && child == parent + 16);
        assert(parent != parentSource && child != childSource);
        assert(load(parent) == 0xabc && load(parent + 8) == child && load(child) == 0x12345678);
        assert(bindRequest(manager, replySg, 0, "sg reply", true) == 0);
        assert(uint32_t(load(base + 0x720)) == 0x7206);
        assert(bindRequest(manager, 0x40086303, 0, "", false, {}, packet) == 0);
        machine.cpu.retired += 4096;
        assert(linux.schedule(machine) && linux.processId() == binderChild);
        assert(bindRequest(channel, 0, 0, "", true) == 0 && uint32_t(load(base + 0x720)) == 0x80407203);
        const auto response = load(base + 0x724 + 6 * 8);
        assert(std::memcmp(machine.read(response, 8, 4), "sg reply", 8) == 0);
        assert(load(parentSource + 8) == childSource);
    }
    constexpr uint32_t flatBinder = 0x73622a85, flatHandle = 0x73682a85;
    constexpr uint64_t objectPtr = base + 0x990, objectCookie = base + 0x9b0;
    constexpr std::array<uint64_t, 3> object{flatBinder, objectPtr, objectCookie};
    constexpr std::array<uint64_t, 1> objectOffset{0}, invalidOffset{4};
    const std::string_view objectBytes(reinterpret_cast<const char*>(object.data()), sizeof object);
    assert(bindRequest(channel, 0x40406300, 43, objectBytes, true, invalidOffset) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x7211);
    assert(bindRequest(channel, 0x40406300, 44, objectBytes, true, objectOffset) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x80107207 && load(base + 0x724) == objectPtr);
    assert(bindRequest(channel, 0, 0, "", true) == 0 && uint32_t(load(base + 0x720)) == 0x80107208);
    assert(bindRequest(channel, 0, 0, "", true) == 0 && uint32_t(load(base + 0x720)) == 0x7206);
    assert(bindRequest(channel, 0, 0, "", true) == channel && linux.blocked());
    assert(linux.schedule(machine) && linux.processId() == 1);
    assert(bindRequest(manager, 0, 0, "", true) == 0 && uint32_t(load(base + 0x720)) == 0x80407202);
    const auto receivedBinderBuffer = load(base + 0x724 + 6 * 8);
    const auto handle = load(receivedBinderBuffer + 8);
    assert(uint32_t(load(receivedBinderBuffer)) == flatHandle && handle && load(receivedBinderBuffer + 16) == 0);
    assert(load(load(base + 0x724 + 7 * 8)) == 0);
    assert(bindRequest(manager, 0x40046305, 0, "", false, {}, handle) == 0);
    // binder_handle_cookie is packed: the cookie follows a 32-bit handle.
    assert(bindRequest(manager, 0x400c630e, 0, "", false, {}, handle) == 0);
    const std::array<uint64_t, 3> proxy{flatHandle, handle, 0};
    const std::string_view proxyBytes(reinterpret_cast<const char*>(proxy.data()), sizeof proxy);
    assert(bindRequest(manager, 0x40406301, 0, proxyBytes, true, objectOffset) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x7206);
    assert(bindRequest(manager, 0x40086303, 0, "", false, {}, receivedBinderBuffer) == 0);
    assert(bindRequest(manager, 0x40046306, 0, "", false, {}, handle) == 0);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.processId() == binderChild);
    assert(bindRequest(channel, 0, 0, "", true) == 0 && uint32_t(load(base + 0x720)) == 0x80407203);
    const auto roundTrip = load(base + 0x724 + 6 * 8);
    assert(std::memcmp(machine.read(roundTrip, sizeof object, 4), object.data(), sizeof object) == 0);
    {
        constexpr uint64_t fixedFd = 100;
        constexpr uint64_t fileType = 0x66642a85, acceptFiles = uint64_t{0x10} << 32;
        const std::array<uint64_t, 3> file{fileType, fixedFd, 1};
        const std::string_view fileBytes(reinterpret_cast<const char*>(file.data()), sizeof file);
        assert(call(24, {channel, fixedFd, 0}) == fixedFd);
        for (const bool accepts : {false, true}) {
            const auto tag = uint64_t{46} | (accepts ? acceptFiles : 0);
            assert(bindRequest(channel, 0x40406300, tag, "request a file", true) == 0);
            assert(uint32_t(load(base + 0x720)) == 0x7206);
            assert(bindRequest(channel, 0, 0, "", true) == channel && linux.blocked());
            assert(linux.schedule(machine) && linux.processId() == 1);
            assert(bindRequest(manager, 0, 0, "", true) == 0);
            assert(uint32_t(load(base + 0x720)) == 0x80407202);
            const auto requestBuffer = load(base + 0x724 + 6 * 8);
            const auto origin = call(19, {29, 0}); // eventfd2
            assert(origin < fixedFd && call(24, {origin, fixedFd, 0}) == fixedFd);
            assert(bindRequest(manager, 0x40406301, 0, fileBytes, true, objectOffset) == 0);
            assert(uint32_t(load(base + 0x720)) == 0x7206);
            assert(call(57, {fixedFd}) == 0 && call(57, {origin}) == 0);
            assert(bindRequest(manager, 0x40086303, 0, "", false, {}, requestBuffer) == 0);
            machine.cpu.retired += 4096;
            assert(linux.schedule(machine) && linux.processId() == binderChild);
            assert(bindRequest(channel, 0, 0, "", true) == 0);
            if (accepts) {
                assert(uint32_t(load(base + 0x720)) == 0x80407203);
                const auto response = load(base + 0x724 + 6 * 8);
                const auto received = load(response + 8);
                assert(load(response) == fileType && received < fixedFd);
                assert(call(25, {received, 1}) == 1); // F_GETFD, FD_CLOEXEC
                assert(bindRequest(channel, 0x40086303, 0, "", false, {}, response) == 0);
                assert(call(63, {received, base + 0x900, 8}) == 8 && load(base + 0x900) == 29);
                assert(call(57, {received}) == 0);
            } else {
                assert(uint32_t(load(base + 0x720)) == 0x7211);
            }
        }
        assert(call(57, {fixedFd}) == 0);
    }
    const auto binderWorker = call(220, {0x3d0f00, base + 0x900, base + 0x810, base + 0xa00, base + 0x810});
    assert(binderWorker > binderChild);
    call(93, {0}); // The leader exits, leaving its Binder connection alive in the worker.
    assert(linux.schedule(machine) && linux.processId() == binderChild && linux.threadId() == binderWorker);
    machine.cpu.retired += 4096;
    assert(linux.schedule(machine) && linux.processId() == 1);
    assert(bindRequest(manager, 0, 0, "", true) == manager && linux.blocked());
    assert(linux.schedule(machine) && linux.threadId() == binderWorker);
    call(94, {0});
    assert(linux.schedule(machine) && linux.processId() == 1);

    assert(bindRequest(manager, 0, 0, "", true) == 0);
    assert(uint32_t(load(base + 0x720)) == 0x8008720f && load(base + 0x724) == 0);
    assert(bindRequest(manager, 0x40086310, 0, "", false) == 0);

    Startup commandLine{};
    commandLine.filename = "/system/bin/app_process64";
    commandLine.argv0 = "app_process";
    commandLine.arguments = {"--zygote"};
    linux.setCommandLine(commandLine);
    put(base + 0x280, "/proc/self/cmdline");
    const auto commandLineFd = call(56, {uint32_t(-100), base + 0x280, 0});
    constexpr char expectedCommandLine[] = "app_process\0--zygote";
    assert(call(63, {commandLineFd, base + 0x300, 64}) == sizeof expectedCommandLine);
    assert(std::memcmp(machine.read(base + 0x300, sizeof expectedCommandLine, 4),
                       expectedCommandLine, sizeof expectedCommandLine) == 0);
    assert(call(62, {commandLineFd, 0, 0}) == 0);
    assert(call(57, {commandLineFd}) == 0);

    const auto killedChild = call(220, {0x1200011, 0, 0, 0, base + 0x848});
    assert(killedChild > binderChild);
    const auto processHandle = call(434, {killedChild, 0});
    assert(processHandle < 256 && call(25, {processHandle, 1}) == 1);
    const auto duplicateHandle = call(25, {processHandle, 1030, 100});
    assert(duplicateHandle == 100 && call(25, {duplicateHandle, 1}) == 1);
    constexpr auto processPoll = base + 0x910;
    constexpr auto noWaitTimeout = base + 0x930;
    assert(machine.store64(processPoll, duplicateHandle | uint64_t(1) << 32));
    assert(machine.store64(noWaitTimeout, 0) && machine.store64(noWaitTimeout + 8, 0));
    assert(call(73, {processPoll, 1, noWaitTimeout}) == 0);
    assert(call(129, {killedChild, 9}) == 0);
    assert(call(129, {killedChild, 0}) == uint64_t(-3));
    assert(call(73, {processPoll, 1, noWaitTimeout}) == 1 && load(processPoll) >> 48 == 1);
    assert(call(95, {3, processHandle, base + 512, 0x1000005}) == 0);
    assert((load(base + 520) & 0xffffffff) == 2);
    assert(load(base + 528) == killedChild && load(base + 536) == 9);
    assert(call(260, {killedChild, base + 512, 1}) == killedChild && load(base + 512) == 9);
    assert(call(73, {processPoll, 1, noWaitTimeout}) == 1 && load(processPoll) >> 48 == 17);
    assert(call(57, {processHandle}) == 0 && call(57, {duplicateHandle}) == 0);
    const auto groupLeader = call(220, {0x1200011, 0, 0, 0, base + 0x848});
    const auto groupMember = call(220, {0x1200011, 0, 0, 0, base + 0x848});
    assert(groupLeader > killedChild && groupMember > groupLeader);
    assert(call(154, {groupLeader, groupLeader}) == 0);
    assert(call(154, {groupMember, groupLeader}) == 0);
    assert(call(129, {0 - groupLeader, 15}) == 0);
    assert(call(260, {groupLeader, base + 512, 1}) == groupLeader && load(base + 512) == 15);
    assert(call(260, {groupMember, base + 512, 1}) == groupMember && load(base + 512) == 15);



    {
        namespace fs = std::filesystem;
        auto temporary = (fs::temp_directory_path() / "nestvane-data-XXXXXX").string();
        assert(mkdtemp(temporary.data()));
        const struct Cleanup {
            const fs::path directory;
            ~Cleanup() { std::error_code ignored; fs::remove_all(directory, ignored); }
        } cleanup{temporary};
        const auto database = (cleanup.directory / "guest.sqlite3").string();
        ProbeLinux writer(1, 0), stale(1, 0);
        writer.mountData(database);
        stale.mountData(database);
        const auto invoke = [&](ProbeLinux& guest, uint64_t number, std::array<uint64_t, 6> args) {
            std::copy(args.begin(), args.end(), machine.cpu.x.begin());
            machine.cpu.x[8] = number;
            assert(guest.syscall(machine));
            return machine.cpu.x[0];
        };
        {
            ProbeLinux quota(1, 0);
            quota.mountData((cleanup.directory / "quota.sqlite3").string());
            quota.addBootTmpfs("/metadata", 0755, 0, 6);
            constexpr uint64_t cwd = uint32_t(-100);
            for (unsigned i = 0; i < 1024; ++i) {
                const auto path = "/metadata/" + std::to_string(i);
                put(base, path.c_str());
                const auto file = invoke(quota, 56, {cwd, base, 0xc1, 0600});
                assert(file < 256 && invoke(quota, 57, {file}) == 0);
            }
            put(base, "/metadata/overflow");
            assert(invoke(quota, 56, {cwd, base, 0xc1, 0600}) == uint64_t(-28));
            put(base, "/data/package");
            const auto persistent = invoke(quota, 56, {cwd, base, 0xc1, 0600});
            assert(persistent < 256 && invoke(quota, 57, {persistent}) == 0 && quota.syncData());
        }
        constexpr auto atCwd = uint32_t(-100);
        constexpr auto firstPath = base + 0x280, secondPath = base + 0x300, payload = base + 0x380;
        assert(writer.loadFsCreatePolicy(
            "2 19\nu:r:init:s0 u:r:init:s0 40000 ffffffff 0 ffffffff 1 0\n"
            "u:r:shell:s0 u:r:shell:s0 0 ffffffff 0 ffffffff 1 0\n"));
        put(firstPath, "/data");
        assert(invoke(writer, 43, {firstPath, base + 0x450}) == 0);
        const auto quotaBlocks = load(base + 0x460);
        assert(quotaBlocks == (128 * 1024 * 1024) / pageSize);
        assert(load(base + 0x468) == quotaBlocks && load(base + 0x470) == quotaBlocks);
        put(firstPath, "/proc/thread-self/attr/fscreate");
        const auto labelFd = invoke(writer, 56, {atCwd, firstPath, 2});
        assert(labelFd < 256);
        constexpr char context[] = "u:object_r:system_data_file:s0:c512,c768";
        put(payload, context);
        writer.setBootContext("u:r:shell:s0");
        assert(invoke(writer, 64, {labelFd, payload, sizeof(context)}) == uint64_t(-13));
        writer.setBootContext("u:r:init:s0");
        assert(invoke(writer, 64, {labelFd, payload, sizeof(context)}) == sizeof(context));
        assert(invoke(writer, 57, {labelFd}) == 0);
        put(firstPath, "/data/private");
        assert(invoke(writer, 34, {atCwd, firstPath, 0700}) == 0);
        constexpr auto labelName = base + 0x450;
        put(labelName, "security.selinux");
        put(payload, context);
        assert(invoke(writer, 6, {firstPath, labelName, payload, sizeof(context), 1}) == uint64_t(-17));
        assert(invoke(writer, 6, {firstPath, labelName, payload, sizeof(context), 2}) == 0);
        put(payload, "/proc/thread-self/attr/fscreate");
        const auto clearLabel = invoke(writer, 56, {atCwd, payload, 2});
        assert(clearLabel < 256 && invoke(writer, 64, {clearLabel, 0, 0}) == 0);
        assert(invoke(writer, 57, {clearLabel}) == 0);
        assert(invoke(writer, 54, {atCwd, firstPath, 1000, 1000}) == 0);
        assert(invoke(writer, 146, {1000}) == 0);
        put(firstPath, "/data/private/draft");
        put(secondPath, "/data/private/current");
        const auto saved = invoke(writer, 56, {atCwd, firstPath, 0xc2, 0600});
        assert(saved < 256 && machine.store64(payload, 0xdecafbad12345678ULL));
        assert(invoke(writer, 64, {saved, payload, 8}) == 8);
        assert(invoke(writer, 38, {atCwd, firstPath, atCwd, secondPath}) == 0);
        put(firstPath, "/data/private");
        const auto activeDirectory = invoke(writer, 56, {atCwd, firstPath, 0x4000});
        assert(activeDirectory < 256);
        put(secondPath, "/data/moved");
        assert(invoke(writer, 38, {atCwd, firstPath, atCwd, secondPath}) == 0);
        put(payload, "current");
        const auto moved = invoke(writer, 56, {activeDirectory, payload, 0});
        assert(moved < 256 && invoke(writer, 57, {moved}) == 0);
        assert(invoke(writer, 57, {activeDirectory}) == 0);
        assert(invoke(writer, 82, {saved}) == 0);
        assert(!stale.syncData());
        ProbeLinux reopened(1, 1000), other(1, 2000);
        reopened.mountData(database);
        other.mountData(database);
        put(payload, "security.selinux");
        assert(invoke(reopened, 8, {secondPath, payload, base + 0x480, 128}) == sizeof(context));
        assert(std::memcmp(machine.read(base + 0x480, sizeof(context), 4), context, sizeof(context)) == 0);
        put(secondPath, "/data/moved/current");
        assert(invoke(other, 56, {atCwd, secondPath, 0}) == uint64_t(-13));
        assert(invoke(reopened, 56, {atCwd, firstPath, 0}) == uint64_t(-2));
        const auto restored = invoke(reopened, 56, {atCwd, secondPath, 0});
        assert(restored < 256 && invoke(reopened, 63, {restored, payload, 8}) == 8);
        assert(load(payload) == 0xdecafbad12345678ULL);
        assert(invoke(reopened, 35, {atCwd, secondPath, 0}) == 0);
        assert(invoke(reopened, 82, {restored}) == 0);
        ProbeLinux deleted(1, 1000);
        deleted.mountData(database);
        assert(invoke(deleted, 56, {atCwd, secondPath, 0}) == uint64_t(-2));
    }

    put(base + 0x220, "bpf");
    put(base + 0x240, "/sys/fs/bpf");
    put(base + 0x260, "/sys/fs/bpf/netd");
    put(base + 0x280, "/sys/fs/bpf/netd/regular-file");
    assert(call(40, {base + 0x220, base + 0x240, base + 0x220, 14}) == 0);
    assert(call(43, {base + 0x240, base + 0x2a0}) == 0);
    assert(load(base + 0x2a0) == 0xcafe4a11);
    assert(call(34, {uint32_t(-100), base + 0x260, 0700}) == 0);
    assert(call(56, {uint32_t(-100), base + 0x280, 0x42, 0600}) == uint64_t(-13));
    put(base + 0x320, "/proc/sys/kernel/unprivileged_bpf_disabled");
    const auto bpfSetting = call(56, {uint32_t(-100), base + 0x320, 2});
    assert(bpfSetting < 256);
    assert(call(63, {bpfSetting, base + 0x360, 2}) == 2);
    assert(std::memcmp(machine.read(base + 0x360, 2, 4), "2\n", 2) == 0);
    assert(call(62, {bpfSetting, 0, 0}) == 0);
    put(base + 0x360, "0\n");
    assert(call(64, {bpfSetting, base + 0x360, 2}) == 2);
    assert(call(62, {bpfSetting, 0, 0}) == 0);
    put(base + 0x360, "1\n");
    assert(call(64, {bpfSetting, base + 0x360, 2}) == 2);
    assert(call(62, {bpfSetting, 0, 0}) == 0);
    put(base + 0x360, "0\n");
    assert(call(64, {bpfSetting, base + 0x360, 2}) == uint64_t(-1));
    assert(call(57, {bpfSetting}) == 0);


    assert(linux.loadFsCreatePolicy(
        "2 19 30\n"
        "u:r:init:s0 u:r:init:s0 20040000 ffffffff 0 ffffffff 1 0\n"
        "u:r:shell:s0 u:r:shell:s0 0 ffffffff 0 ffffffff 1 0\n"));
    put(base + 0x320, "/proc/thread-self/attr/sockcreate");
    constexpr char zygoteContext[] = "u:r:zygote:s0";
    put(base + 0x360, zygoteContext);
    linux.setBootContext("u:r:shell:s0");
    const auto deniedSocketLabel = call(56, {uint32_t(-100), base + 0x320, 1});
    assert(call(64, {deniedSocketLabel, base + 0x360, sizeof zygoteContext}) == uint64_t(-13));
    assert(call(57, {deniedSocketLabel}) == 0);
    linux.setBootContext("u:r:init:s0");
    const auto socketLabel = call(56, {uint32_t(-100), base + 0x320, 1});
    assert(call(64, {socketLabel, base + 0x360, sizeof zygoteContext}) == sizeof zygoteContext);
    assert(call(57, {socketLabel}) == 0);
    assert(call(199, {1, 1, 0, base + 0x3a0}) == 0);
    const auto labeledPair = load(base + 0x3a0);
    assert(machine.store64(base + 0x390, 32));
    assert(call(209, {uint32_t(labeledPair), 1, 31, base + 0x3c0, base + 0x390}) == 0);
    assert(load(base + 0x390) == sizeof zygoteContext);
    assert(std::memcmp(machine.read(base + 0x3c0, sizeof zygoteContext, 4), zygoteContext, sizeof zygoteContext) == 0);
    const auto socketSender = uint32_t(labeledPair), socketReceiver = uint32_t(labeledPair >> 32);
    assert(machine.store64(base + 0x390, 4) && machine.store64(base + 0x3c0, 3));
    assert(call(208, {socketSender, 1, 2, base + 0x3c0, 3}) == uint64_t(-22));
    assert(call(208, {socketSender, 1, 2, 0xdead, 4}) == uint64_t(-14));
    assert(call(208, {socketSender, 1, 2, base + 0x3c0, 4}) == 0);
    assert(call(209, {socketSender, 1, 2, base + 0x3c0, base + 0x390}) == 0);
    assert((load(base + 0x3c0) & UINT32_MAX) == 1);
    assert(call(209, {socketReceiver, 1, 2, base + 0x3c0, base + 0x390}) == 0);
    assert((load(base + 0x3c0) & UINT32_MAX) == 0);
    assert(call(208, {socketSender, 1, 2, base + 0x3c0, 4}) == 0);
    assert(call(209, {socketSender, 1, 2, base + 0x3c0, base + 0x390}) == 0);
    assert((load(base + 0x3c0) & UINT32_MAX) == 0);
    assert(machine.store64(base + 0x390, 4) && machine.store64(base + 0x3c0, 4096));
    assert(call(208, {socketSender, 1, 7, base + 0x3c0, 4}) == 0);
    assert(call(209, {socketSender, 1, 7, base + 0x3c0, base + 0x390}) == 0);
    assert((load(base + 0x3c0) & UINT32_MAX) == 8192);
    assert(call(208, {socketReceiver, 1, 8, base + 0x3c0, 4}) == 0);
    assert(call(209, {socketReceiver, 1, 8, base + 0x3c0, base + 0x390}) == 0);
    assert((load(base + 0x3c0) & UINT32_MAX) == 16384);
    for (unsigned i = 0; i < 8; ++i) assert(call(64, {socketSender, base, 1024}) == 1024);
    assert(call(25, {socketSender, 4, 0x800}) == 0); // O_NONBLOCK
    assert(call(64, {socketSender, base, 1}) == uint64_t(-11));
    assert(call(63, {socketReceiver, base, 512}) == 512);
    assert(call(64, {socketSender, base, 1024}) == 512);
    assert(call(57, {socketSender}) == 0 && call(57, {socketReceiver}) == 0);

    put(base + 0x320, "/dev/urandom");
    const auto entropyFd = call(56, {uint32_t(-100), base + 0x320, 0});
    assert(entropyFd < 256);
    assert(call(63, {entropyFd, base + 0x350, 32}) == 32);
    assert(call(63, {entropyFd, 0xdead, 32}) == uint64_t(-14));
    assert(call(63, {entropyFd, 0, 0}) == 0);
    assert(call(57, {entropyFd}) == 0);
    assert(call(278, {base + 0x350, 16, 4}) == 16); // GRND_INSECURE is valid after initialization too.
    assert(call(278, {base + 0x350, 16, 6}) == uint64_t(-22));

    assert(call(126, {1}) == 1 && call(125, {2}) == 99);
    assert(call(126, {0}) == 0 && call(125, {4}) == uint64_t(-22));
    assert(machine.store64(base + 0x390, 2));
    assert(call(119, {0, 1, base + 0x390}) == uint64_t(-95));
    assert(call(120, {0}) == 0);

    assert(call(163, {7, base + 0x400}) == 0);
    const auto softDescriptorLimit = load(base + 0x400);
    assert(softDescriptorLimit > 256);
    const auto highDescriptor = call(25, {0, 1030, 256});
    assert(highDescriptor == 256 && call(25, {highDescriptor, 1}) == 1);
    assert(call(24, {highDescriptor, softDescriptorLimit, 0}) == uint64_t(-9));
    assert(call(57, {highDescriptor}) == 0);

    const auto split = call(222, {0, pageSize * 2, 3, 0x22, uint64_t(-1), 0});
    assert(split > base && split < (uint64_t{1} << 45));
    assert(machine.store64(split + pageSize, 42));
    assert(call(226, {split + pageSize, pageSize, 1}) == 0);
    assert(call(216, {split, pageSize * 2, pageSize * 3, 1}) == uint64_t(-14));
    assert(call(226, {split, pageSize * 2, 3}) == 0);
    const auto resized = call(216, {split, pageSize * 2, pageSize * 3, 1});
    assert(resized > base && resized < (uint64_t{1} << 45));
    assert(load(resized + pageSize) == 42 && load(resized + pageSize * 2) == 0);
    assert(call(215, {resized, pageSize * 3}) == 0);

    assert(call(163, {3, base + 0x400}) == 0);
    assert(load(base + 0x400) == guestStackSize && load(base + 0x408) == maxGuestStackSize);
    Startup startup{};
    startup.filename = "/system/bin/app_process64";
    startup.arguments = {"--zygote", "--start-system-server"};
    startup.environment = {"ANDROID_ROOT=/system", "ANDROID_DATA=/data"};
    auto initialStack = linux.createMachine(Image{base, {}, startup});
    constexpr uint64_t stackBottom = 0x8000'0000 - guestStackSize;
    assert(!initialStack.read(stackBottom - 1, 1, 4));
    assert(initialStack.store64(stackBottom, 42));
    const auto stackWord = [&](size_t index) {
        uint64_t value = 0;
        assert(initialStack.load64(initialStack.cpu.sp + index * sizeof(value), value));
        return value;
    };
    assert(stackWord(0) == 3 && stackWord(4) == 0 && stackWord(7) == 0);
    assert(stackWord(1) + startup.filename.size() + 1 == stackWord(2));
    assert(stackWord(2) + startup.arguments[0].size() + 1 == stackWord(3));
    assert(stackWord(3) + startup.arguments[1].size() + 1 == stackWord(5));
    assert(stackWord(5) + startup.environment[0].size() + 1 == stackWord(6));
    uint64_t ticksPerSecond = 0;
    for (size_t index = 8; const auto tag = stackWord(index); index += 2) {
        if (tag == 17) ticksPerSecond = stackWord(index + 1);
    }
    assert(ticksPerSecond == 100);

    put(base + 0x320, "art-heap-test");
    assert(call(279, {base + 0x320, 4}) == uint64_t(-22));
    const auto memoryFile = call(279, {base + 0x320, 1});
    assert(memoryFile < 256 && call(25, {memoryFile, 1}) == 1);
    assert(call(25, {memoryFile, 1034}) == 1);
    assert(call(46, {memoryFile, 3 * 1024 * 1024}) == 0);
    assert(machine.store64(base + 0x400, 0));
    assert(call(62, {memoryFile, pageSize, 0}) == pageSize);
    assert(call(29, {memoryFile, 0x541b, base + 0x400}) == 0 && load(base + 0x400) == 3 * 1024 * 1024 - pageSize);
    assert(call(29, {memoryFile, 0x541b, 0xdead}) == uint64_t(-14));
    const auto mappedMemoryFile = call(222, {0, pageSize, 3, 1, memoryFile, 0});
    assert(mappedMemoryFile > base && mappedMemoryFile < (uint64_t{1} << 45));
    assert(load(mappedMemoryFile) == 0 && machine.store64(mappedMemoryFile, 42));
    assert(call(67, {memoryFile, base + 0x400, 8, 0}) == 8 && load(base + 0x400) == 42);
    constexpr uint64_t lowFileMap = 0x4100'0000;
    assert(call(222, {lowFileMap, pageSize, 0, 0x100022, uint64_t(-1), 0}) == lowFileMap);
    assert(call(222, {lowFileMap, pageSize, 1, 0x12, memoryFile, 0}) == lowFileMap);
    assert(load(lowFileMap) == 42);
    assert(call(226, {lowFileMap, pageSize, 3}) == 0);
    assert(machine.store64(lowFileMap, 19) && load(mappedMemoryFile) == 42);
    assert(call(215, {lowFileMap, pageSize}) == 0);
    assert(call(57, {memoryFile}) == 0 && load(mappedMemoryFile) == 42);
    assert(call(215, {mappedMemoryFile, pageSize}) == 0);

    const auto sealable = call(279, {base + 0x320, 3});
    assert(sealable < 256 && call(25, {sealable, 1034}) == 0);
    assert(call(46, {sealable, pageSize * 2}) == 0);
    const auto writer = call(222, {0, pageSize, 3, 1, sealable, 0});
    assert(writer < (uint64_t{1} << 45));
    assert(call(25, {sealable, 1033, 8}) == uint64_t(-16));
    assert(call(25, {sealable, 1033, 2 | 4 | 16}) == 0);
    assert(call(46, {sealable, pageSize}) == uint64_t(-1));
    assert(call(46, {sealable, pageSize * 3}) == uint64_t(-1));
    assert(call(68, {sealable, base, 8, 0}) == uint64_t(-1));
    assert(call(222, {0, pageSize, 3, 1, sealable, 0}) == uint64_t(-1));
    const auto reader = call(222, {0, pageSize, 1, 1, sealable, 0});
    assert(reader < (uint64_t{1} << 45) && machine.store64(writer, 123) && load(reader) == 123);
    assert(call(226, {reader, pageSize, 3}) == uint64_t(-13));
    assert(call(215, {writer, pageSize}) == 0);
    assert(call(25, {sealable, 1033, 8 | 1}) == 0);
    assert(call(25, {sealable, 1034}) == 31);
    assert(call(25, {sealable, 1033, 0}) == uint64_t(-1));
    assert(call(57, {sealable}) == 0 && call(215, {reader, pageSize}) == 0);

    std::cout << "guest kernel ABI checks passed, including bind views, fork, SIGCHLD, pipe, boot policy, loop bounds, PTY, uevents, guest sysctls, securebits, I/O priority, Binder transactions and persisted guest data\n";
}
