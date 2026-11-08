// Host LVGL config for the UI simulator only (not used by idf.py builds).
// Mirrors the settings that matter for the 240x240 TFT UI; LVGL defaults
// cover the rest.
#pragma once
#ifndef LV_CONF_H
#define LV_CONF_H
#define LV_COLOR_DEPTH 16
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_OS LV_OS_PTHREAD
#define LV_USE_LOG 0
#define LV_USE_SDL 1
#define LV_USE_SNAPSHOT 1
#define LV_USE_THEME_DEFAULT 0
#define LV_THEME_DEFAULT_DARK 1
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_SOURCE_HAN_SANS_SC_16_CJK 1
#endif
