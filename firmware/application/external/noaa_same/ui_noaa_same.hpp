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
#include "recent_entries.hpp"
#include "ui.hpp"
#include "ui_navigation.hpp"
#include "ui_receiver.hpp"
#include "ui_widget.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace ui::external_app::noaa_same {

struct NoaaChannelRecentEntry {
    using Key = uint8_t;

    static constexpr Key invalid_key = 0xff;

    uint8_t channel{invalid_key};
    int16_t power_db{-120};
    int16_t power_30s_db{-120};
    int16_t relative_db{0};
    bool qualified{false};
    uint8_t decoder_slot{invalid_key};
    bool audio_source{false};

    explicit NoaaChannelRecentEntry(uint8_t channel)
        : channel{channel} {
    }

    Key key() const {
        return channel;
    }
};

using NoaaChannelRecentEntries = RecentEntries<NoaaChannelRecentEntry>;

class NoaaSameView : public View {
   public:
    explicit NoaaSameView(NavigationView& nav);
    ~NoaaSameView();

    void focus() override;
    std::string title() const override { return "NOAA SAME"; }
    static std::string channel_name(uint8_t channel);

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
    std::array<int16_t, channel_count> channel_power_30s{};
    std::array<bool, channel_count> channel_qualified{};
    std::array<int16_t, channel_count> scan_power{};
    std::array<bool, channel_count> scan_qualified{};
    std::array<Preset, 3> presets{{
        {"P1", {{3, 4, 5}}},
        {"P2", {{0, 1, 2}}},
        {"P3", {{4, 5, 6}}},
    }};

    bool audio_enabled{false};
    uint8_t audio_slot{0};
    bool scanning{false};
    uint8_t scan_seconds_remaining{0};
    uint32_t update_count{0};
    uint32_t baseband_blocks{0};
    uint8_t active_decoders{0};
    uint8_t rx_sat_percent{0};
    uint8_t seconds_until_sort{0};
    uint32_t rx_overruns{0};
    SignalToken signal_token_tick_second{};

    app_settings::SettingsManager settings_{
        "rx_noaa_same",
        app_settings::Mode::RX,
        {
            {"startup_preset"sv, &startup_preset},
            {"selected_preset"sv, &selected_preset},
        }};

    RecentEntriesColumns columns{
        {{"Ch", 7},
         {"Now", 4},
         {"30s", 4},
         {"Dec", 4}}};
    NoaaChannelRecentEntries recent{};
    RecentEntriesView<NoaaChannelRecentEntries> recent_entries_view{columns, recent};

    Labels labels{{
        {{UI_POS_X(0), UI_POS_Y(0)}, "LNA:   VGA:   AMP:   Vol:", Theme::getInstance()->fg_light->foreground},
        {{0, 152}, "Weather Channels", Theme::getInstance()->fg_light->foreground},
        {{0, 184}, "Preset:", Theme::getInstance()->fg_light->foreground},
    }};

    LNAGainField field_lna{
        {UI_POS_X(4), UI_POS_Y(0)}};

    VGAGainField field_vga{
        {UI_POS_X(11), UI_POS_Y(0)}};

    RFAmpField field_rf_amp{
        {UI_POS_X(18), UI_POS_Y(0)}};

    RSSI rssi{
        {UI_POS_X(20), 4, UI_POS_WIDTH_REMAINING(23), 8},
    };

    AudioVolumeField field_volume{
        {UI_POS_X(26), UI_POS_Y(0)}};

    OptionsField preset_field{
        {64, 184},
        6,
        {{"P1", 1}, {"P2", 2}, {"P3", 3}}};

    OptionsField startup_field{
        {128, 184},
        10,
        {{"Manual", 0}, {"P1", 1}, {"P2", 2}, {"P3", 3}}};

    Checkbox checkbox_audio{
        {0, 216},
        5,
        "Audio",
        true};

    Button button_rescan{
        {0, 248, 72, 24},
        "Rescan"};

    Button button_apply{
        {80, 248, 72, 24},
        "Apply"};

    Button button_load{
        {160, 248, 72, 24},
        "Load"};

    Button button_save{
        {0, 280, 72, 24},
        "Save"};

    Text text_status{
        {80, 216, 160, 16},
        ""};

    Text text_diag{
        {80, 280, 160, 16},
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
    void sort_recent_entries();
    void toggle_channel(uint8_t channel);
    void release_channel(uint8_t channel);
    bool assign_channel(uint8_t channel);
    int8_t slot_for_channel(uint8_t channel) const;
    void sanitize_audio_slot();

    static uint8_t clamp_channel(int32_t value);
};

}  // namespace ui::external_app::noaa_same

#endif /*__UI_NOAA_SAME_H__*/
