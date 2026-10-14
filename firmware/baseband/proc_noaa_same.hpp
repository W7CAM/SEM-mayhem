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

#ifndef __PROC_NOAA_SAME_H__
#define __PROC_NOAA_SAME_H__

#include "audio_output.hpp"
#include "baseband_processor.hpp"
#include "baseband_thread.hpp"
#include "dsp_decimate.hpp"
#include "dsp_demodulate.hpp"
#include "dsp_frequency_xlator.hpp"
#include "message.hpp"
#include "rssi_thread.hpp"

#include <array>
#include <atomic>
#include <cstdint>

class NoaaSameProcessor : public BasebandProcessor {
   public:
    NoaaSameProcessor();

    void execute(const buffer_c8_t& buffer) override;
    void on_message(const Message* const message) override;

   private:
    static constexpr size_t channel_count = NoaaSameStatusMessage::channel_count;
    static constexpr size_t max_decoders = NoaaSameStatusMessage::max_decoders;
    static constexpr uint32_t baseband_fs = 3072000;
    static constexpr uint32_t decim_0_output_fs = baseband_fs / 4;
    static constexpr uint32_t channelizer_input_fs = decim_0_output_fs / 2;
    static constexpr uint32_t demod_input_fs = channelizer_input_fs / 8 / 2;
    static constexpr uint32_t status_updates_per_second = 4;
    static constexpr size_t status_interval_blocks = 1500 / status_updates_per_second;

    struct ChannelState {
        dsp::FrequencyTranslatingDecimator32By8 translator{};
        dsp::decimate::FIRAndDecimateComplex channel_filter{};
        uint64_t power_sum{0};
        uint32_t power_samples{0};
        uint32_t mean_mag_sq{0};
        uint32_t mean_mag_sq_30s{0};
        int16_t power_db{-120};
        int16_t power_30s_db{-120};
        int16_t relative_db{0};
        bool qualified{false};
        bool smoothed_power_valid{false};
    };

    struct DecoderSlot {
        dsp::demodulate::FM demod{};
        uint8_t channel{0xff};
        bool active{false};
        bool same_busy{false};
        uint32_t audio_blocks{0};

        void reset(uint8_t new_channel);
    };

    std::array<complex16_t, 512> common{};
    const buffer_c16_t common_buffer{
        common.data(),
        common.size()};

    std::array<complex16_t, 512> work{};
    const buffer_c16_t work_buffer{
        work.data(),
        work.size()};

    std::array<int16_t, 16> audio{};
    const buffer_s16_t audio_buffer{
        audio.data(),
        audio.size()};

    dsp::decimate::FIRC8xR16x24FS4Decim4 decim_0{};
    std::atomic<RxFs4Direction> requested_fs4_direction_{RxFs4Direction::Down};
    RxFs4Direction applied_fs4_direction_{RxFs4Direction::Down};
    dsp::decimate::FIRC16xR16x16Decim2 decim_1{};
    std::array<ChannelState, channel_count> channels{};
    std::array<DecoderSlot, max_decoders> decoders{};
    AudioOutput audio_output{};

    bool configured{false};
    bool audio_enabled{false};
    uint8_t audio_slot{0};
    uint32_t block_count{0};
    uint32_t status_input_samples{0};
    uint32_t status_count{0};

    void configure();
    void configure(const NoaaSameConfigureMessage& message);
    void configure_fs4();
    bool process_channel(size_t channel_index, const buffer_c16_t& src);
    void update_channel_power();
    void send_status();
    uint8_t active_decoder_count() const;

    /* NB: Threads should be the last members in the class definition. */
    BasebandThread baseband_thread{baseband_fs, this, baseband::Direction::Receive};
    RSSIThread rssi_thread{};
};

#endif /*__PROC_NOAA_SAME_H__*/
