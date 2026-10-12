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

#include "external_app.hpp"
#include "ui_navigation.hpp"
#include "ui_noaa_same.hpp"

namespace ui::external_app::noaa_same {
void initialize_app(ui::NavigationView& nav) {
    nav.push<NoaaSameView>();
}
}  // namespace ui::external_app::noaa_same

extern "C" {

__attribute__((section(".external_app.app_noaa_same.application_information"), used)) application_information_t _application_information_noaa_same = {
    /*.memory_location = */ (uint8_t*)0x00000000,
    /*.externalAppEntry = */ ui::external_app::noaa_same::initialize_app,
    /*.header_version = */ CURRENT_HEADER_VERSION,
    /*.app_version = */ VERSION_MD5,

    /*.app_name = */ "NOAA SAME",
    /*.bitmap_data = */ {
        0x3C,
        0x00,
        0x42,
        0x00,
        0x99,
        0x00,
        0xA5,
        0x00,
        0xA5,
        0x00,
        0x99,
        0x00,
        0x42,
        0x00,
        0x3C,
        0x00,
        0x18,
        0x00,
        0x3C,
        0x00,
        0x7E,
        0x00,
        0x18,
        0x00,
        0x18,
        0x00,
        0x18,
        0x00,
        0x18,
        0x00,
        0x18,
        0x00,
    },
    /*.icon_color = */ ui::Color::yellow().v,
    /*.menu_location = */ app_location_t::RX,
    /*.desired_menu_position = */ -1,

    /*.m4_app_tag = portapack::spi_flash::image_tag_noaa_same */ {'P', 'N', 'O', 'S'},
    /*.m4_app_offset = */ 0x00000000,
};
}
