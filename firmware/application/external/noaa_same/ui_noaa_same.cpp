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

#include "ui_noaa_same.hpp"

#include "audio.hpp"
#include "baseband_api.hpp"
#include "file.hpp"
#include "portapack.hpp"
#include "portapack_shared_memory.hpp"
#include "string_format.hpp"

#include <algorithm>

using namespace portapack;

namespace ui::external_app::noaa_same {

namespace {

constexpr std::array<rf::Frequency, NoaaSameStatusMessage::channel_count> noaa_frequencies{{
    162400000,
    162425000,
    162450000,
    162475000,
    162500000,
    162525000,
    162550000,
}};

int parse_digit(char c) {
    return (c >= '0' && c <= '9') ? c - '0' : -1;
}

std::filesystem::path noaa_dir() {
    return {u"/NOAA"};
}

std::filesystem::path presets_path() {
    return {u"/NOAA/PRESETS.TXT"};
}

}  // namespace

NoaaSameView::NoaaSameView(NavigationView& nav)
    : nav_{nav} {
    audio::output::stop();
    shared_memory.request_m4_performance_counter = 1;

    load_presets();
    if (startup_preset > 0 && startup_preset <= presets.size())
        decoder_channels = presets[startup_preset - 1].channels;

    baseband::run_image(portapack::spi_flash::image_tag_noaa_same);
    configure_receiver();

    add_children({
        &labels,
        &decoder_fields[0],
        &decoder_fields[1],
        &decoder_fields[2],
        &decoder_status[0],
        &decoder_status[1],
        &decoder_status[2],
        &preset_field,
        &startup_field,
        &checkbox_audio,
        &checkbox_force,
        &button_rescan,
        &button_apply,
        &button_load,
        &button_save,
        &text_status,
        &text_diag,
    });
    for (auto& row : channel_rows)
        add_child(&row);

    for (size_t i = 0; i < decoder_fields.size(); ++i) {
        decoder_fields[i].set_options(channel_options());
        decoder_fields[i].on_change = [this, i](size_t, OptionsField::value_t v) {
            set_decoder_from_field(i, clamp_channel(v));
        };
    }

    preset_field.set_selected_index(selected_preset > 0 && selected_preset <= 3 ? selected_preset - 1 : 0, false);
    preset_field.on_change = [this](size_t, OptionsField::value_t v) {
        selected_preset = static_cast<uint8_t>(v);
    };

    startup_field.set_by_value(startup_preset);
    startup_field.on_change = [this](size_t, OptionsField::value_t v) {
        startup_preset = static_cast<uint8_t>(v);
        text_status.set("Startup saved");
    };

    checkbox_audio.set_value(false);
    checkbox_audio.on_select = [this](Checkbox&, bool v) {
        audio_enabled = v;
        if (audio_enabled) {
            audio::set_rate(audio::Rate::Hz_24000);
            audio::output::start();
            receiver_model.set_headphone_volume(receiver_model.headphone_volume());
        } else {
            audio::output::stop();
        }
        send_config(false);
    };

    checkbox_force.set_value(false);
    checkbox_force.on_select = [this](Checkbox&, bool v) {
        immediate_override = v;
    };

    button_rescan.on_select = [this](Button&) {
        start_recommendation_scan();
    };

    button_apply.on_select = [this](Button&) {
        decoder_channels = pending_recommendation;
        refresh_decoder_fields();
        send_config(immediate_override);
        text_status.set("Top 3 applied");
    };

    button_load.on_select = [this](Button&) {
        load_selected_preset();
    };

    button_save.on_select = [this](Button&) {
        save_selected_preset();
    };

    signal_token_tick_second = rtc_time::signal_tick_second += [this]() {
        on_tick_second();
    };

    refresh_decoder_fields();
    refresh_ui();
    send_config(false);
}

NoaaSameView::~NoaaSameView() {
    rtc_time::signal_tick_second -= signal_token_tick_second;
    shared_memory.request_m4_performance_counter = 0;
    audio::output::stop();
    receiver_model.disable();
    baseband::shutdown();
}

void NoaaSameView::focus() {
    button_rescan.focus();
}

void NoaaSameView::configure_receiver() {
    receiver_model.set_target_frequency(center_frequency);
    receiver_model.set_sampling_rate(receiver_sample_rate);
    receiver_model.set_baseband_bandwidth(receiver_bandwidth);
    receiver_model.enable();
}

void NoaaSameView::send_config(bool immediate) {
    std::array<uint32_t, max_decoders> frequencies{};
    for (size_t i = 0; i < decoder_channels.size(); ++i) {
        if (decoder_channels[i] < channel_count)
            frequencies[i] = noaa_frequencies[decoder_channels[i]];
    }

    baseband::set_noaa_same_config(frequencies, audio_enabled, 0, immediate);
}

OptionsField::options_t NoaaSameView::channel_options() {
    return {
        {"Off", channel_off},
        {"162.400", 0},
        {"162.425", 1},
        {"162.450", 2},
        {"162.475", 3},
        {"162.500", 4},
        {"162.525", 5},
        {"162.550", 6},
    };
}

std::string NoaaSameView::channel_name(uint8_t channel) {
    if (channel >= channel_count)
        return "Off";
    return to_string_short_freq(noaa_frequencies[channel]);
}

uint8_t NoaaSameView::clamp_channel(int32_t value) {
    return (value >= 0 && value < static_cast<int32_t>(channel_count))
               ? static_cast<uint8_t>(value)
               : channel_off;
}

std::string NoaaSameView::format_channel_row(size_t index, int16_t power, int16_t relative, bool qualified) {
    return channel_name(static_cast<uint8_t>(index)) + " " +
           to_string_dec_int(power, 4) + " " +
           to_string_dec_int(relative, 3) + " " +
           (qualified ? "Y" : "-");
}

std::string NoaaSameView::format_slot_status(const NoaaSameDecoderSlotStatus& slot) {
    if (!slot.active)
        return "Off";
    return std::string(slot.same_busy ? "Busy " : "Ready") + " " + channel_name(slot.channel);
}

void NoaaSameView::set_decoder_from_field(size_t slot, uint8_t channel) {
    if (slot >= decoder_channels.size())
        return;

    decoder_channels[slot] = channel;
    send_config(immediate_override);
    text_status.set("Slot changed");
}

void NoaaSameView::refresh_decoder_fields() {
    for (size_t i = 0; i < decoder_fields.size(); ++i) {
        const auto channel = decoder_channels[i];
        const size_t option_index = channel < channel_count ? channel + 1 : 0;
        decoder_fields[i].set_selected_index(option_index, false);
    }
}

void NoaaSameView::refresh_ui() {
    for (size_t i = 0; i < channel_rows.size(); ++i)
        channel_rows[i].set(format_channel_row(i, channel_power[i], channel_relative[i], channel_qualified[i]));

    rx_sat_percent = static_cast<uint8_t>((static_cast<uint32_t>(shared_memory.m4_performance_counter) * 100) / 127);
    text_diag.set("CPU " + to_string_dec_uint(rx_sat_percent, 3) +
                  "% act " + to_string_dec_uint(active_decoders) +
                  " miss " + to_string_dec_uint(deadline_misses));
}

void NoaaSameView::on_status(const NoaaSameStatusMessage& message) {
    update_count = message.update_count;
    baseband_blocks = message.baseband_blocks;
    deadline_misses = message.deadline_misses;
    active_decoders = message.active_decoders;

    for (size_t i = 0; i < channel_count; ++i) {
        channel_power[i] = message.channels[i].power_db;
        channel_relative[i] = message.channels[i].relative_db;
        channel_qualified[i] = message.channels[i].qualified;
        if (scanning)
            scan_power[i] = std::max(scan_power[i], channel_power[i]);
    }

    for (size_t i = 0; i < max_decoders; ++i)
        decoder_status[i].set(format_slot_status(message.decoders[i]));

    refresh_ui();
}

void NoaaSameView::start_recommendation_scan() {
    scanning = true;
    scan_seconds_remaining = 2;
    scan_power.fill(-120);
    text_status.set("Scanning 2s...");
}

void NoaaSameView::finish_recommendation_scan() {
    scanning = false;

    std::array<uint8_t, channel_count> indexes{{0, 1, 2, 3, 4, 5, 6}};
    std::sort(indexes.begin(), indexes.end(), [this](uint8_t a, uint8_t b) {
        return scan_power[a] > scan_power[b];
    });

    pending_recommendation.fill(channel_off);
    size_t out = 0;
    const int16_t noise = scan_power[indexes.back()];
    for (auto index : indexes) {
        if (out >= max_decoders)
            break;
        if (scan_power[index] - noise >= 6)
            pending_recommendation[out++] = index;
    }

    std::string summary{"Top:"};
    for (auto channel : pending_recommendation)
        summary += " " + channel_name(channel);
    text_status.set(summary);
}

void NoaaSameView::on_tick_second() {
    if (scanning && scan_seconds_remaining > 0) {
        scan_seconds_remaining--;
        if (scan_seconds_remaining == 0)
            finish_recommendation_scan();
        else
            text_status.set("Scanning " + to_string_dec_uint(scan_seconds_remaining) + "s...");
    }
    refresh_ui();
}

void NoaaSameView::load_selected_preset() {
    if (selected_preset == 0 || selected_preset > presets.size())
        return;

    decoder_channels = presets[selected_preset - 1].channels;
    refresh_decoder_fields();
    send_config(immediate_override);
    text_status.set("Loaded " + presets[selected_preset - 1].name);
}

void NoaaSameView::save_selected_preset() {
    if (selected_preset == 0 || selected_preset > presets.size())
        return;

    presets[selected_preset - 1].channels = decoder_channels;
    save_presets();
    text_status.set("Saved " + presets[selected_preset - 1].name);
}

void NoaaSameView::load_presets() {
    auto result = File::read_file(presets_path());
    if (!result)
        return;

    const auto& text = result.value();
    size_t line_start = 0;
    while (line_start < text.size()) {
        const auto line_end = text.find('\n', line_start);
        const auto line = text.substr(line_start, line_end == std::string::npos ? std::string::npos : line_end - line_start);
        line_start = line_end == std::string::npos ? text.size() : line_end + 1;

        if (line.size() < 8 || line[0] != 'P')
            continue;

        const int preset_number = parse_digit(line[1]);
        if (preset_number < 1 || preset_number > 3)
            continue;

        auto& preset = presets[preset_number - 1];
        preset.name = line.substr(0, 2);
        for (size_t i = 0; i < max_decoders; ++i) {
            const size_t pos = 3 + (i * 2);
            preset.channels[i] = pos < line.size() ? clamp_channel(parse_digit(line[pos])) : channel_off;
        }
    }
}

void NoaaSameView::save_presets() {
    ensure_directory(noaa_dir());

    File file{};
    auto error = file.create(presets_path());
    if (error) {
        text_status.set("Preset save error");
        return;
    }

    for (const auto& preset : presets) {
        std::string line = preset.name + "=";
        for (size_t i = 0; i < max_decoders; ++i) {
            if (i)
                line += ",";
            line += (preset.channels[i] < channel_count
                         ? to_string_dec_uint(preset.channels[i])
                         : "x");
        }
        file.write_line(line);
    }
}

}  // namespace ui::external_app::noaa_same
