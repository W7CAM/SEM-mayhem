/*
 * Copyright (C) 2026
 *
 * This file is part of PortaPack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 */

#include "doctest.h"

#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace {

constexpr double two_pi = 6.283185307179586476925;
constexpr uint32_t fs = 768000;
constexpr std::array<int32_t, 7> noaa_offsets_hz{{
    -75000,
    -50000,
    -25000,
    0,
    25000,
    50000,
    75000,
}};

uint64_t mean_complex_power(const std::vector<std::complex<int16_t>>& iq) {
    uint64_t sum = 0;
    for (const auto& sample : iq) {
        const int32_t i = sample.real();
        const int32_t q = sample.imag();
        sum += static_cast<uint32_t>((i * i) + (q * q));
    }
    return sum / iq.size();
}

std::vector<std::complex<int16_t>> tone(size_t count, int32_t offset_hz, int16_t amplitude) {
    std::vector<std::complex<int16_t>> iq(count);
    for (size_t n = 0; n < count; ++n) {
        const double phase = two_pi * offset_hz * static_cast<double>(n) / fs;
        iq[n] = {
            static_cast<int16_t>(std::lround(amplitude * std::cos(phase))),
            static_cast<int16_t>(std::lround(amplitude * std::sin(phase))),
        };
    }
    return iq;
}

double channel_power_at(const std::vector<std::complex<int16_t>>& iq, int32_t offset_hz) {
    std::complex<double> acc{0.0, 0.0};
    for (size_t n = 0; n < iq.size(); ++n) {
        const double phase = -two_pi * offset_hz * static_cast<double>(n) / fs;
        acc += std::complex<double>(iq[n].real(), iq[n].imag()) *
               std::complex<double>(std::cos(phase), std::sin(phase));
    }
    return std::norm(acc) / static_cast<double>(iq.size() * iq.size());
}

}  // namespace

TEST_CASE("noaa same channel offsets are 25 kHz apart and centered on channel 4") {
    for (size_t i = 1; i < noaa_offsets_hz.size(); ++i)
        CHECK(noaa_offsets_hz[i] - noaa_offsets_hz[i - 1] == 25000);
    CHECK(noaa_offsets_hz[3] == 0);
}

TEST_CASE("noaa same mean complex power uses average power rather than peak") {
    const auto iq = tone(1024, 25000, 1200);
    CHECK(mean_complex_power(iq) == doctest::Approx(1200u * 1200u).epsilon(0.002));

    auto pulsed = iq;
    for (size_t i = 0; i < pulsed.size(); i += 2)
        pulsed[i] = {0, 0};
    CHECK(mean_complex_power(pulsed) == doctest::Approx((1200u * 1200u) / 2).epsilon(0.003));
}

TEST_CASE("noaa same synthetic tones separate adjacent weather channels") {
    const auto iq = tone(4096, noaa_offsets_hz[5], 1000);
    const double wanted = channel_power_at(iq, noaa_offsets_hz[5]);
    const double adjacent = channel_power_at(iq, noaa_offsets_hz[4]);
    const double other_side = channel_power_at(iq, noaa_offsets_hz[6]);

    CHECK(wanted > 990.0 * 990.0);
    CHECK(10.0 * std::log10(wanted / adjacent) > 35.0);
    CHECK(10.0 * std::log10(wanted / other_side) > 35.0);
}
