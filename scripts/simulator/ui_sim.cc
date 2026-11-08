// Host-side UI simulator: replays the real firmware's display-call sequences
// (main/application.cc family-call paths) on the repo's own LVGL 9.5 source
// with an SDL2 window, so the reworked family-call UI can be reviewed without
// hardware. NOT compiled by idf.py; built by scripts/simulator/build_ui_sim.sh.
//
// Fidelity contract: every scene below mirrors the Display:: calls the real
// firmware makes on that path (SetStatus texts are the Lang::Strings values,
// chat texts are the real server TTS/STT payloads). See README.md for the
// scene -> firmware-path mapping table.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl.h"
#include "drivers/sdl/lv_sdl_window.h"
#include "draw/snapshot/lv_snapshot.h"

// Bundled CJK font (Source Han Sans 16px common set) so server TTS/STT
// Chinese payloads render for real; LVGL exports it from src/font/.
extern const lv_font_t lv_font_source_han_sans_sc_16_cjk;

#if LV_VERSION_CHECK(9, 5, 0)
#else
#error "This simulator is written against LVGL 9.5 (the repo's managed component)"
#endif

static lv_obj_t* status_label;
static lv_obj_t* notification_label;
static lv_obj_t* emotion_label;
static lv_obj_t* chat_assistant;
static lv_obj_t* chat_user;
static lv_obj_t* top_bar;

static void SetupUI() {
    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101014), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xE8E8EC), 0);
    lv_obj_set_style_text_font(screen, &lv_font_source_han_sans_sc_16_cjk, 0);

    // Column flex container, mirrors LcdDisplay::SetupUI (main/display/lcd_display.cc).
    lv_obj_t* container = lv_obj_create(screen);
    lv_obj_set_size(container, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_pad_row(container, 0, 0);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x101014), 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);

    // Layer 1: top bar (network left / battery right; icon font replaced by text).
    top_bar = lv_obj_create(container);
    lv_obj_set_size(top_bar, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar, 0, 0);
    lv_obj_set_style_border_width(top_bar, 0, 0);
    lv_obj_set_style_pad_all(top_bar, 4, 0);
    lv_obj_set_style_bg_opa(top_bar, LV_OPA_TRANSP, 0);
    lv_obj_t* net = lv_label_create(top_bar);
    lv_label_set_text(net, LV_SYMBOL_WIFI);
    lv_obj_align(net, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* bat = lv_label_create(top_bar);
    lv_label_set_text(bat, LV_SYMBOL_BATTERY_FULL);
    lv_obj_align(bat, LV_ALIGN_RIGHT_MID, 0, 0);

    // Layer 2: status bar — status_label and notification_label share the slot
    // and hide each other, exactly like lvgl_display.cc SetStatus/ShowNotification.
    lv_obj_t* status_bar = lv_obj_create(container);
    lv_obj_set_size(status_bar, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar, 0, 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_pad_all(status_bar, 6, 0);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_TRANSP, 0);
    status_label = lv_label_create(status_bar);
    lv_label_set_text(status_label, "Standby");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xB9B9C2), 0);
    lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_width(status_bar, LV_HOR_RES);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    notification_label = lv_label_create(status_bar);
    lv_label_set_text(notification_label, "");
    lv_obj_add_flag(notification_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(notification_label, lv_color_hex(0xFFC857), 0);

    // Layer 3: emotion area (real firmware renders emoji/fonts; text here).
    emotion_label = lv_label_create(container);
    lv_label_set_text(emotion_label, "");
    lv_obj_set_size(emotion_label, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(emotion_label, &lv_font_source_han_sans_sc_16_cjk, 0);
    lv_obj_set_flex_grow(emotion_label, 1);
    lv_obj_set_style_pad_top(emotion_label, 24, 0);
    lv_obj_set_style_text_align(emotion_label, LV_TEXT_ALIGN_CENTER, 0);

    // Layer 4: chat messages — assistant (server TTS) on top, user (STT) below,
    // role-colored like lcd_display.cc.
    chat_assistant = lv_label_create(container);
    lv_obj_set_size(chat_assistant, LV_HOR_RES - 16, LV_SIZE_CONTENT);
    lv_label_set_long_mode(chat_assistant, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_all(chat_assistant, 8, 0);
    lv_obj_set_style_radius(chat_assistant, 8, 0);
    lv_obj_set_style_bg_color(chat_assistant, lv_color_hex(0x2A2A33), 0);
    lv_obj_set_style_bg_opa(chat_assistant, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_bottom(chat_assistant, 12, 0);
    lv_label_set_text(chat_assistant, "");

    chat_user = lv_label_create(container);
    lv_obj_set_size(chat_user, LV_HOR_RES - 16, LV_SIZE_CONTENT);
    lv_label_set_long_mode(chat_user, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_all(chat_user, 8, 0);
    lv_obj_set_style_radius(chat_user, 8, 0);
    lv_obj_set_style_bg_color(chat_user, lv_color_hex(0x24322B), 0);
    lv_obj_set_style_bg_opa(chat_user, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_bottom(chat_user, 12, 0);
    lv_obj_set_style_text_color(chat_user, lv_color_hex(0x8BE9A8), 0);
    lv_label_set_text(chat_user, "");
}

static void SetStatus(const char* status) {
    lv_label_set_text(status_label, status);
    lv_obj_remove_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(notification_label, LV_OBJ_FLAG_HIDDEN);
}

static void ShowNotification(const char* notification) {
    lv_label_set_text(notification_label, notification);
    lv_obj_remove_flag(notification_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
}

static void SetEmotion(const char* emotion) {
    // Real firmware resolves emoji/font icons; the sim prints the emotion id.
    lv_label_set_text_fmt(emotion_label, "[ %s ]", emotion);
}

static void SetChatMessage(const char* role, const char* content) {
    lv_obj_t* target = chat_assistant;
    lv_color_t color = lv_color_hex(0xE8E8EC);
    if (strcmp(role, "user") == 0) { target = chat_user; color = lv_color_hex(0x8BE9A8); }
    else if (strcmp(role, "system") == 0) color = lv_color_hex(0xFFC857);
    lv_label_set_text(target, content);
    lv_obj_set_style_text_color(target, color, 0);
    // Empty message -> hide the bubble so scenes start clean.
    if (content[0] == '\0') lv_obj_add_flag(target, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(target, LV_OBJ_FLAG_HIDDEN);
}

static void Pump(int frames) {
    for (int i = 0; i < frames; ++i) {
        lv_tick_inc(10);
        lv_timer_handler();
        usleep(10000);
    }
}

static void SaveBmp(const char* path) {
    // lv_snapshot in ARGB8888 -> 24-bit BMP (rows bottom-up, BGR).
    lv_draw_buf_t* buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_ARGB8888);
    if (buf == nullptr || buf->data == nullptr) {
        fprintf(stderr, "snapshot failed\n");
        return;
    }
    uint32_t w = buf->header.w;
    uint32_t h = buf->header.h;
    uint32_t stride = buf->header.stride;
    uint8_t* px = buf->data;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "cannot open %s\n", path);
        lv_draw_buf_destroy(buf);
        return;
    }
    uint32_t data_size = w * 3 * h;
    uint32_t file_size = 54 + data_size;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &file_size, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, (uint32_t[]){h}, 4);
    uint16_t planes = 1, bpp = 24;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &data_size, 4);
    (void)!write(fd, hdr, 54);
    // LVGL ARGB8888 memory layout is B,G,R,A per pixel (little-endian 0xAARRGGBB).
    for (int row = (int)h - 1; row >= 0; --row) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = px + ((uint32_t)row * stride) + x * 4;
            uint8_t bgr[3] = {p[0], p[1], p[2]};  // B,G,R
            (void)!write(fd, bgr, 3);
        }
    }
    close(fd);
    lv_draw_buf_destroy(buf);
    printf("saved %s (%ux%u)\n", path, w, h);
}

