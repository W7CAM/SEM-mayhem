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

#include "proc_noaa_same.hpp"

#include "audio_dma.hpp"
#include "dsp_fir_taps.hpp"
#include "dsp_iir_config.hpp"
#include "event_m4.hpp"
#include "portapack_shared_memory.hpp"
#include "utility.hpp"

#include <algorithm>
#include <cstddef>

namespace {

constexpr std::array<int32_t, NoaaSameStatusMessage::channel_count> noaa_offsets_hz{{
    -75000,
    -50000,
    -25000,
    0,
    25000,
    50000,
    75000,
}};

constexpr std::array<uint32_t, NoaaSameStatusMessage::channel_count> noaa_frequencies_hz{{
    162400000,
    162425000,
    162450000,
    162475000,
    162500000,
    162525000,
    162550000,
}};

constexpr int16_t minimum_qualified_relative_db = 6;

uint32_t max_magnitude_squared(const buffer_c16_t& src) {
    uint32_t max_mag_sq = 0;
    for (size_t i = 0; i < src.count; ++i) {
        const int32_t i_sample = src.p[i].real();
        const int32_t q_sample = src.p[i].imag();
        const uint32_t mag_sq = static_cast<uint32_t>(i_sample * i_sample + q_sample * q_sample);
        if (mag_sq > max_mag_sq)
            max_mag_sq = mag_sq;
    }
    return max_mag_sq;
}

int16_t magnitude_to_db(uint32_t mag_sq) {
    if (mag_sq == 0)
        return -120;

    const float normalized = static_cast<float>(mag_sq) * (1.0f / (32768.0f * 32768.0f));
    return static_cast<int16_t>(mag2_to_dbv_norm(normalized));
}

uint8_t channel_for_frequency(uint32_t frequency) {
    for (size_t i = 0; i < noaa_frequencies_hz.size(); ++i) {
        if (noaa_frequencies_hz[i] == frequency)
            return i;
    }

    return 0xff;
}

}  // namespace

NoaaSameProcessor::NoaaSameProcessor() {
    configure();
}

void NoaaSameProcessor::DecoderSlot::reset(uint8_t new_channel) {
    channel = new_channel;
    active = new_channel < NoaaSameStatusMessage::channel_count;
    same_busy = false;
    audio_blocks = 0;
    demod.configure(NoaaSameProcessor::demod_input_fs, 5000);
}

void NoaaSameProcessor::configure() {
    configure_fs4();
    decim_1.configure(taps_audio_wide_halfband_0.taps);

    for (size_t i = 0; i < channels.size(); ++i) {
        auto& channel = channels[i];
        channel.translator.configure(taps_16k0_decim_1.taps, channelizer_input_fs);
        channel.translator.set_frequency(noaa_offsets_hz[i]);
        channel.channel_filter.configure(taps_16k0_channel.taps, 2);
    }

    for (auto& decoder : decoders)
        decoder.reset(0xff);

    audio_output.configure(audio_24k_hpf_300hz_config, audio_24k_deemph_300_6_config, 0.0f);
    configured = true;
}

void NoaaSameProcessor::execute(const buffer_c8_t& buffer) {
    if (!configured)
        return;

    if (requested_fs4_direction_.load(std::memory_order_relaxed) != applied_fs4_direction_)
        configure_fs4();

    block_count++;

    const auto decim_0_out = decim_0.execute(buffer, common_buffer);
    const auto channelizer_in = decim_1.execute(decim_0_out, common_buffer);

    for (size_t i = 0; i < channels.size(); ++i)
        process_channel(i, channelizer_in);

    if ((block_count % status_interval_blocks) == 0) {
        update_channel_power();
        send_status();
    }
}

