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

#ifndef __UI_NOAA_SAME_H__
#define __UI_NOAA_SAME_H__

#include "app_settings.hpp"
#include "event_m0.hpp"
#include "message.hpp"
#include "radio_state.hpp"
#include "ui.hpp"
#include "ui_navigation.hpp"
#include "ui_widget.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace ui::external_app::noaa_same {

class NoaaSameView : public View {
   public:
    explicit NoaaSameView(NavigationView& nav);
    ~NoaaSameView();

    void focus() override;
    std::string title() const override { return "NOAA SAME"; }

   private:
    static constexpr size_t channel_count = NoaaSameStatusMessage::channel_count;
    static constexpr size_t max_decoders = NoaaSameStatusMessage::max_decoders;
    static constexpr uint8_t channel_off = 0xff;
    static constexpr rf::Frequency center_frequency = 162475000;
    static constexpr uint32_t receiver_bandwidth = 1750000;
    static constexpr uint32_t receiver_sample_rate = 3072000;

    struct Preset {
        std::string name;
        std::array<uint8_t, max_decoders> channels;
    };

    NavigationView& nav_;
    RxRadioState radio_state_{
        center_frequency,
        receiver_bandwidth,
        receiver_sample_rate,
        ReceiverModel::Mode::Capture};

    uint8_t startup_preset{0};
    uint8_t selected_preset{1};
    std::array<uint8_t, max_decoders> decoder_channels{{3, 4, 5}};
    std::array<uint8_t, max_decoders> pending_recommendation{{channel_off, channel_off, channel_off}};
    std::array<int16_t, channel_count> channel_power{};
    std::array<int16_t, channel_count> channel_relative{};
    std::array<bool, channel_count> channel_qualified{};
    std::array<int16_t, channel_count> scan_power{};
    std::array<Preset, 3> presets{{
        {"P1", {{3, 4, 5}}},
        {"P2", {{0, 1, 2}}},
        {"P3", {{4, 5, 6}}},
    }};

    bool audio_enabled{false};
    bool immediate_override{false};
    bool scanning{false};
    uint8_t scan_seconds_remaining{0};
    uint32_t update_count{0};
    uint32_t baseband_blocks{0};
    uint32_t deadline_misses{0};
    uint8_t active_decoders{0};
    uint8_t rx_sat_percent{0};
    SignalToken signal_token_tick_second{};

    app_settings::SettingsManager settings_{
        "rx_noaa_same",
        app_settings::Mode::RX,
        {
            {"startup_preset"sv, &startup_preset},
            {"selected_preset"sv, &selected_preset},
        }};

    Labels labels{{
        {{0, 0}, "NOAA Weather Monitor", Theme::getInstance()->fg_light->foreground},
        {{0, 16}, "Ch       Pwr  Rel Q", Theme::getInstance()->fg_light->foreground},
        {{0, 144}, "Decoders", Theme::getInstance()->fg_light->foreground},
        {{0, 208}, "Preset:", Theme::getInstance()->fg_light->foreground},
    }};

    std::array<Text, channel_count> channel_rows{{
        Text{{0, 32, 240, 16}},
        Text{{0, 48, 240, 16}},
        Text{{0, 64, 240, 16}},
        Text{{0, 80, 240, 16}},
        Text{{0, 96, 240, 16}},
        Text{{0, 112, 240, 16}},
        Text{{0, 128, 240, 16}},
    }};

    std::array<OptionsField, max_decoders> decoder_fields{{
        OptionsField{{0, 160}, 9, {}},
        OptionsField{{80, 160}, 9, {}},
        OptionsField{{160, 160}, 9, {}},
    }};

    std::array<Text, max_decoders> decoder_status{{
        Text{{0, 176, 72, 16}},
        Text{{80, 176, 72, 16}},
        Text{{160, 176, 72, 16}},
    }};

    OptionsField preset_field{
        {64, 208},
        6,
        {{"P1", 1}, {"P2", 2}, {"P3", 3}}};

    OptionsField startup_field{
        {128, 208},
        10,
        {{"Manual", 0}, {"P1", 1}, {"P2", 2}, {"P3", 3}}};

    Checkbox checkbox_audio{
        {0, 232},
        9,
        "Audio",
        true};

    Checkbox checkbox_force{
        {80, 232},
        9,
        "Force",
        true};

    Button button_rescan{
        {0, 256, 72, 24},
        "Rescan"};

    Button button_apply{
        {80, 256, 72, 24},
        "Apply"};

    Button button_load{
        {160, 256, 72, 24},
        "Load"};

    Button button_save{
        {0, 288, 72, 24},
        "Save"};

    Text text_status{
        {80, 288, 160, 16},
        ""};

    Text text_diag{
        {80, 304, 160, 16},
        ""};

    MessageHandlerRegistration message_handler_status{
        Message::ID::NoaaSameStatus,
        [this](Message* const p) {
            this->on_status(*reinterpret_cast<const NoaaSameStatusMessage*>(p));
        }};

    void configure_receiver();
    void send_config(bool immediate);
    void load_presets();
    void save_presets();
    void load_selected_preset();
    void save_selected_preset();
    void start_recommendation_scan();
    void finish_recommendation_scan();
    void on_tick_second();
    void on_status(const NoaaSameStatusMessage& message);
    void refresh_ui();
    void refresh_decoder_fields();
    void set_decoder_from_field(size_t slot, uint8_t channel);

    static OptionsField::options_t channel_options();
    static std::string channel_name(uint8_t channel);
    static std::string format_channel_row(size_t index, int16_t power, int16_t relative, bool qualified);
    static std::string format_slot_status(const NoaaSameDecoderSlotStatus& slot);
    static uint8_t clamp_channel(int32_t value);
};

}  // namespace ui::external_app::noaa_same

#endif /*__UI_NOAA_SAME_H__*/