// Scenes mirror the real firmware's display-call sequences:
//  idle    -> OnStateChanged(kDeviceStateIdle)         SetStatus(STANDBY)+SetEmotion(neutral)
//  incoming-> remote_wakeup + call_incoming + TTS      Connecting.. -> Listening.. -> assistant chat -> user reply
//  active  -> bridged call: alternating Speaking/Listening status
//  hangup  -> goodbye received: assistant chat + STANDBY + neutral
static void Scene(const char* name) {
    if (strcmp(name, "idle") == 0) {
        SetStatus("Standby");
        SetEmotion("neutral");
        SetChatMessage("assistant", "");
        SetChatMessage("user", "");
    } else if (strcmp(name, "incoming") == 0) {
        // RemoteWakeup opens the channel: Connecting -> Listening.
        SetStatus("Connecting...");
        Pump(30);
        SetStatus("Listening...");
        Pump(20);
        // Server TTS announces the call (announcement text verbatim, no LLM).
        SetChatMessage("assistant", "您收到来自 妈妈 的来电");
        SetEmotion("neutral");
        Pump(40);
        // User's spoken reply arrives via STT; server bridges on accept.
        SetChatMessage("user", "嗯，接听");
        Pump(30);
    } else if (strcmp(name, "active") == 0) {
        SetStatus("Speaking...");
        Pump(30);
        SetStatus("Listening...");
        Pump(30);
    } else if (strcmp(name, "hangup") == 0) {
        // Goodbye from the peer: chat message then back to standby.
        SetChatMessage("assistant", "对方已挂断");
        Pump(30);
        SetStatus("Standby");
        SetEmotion("neutral");
        Pump(20);
    } else {
        fprintf(stderr, "unknown scene: %s (idle|incoming|active|hangup)\n", name);
        exit(2);
    }
}

int main(int argc, char** argv) {
    const char* scene = "idle";
    const char* shot = nullptr;
    bool headless = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc) scene = argv[++i];
        else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (strcmp(argv[i], "--headless") == 0) headless = true;
        else if (strcmp(argv[i], "--all") == 0) { scene = "__all__"; }
    }
    if (headless) setenv("SDL_VIDEODRIVER", "dummy", 1);

    lv_init();
    lv_display_t* disp = lv_sdl_window_create(240, 240);
    fprintf(stderr, "diag: disp=%p screen=%p size=%dx%d\n", (void*)disp,
            (void*)lv_screen_active(),
            disp ? (int)lv_display_get_horizontal_resolution(disp) : -1,
            disp ? (int)lv_display_get_vertical_resolution(disp) : -1);
    lv_tick_set_cb([]() -> uint32_t { static uint32_t t = 0; t += 10; return t; });
    SetupUI();

    if (strcmp(scene, "__all__") == 0) {
        const char* scenes[] = {"idle", "incoming", "active", "hangup"};
        char path[256];
        for (const char* s : scenes) {
            Scene(s);
            Pump(10);
            if (shot) {
                snprintf(path, sizeof(path), "%s-%s.bmp", shot, s);
                SaveBmp(path);
            }
        }
    } else {
        Scene(scene);
        Pump(10);
        if (shot) SaveBmp(shot);
    }
    printf("UI_SIM_OK scene=%s\n", scene);
    return 0;
}