void NoaaSameProcessor::process_channel(size_t channel_index, const buffer_c16_t& src) {
    auto& channel = channels[channel_index];
    const auto translated = channel.translator.execute(src, work_buffer);
    const auto channel_out = channel.channel_filter.execute(translated, work_buffer);

    const auto mag_sq = max_magnitude_squared(channel_out);
    if (mag_sq > channel.max_mag_sq)
        channel.max_mag_sq = mag_sq;

    for (size_t slot_index = 0; slot_index < decoders.size(); ++slot_index) {
        auto& decoder = decoders[slot_index];
        if (!decoder.active || decoder.channel != channel_index)
            continue;

        const auto audio_out = decoder.demod.execute(channel_out, audio_buffer);
        decoder.audio_blocks++;

        if (audio_enabled && slot_index == audio_slot)
            audio_output.write(audio_out);

        /* SAME decoder hook: feed audio_out into this slot's future SAME state machine. */
    }
}

void NoaaSameProcessor::update_channel_power() {
    int16_t noise_floor = 120;

    for (auto& channel : channels) {
        channel.power_db = magnitude_to_db(channel.max_mag_sq);
        channel.max_mag_sq = 0;
        noise_floor = std::min(noise_floor, channel.power_db);
    }

    for (auto& channel : channels) {
        channel.relative_db = channel.power_db - noise_floor;
        channel.qualified = channel.relative_db >= minimum_qualified_relative_db;
    }
}

void NoaaSameProcessor::send_status() {
    NoaaSameStatusMessage message{};

    for (size_t i = 0; i < channels.size(); ++i) {
        message.channels[i].power_db = channels[i].power_db;
        message.channels[i].relative_db = channels[i].relative_db;
        message.channels[i].qualified = channels[i].qualified;
    }

    for (size_t i = 0; i < decoders.size(); ++i) {
        message.decoders[i].channel = decoders[i].channel;
        message.decoders[i].active = decoders[i].active;
        message.decoders[i].same_busy = decoders[i].same_busy;
        message.decoders[i].audio_blocks = decoders[i].audio_blocks;
    }

    message.update_count = ++status_count;
    message.baseband_blocks = block_count;
    message.deadline_misses = deadline_misses;
    message.active_decoders = active_decoder_count();
    shared_memory.application_queue.push(message);
}

uint8_t NoaaSameProcessor::active_decoder_count() const {
    uint8_t active = 0;
    for (const auto& decoder : decoders) {
        if (decoder.active)
            active++;
    }
    return active;
}

void NoaaSameProcessor::on_message(const Message* const message) {
    switch (message->id) {
        case Message::ID::RxFs4Config:
            requested_fs4_direction_.store(
                static_cast<const RxFs4ConfigMessage*>(message)->direction,
                std::memory_order_relaxed);
            break;

        case Message::ID::NoaaSameConfigure:
            configure(*reinterpret_cast<const NoaaSameConfigureMessage*>(message));
            break;

        default:
            break;
    }
}

void NoaaSameProcessor::configure(const NoaaSameConfigureMessage& message) {
    audio_enabled = message.audio_enabled;
    audio_slot = message.audio_slot < max_decoders ? message.audio_slot : 0;

    for (size_t i = 0; i < decoders.size(); ++i) {
        const auto new_channel = channel_for_frequency(message.decoder_frequencies[i]);
        if (decoders[i].channel == new_channel)
            continue;

        if (decoders[i].same_busy && !message.immediate_override)
            continue;

        decoders[i].reset(new_channel);
    }

    send_status();
}

void NoaaSameProcessor::configure_fs4() {
    chSysLock();
    const auto direction = requested_fs4_direction_.load(std::memory_order_relaxed);
    using Shift = dsp::decimate::FIRC8xR16x24FS4Decim4::Shift;
    decim_0.configure(taps_16k0_decim_0.taps, 33554432,
                      direction == RxFs4Direction::Up ? Shift::Up : Shift::Down);
    applied_fs4_direction_ = direction;
    chSysUnlock();
}

int main() {
    audio::dma::init_audio_out();
    EventDispatcher event_dispatcher{std::make_unique<NoaaSameProcessor>()};
    event_dispatcher.run();
    return 0;
}
