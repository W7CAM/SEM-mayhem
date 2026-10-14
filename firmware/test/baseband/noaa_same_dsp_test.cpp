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
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace {

constexpr double two_pi = 6.283185307179586476925;
constexpr uint32_t fs = 768000;
constexpr uint32_t audio_fs = 24000;
constexpr uint8_t channel_off = 0xff;
constexpr size_t max_decoders = 3;
constexpr std::array<int32_t, 7> noaa_offsets_hz{{
    -75000,
    -50000,
    -25000,
    0,
    25000,
    50000,
    75000,
}};
constexpr std::array<uint32_t, 7> noaa_frequencies_hz{{
    162400000,
    162425000,
    162450000,
    162475000,
    162500000,
    162525000,
    162550000,
}};

struct DecoderAssignments {
    std::array<uint8_t, max_decoders> channels{{channel_off, channel_off, channel_off}};
    uint8_t audio_slot{0};

    int slot_for_channel(uint8_t channel) const {
        for (size_t i = 0; i < channels.size(); ++i) {
            if (channels[i] == channel)
                return static_cast<int>(i);
        }
        return -1;
    }

    bool assign(uint8_t channel) {
        if (slot_for_channel(channel) >= 0)
            return true;
        for (auto& assigned : channels) {
            if (assigned == channel_off) {
                assigned = channel;
                sanitize_audio_slot();
                return true;
            }
        }
        return false;
    }

    void release(uint8_t channel) {
        for (auto& assigned : channels) {
            if (assigned == channel)
                assigned = channel_off;
        }
        sanitize_audio_slot();
    }

    void sanitize_audio_slot() {
        if (audio_slot < channels.size() && channels[audio_slot] != channel_off)
            return;
        for (size_t i = 0; i < channels.size(); ++i) {
            if (channels[i] != channel_off) {
                audio_slot = i;
                return;
            }
        }
        audio_slot = 0;
    }
};

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

std::vector<std::complex<double>> nfm_modulated_audio(size_t count, double audio_hz, double deviation_hz) {
    std::vector<std::complex<double>> iq(count);
    double phase = 0.0;
    for (size_t n = 0; n < count; ++n) {
        const double audio = std::sin(two_pi * audio_hz * static_cast<double>(n) / audio_fs);
        phase += two_pi * deviation_hz * audio / audio_fs;
        iq[n] = {std::cos(phase), std::sin(phase)};
    }
    return iq;
}

std::vector<double> fm_demodulate(const std::vector<std::complex<double>>& iq, double deviation_hz) {
    std::vector<double> audio;
    audio.reserve(iq.size() - 1);
    for (size_t n = 1; n < iq.size(); ++n) {
        const auto delta = iq[n] * std::conj(iq[n - 1]);
        audio.push_back(std::arg(delta) / (two_pi * deviation_hz / audio_fs));
    }
    return audio;
}

double rms(const std::vector<double>& samples) {
    double sum = 0.0;
    for (auto sample : samples)
        sum += sample * sample;
    return std::sqrt(sum / samples.size());
}

}  // namespace

TEST_CASE("noaa same channel frequencies map to the fixed NOAA weather channels") {
    CHECK(noaa_frequencies_hz[0] == 162400000);
    CHECK(noaa_frequencies_hz[3] == 162475000);
    CHECK(noaa_frequencies_hz[6] == 162550000);

    for (size_t i = 0; i < noaa_frequencies_hz.size(); ++i)
        CHECK(static_cast<int32_t>(noaa_frequencies_hz[i]) - 162475000 == noaa_offsets_hz[i]);
}

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

TEST_CASE("noaa same decoder assignment preserves the three-slot cap and audio source") {
    DecoderAssignments assignments{};

    CHECK(assignments.assign(6));
    CHECK(assignments.assign(0));
    CHECK(assignments.assign(3));
    CHECK_FALSE(assignments.assign(4));
    CHECK(assignments.slot_for_channel(6) == 0);
    CHECK(assignments.slot_for_channel(0) == 1);
    CHECK(assignments.slot_for_channel(3) == 2);

    assignments.audio_slot = 2;
    assignments.release(3);
    CHECK(assignments.slot_for_channel(3) == -1);
    CHECK(assignments.audio_slot == 0);
}

TEST_CASE("noaa same audio source is silent when no decoder slot is active") {
    DecoderAssignments assignments{};
    assignments.sanitize_audio_slot();
    CHECK(assignments.audio_slot == 0);
    CHECK(std::none_of(assignments.channels.begin(), assignments.channels.end(), [](uint8_t channel) {
        return channel != channel_off;
    }));
}

TEST_CASE("noaa same synthetic nfm demodulation recovers a weather audio tone") {
    const auto iq = nfm_modulated_audio(2048, 1050.0, 5000.0);
    const auto audio = fm_demodulate(iq, 5000.0);

    CHECK(rms(audio) == doctest::Approx(0.707).epsilon(0.03));
    CHECK(*std::max_element(audio.begin(), audio.end()) < 1.05);
    CHECK(*std::min_element(audio.begin(), audio.end()) > -1.05);
}
