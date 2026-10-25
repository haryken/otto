#include "otto_emoji_display.h"
#include "speak_lesson.h"

#include <esp_log.h>
#include <esp_heap_caps.h>

#include <cstring>

extern "C" {
#include "qrcodegen.h"
}

#include "application.h"
#include "assets/lang_config.h"
#include "display/lvgl_display/lvgl_image.h"
#include "display/lvgl_display/lvgl_theme.h"

#define TAG "OttoEmojiDisplay"

LV_FONT_DECLARE(OTTO_ICON_FONT);

static constexpr int kQrAutoHideMs = 20000;

OttoEmojiDisplay::OttoEmojiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
}

void OttoEmojiDisplay::SetupUI() {
    // Prevent duplicate calls - parent SetupUI() will also check, but check here for early return
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    // Otto GIF UI looks best on a black background (default NVS theme is "light"/white).
    auto* dark_theme = LvglThemeManager::GetInstance().GetTheme("dark");
    if (dark_theme != nullptr) {
        current_theme_ = dark_theme;
    }

    // Call parent SetupUI() first to create all lvgl objects
    SpiLcdDisplay::SetupUI();

    if (dark_theme != nullptr) {
        SetTheme(dark_theme);
    }
    
    // Setup preview image after UI is initialized - release lock before calling SetEmotion
    // to avoid deadlock (SetEmotion also acquires DisplayLockGuard internally)
    {
        DisplayLockGuard lock(this);
        lv_obj_set_size(preview_image_, width_ , height_ );
    }

    // Set default emotion after UI is initialized
    ConfigureEmojiLayout();
    ConfigureChatLayout();
    SetEmotion("staticstate");
}

void OttoEmojiDisplay::RaiseStatusChrome() {
    if (top_bar_ != nullptr) {
        lv_obj_move_foreground(top_bar_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_move_foreground(status_bar_);
    }
}

void OttoEmojiDisplay::ShowTalkOnBattery(const char* utf8_icon) {
    talk_icon_ = utf8_icon;
    talk_replaces_battery_ = utf8_icon != nullptr && utf8_icon[0] != '\0';
    if (battery_label_ == nullptr) {
        return;
    }
    lv_obj_set_style_text_font(battery_label_, &OTTO_ICON_FONT, 0);
    lv_label_set_text(battery_label_, talk_icon_ ? talk_icon_ : "");
    lv_obj_clear_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);
}

void OttoEmojiDisplay::RestoreBatteryIcon() {
    talk_replaces_battery_ = false;
    talk_icon_ = nullptr;
    if (battery_label_ == nullptr) {
        return;
    }
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    if (theme != nullptr && theme->icon_font() != nullptr) {
        lv_obj_set_style_text_font(battery_label_, theme->icon_font()->font(), 0);
    }
    lv_label_set_text(battery_label_, battery_icon_ ? battery_icon_ : "");
    lv_obj_clear_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);
}

void OttoEmojiDisplay::UpdateStatusBar(bool update_all) {
    LvglDisplay::UpdateStatusBar(update_all);
    if (!talk_replaces_battery_ || talk_icon_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    if (battery_label_ == nullptr) {
        return;
    }
    lv_obj_set_style_text_font(battery_label_, &OTTO_ICON_FONT, 0);
    lv_label_set_text(battery_label_, talk_icon_);
    lv_obj_clear_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);
}

bool OttoEmojiDisplay::WantSpeakSubtitle(const char* role) const {
    if (hide_subtitle_) {
        return false;
    }
    if (!SpeakLesson::Get().IsOn()) {
        return true;
    }
    if (role != nullptr && strcmp(role, "user") == 0) {
        return SpeakLesson::Get().ShowSttSubtitle();
    }
    if (role != nullptr && strcmp(role, "assistant") == 0) {
        return SpeakLesson::Get().ShowRobotSubtitle();
    }
    return true;
}

void OttoEmojiDisplay::RefreshChatOverlay() {
    SetChatMessage(last_chat_role_.c_str(), last_chat_content_.c_str());
}

bool OttoEmojiDisplay::KeepTeachingCardOnTop() {
    if (qr_visible_ || load_visible_ || !SpeakLesson::Get().IsOn() ||
        preview_image_cached_ == nullptr || preview_image_ == nullptr) {
        return false;
    }
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    if (emoji_box_ != nullptr) {
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(preview_image_);
    return true;
}

void OttoEmojiDisplay::ConfigureChatLayout() {
    DisplayLockGuard lock(this);
    if (bottom_bar_ == nullptr || chat_message_label_ == nullptr) {
        return;
    }

    const bool teach_card = SpeakLesson::Get().IsOn() && preview_image_cached_ != nullptr &&
                            preview_image_ != nullptr &&
                            !lv_obj_has_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(top_bar_, teach_card ? LV_OPA_0 : LV_OPA_50, 0);
    }

    lv_obj_set_width(bottom_bar_, LV_HOR_RES);
    lv_obj_set_height(bottom_bar_, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(bottom_bar_, teach_card ? LV_OPA_60 : LV_OPA_80, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lv_color_black(), 0);
    lv_obj_set_style_pad_top(bottom_bar_, 4, 0);
    lv_obj_set_style_pad_bottom(bottom_bar_, 4, 0);
    lv_obj_set_style_pad_left(bottom_bar_, 8, 0);
    lv_obj_set_style_pad_right(bottom_bar_, 8, 0);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_set_width(chat_message_label_, LV_HOR_RES - 16);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);

    if (load_visible_) {
        if (emoji_box_ != nullptr) {
            lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(emoji_box_);
        }
        if (load_overlay_ != nullptr) {
            lv_obj_remove_flag(load_overlay_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(load_overlay_);
        }
        RaiseStatusChrome();
        return;
    }

    if (qr_visible_) {
        if (emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
        if (qr_overlay_ != nullptr) {
            lv_obj_remove_flag(qr_overlay_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(qr_overlay_);
        }
        RaiseStatusChrome();
        return;
    }

    if (KeepTeachingCardOnTop()) {
        if (bottom_bar_ != nullptr && !lv_obj_has_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_move_foreground(bottom_bar_);
        }
        RaiseStatusChrome();
        return;
    }

    // emoji_box_ must stay above the opaque full-screen container_, not behind it.
    if (emoji_box_ != nullptr) {
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(emoji_box_);
    }
    if (bottom_bar_ != nullptr && !lv_obj_has_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(bottom_bar_);
    }
    RaiseStatusChrome();
}

void OttoEmojiDisplay::SetChatMessage(const char* role, const char* content) {
    if (role != nullptr) {
        last_chat_role_ = role;
    }
    last_chat_content_ = content ? content : "";
    const bool show = WantSpeakSubtitle(last_chat_role_.c_str());
    SpiLcdDisplay::SetChatMessage(role, show ? last_chat_content_.c_str() : "");
    DisplayLockGuard lock(this);
    if (bottom_bar_ == nullptr) {
        return;
    }
    if (show && !last_chat_content_.empty() && !hide_subtitle_) {
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    ConfigureChatLayout();
}

void OttoEmojiDisplay::ConfigureEmojiLayout() {
    DisplayLockGuard lock(this);
    if (emoji_box_ == nullptr || emoji_image_ == nullptr) {
        return;
    }
    // Parent uses LV_SIZE_CONTENT on emoji_box_ (~120px). SetEmotion scales GIF 2x to
    // fill 240px LCD; without a larger clip region LVGL crops the left/right of the face.
    lv_obj_set_size(emoji_box_, width_, height_);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_center(emoji_image_);
}

void OttoEmojiDisplay::SetupPreviewImage() {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        ESP_LOGW(TAG, "SetupPreviewImage called but preview_image_ is nullptr (UI not initialized yet)");
        return;
    }
    lv_obj_set_size(preview_image_, width_ , height_ );
}

void OttoEmojiDisplay::InitializeOttoEmojis() {
    ESP_LOGI(TAG, "Otto表情初始化将由Assets系统处理");
}

void OttoEmojiDisplay::SetEmotion(const char* emotion) {
    // Otto assets use GIF names (staticstate, happy, ...), not Font Awesome keys like "neutral".
    if (emotion != nullptr &&
        (strcmp(emotion, "neutral") == 0 || strcmp(emotion, "microchip_ai") == 0)) {
        emotion = "staticstate";
    }

    if (load_visible_) {
        SpiLcdDisplay::SetEmotion(emotion);
        ConfigureEmojiLayout();
        DisplayLockGuard lock(this);
        if (emoji_box_ != nullptr) {
            lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(emoji_box_);
        }
        if (load_overlay_ != nullptr) {
            lv_obj_remove_flag(load_overlay_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(load_overlay_);
        }
        RaiseStatusChrome();
        return;
    }

    // While QR is up, keep overlay on top and do not leave a stopped GIF visible.
    if (qr_visible_) {
        SpiLcdDisplay::SetEmotion(emotion);
        DisplayLockGuard lock(this);
        if (gif_controller_) {
            gif_controller_->Stop();
        }
        if (emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
        if (qr_overlay_ != nullptr) {
            lv_obj_remove_flag(qr_overlay_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(qr_overlay_);
        }
        RaiseStatusChrome();
        return;
    }

    {
        DisplayLockGuard lock(this);
        if (KeepTeachingCardOnTop()) {
            if (bottom_bar_ != nullptr && !lv_obj_has_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_move_foreground(bottom_bar_);
            }
            RaiseStatusChrome();
            return;
        }
    }

    SpiLcdDisplay::SetEmotion(emotion);
    ConfigureEmojiLayout();
    ConfigureChatLayout();
}

void OttoEmojiDisplay::SetStatus(const char* status) {
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    DisplayLockGuard lock(this);
    if (!status) {
        ESP_LOGE(TAG, "SetStatus: status is nullptr");
        return;
    }

    if (strcmp(status, Lang::Strings::LISTENING) == 0) {
        lv_label_set_text(status_label_, "");
        lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(network_label_, LV_OBJ_FLAG_HIDDEN);
        ShowTalkOnBattery("\xEF\x84\xB0");  // mic replaces battery
        return;
    } else if (strcmp(status, Lang::Strings::SPEAKING) == 0) {
        lv_label_set_text(status_label_, "");
        lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(network_label_, LV_OBJ_FLAG_HIDDEN);
        ShowTalkOnBattery("\xEF\x80\xA8");  // speaker replaces battery
        return;
    } else if (strcmp(status, Lang::Strings::CONNECTING) == 0) {
        lv_label_set_text(status_label_, "");
        lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        ShowTalkOnBattery("\xEF\x83\x81");
        return;
    } else if (strcmp(status, Lang::Strings::STANDBY) == 0) {
        RestoreBatteryIcon();
        lv_obj_set_style_text_font(status_label_, text_font, 0);
        lv_label_set_text(status_label_, "");
        lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(network_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    RestoreBatteryIcon();
    lv_obj_set_style_text_font(status_label_, text_font, 0);
    lv_label_set_text(status_label_, status);
    lv_obj_clear_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
}

void OttoEmojiDisplay::PaintCourseLoadingDots() {
    const int lit = (load_dot_ >= 3) ? 0 : static_cast<int>(load_dot_) + 1;
    static const char* kTail[] = { " .", " ..", " ...", "" };
    if (load_label_ != nullptr) {
        std::string line = load_base_text_.empty() ? "Đang tải khóa học" : load_base_text_;
        line += kTail[load_dot_ % 4];
        lv_label_set_text(load_label_, line.c_str());
    }
    for (int i = 0; i < 3; ++i) {
        if (load_dots_[i] == nullptr) {
            continue;
        }
        const bool on = i < lit;
        lv_obj_set_style_bg_opa(load_dots_[i], on ? LV_OPA_COVER : LV_OPA_30, 0);
        lv_obj_set_style_bg_color(load_dots_[i], lv_color_hex(on ? 0x5cf0e8 : 0x1aa7a1), 0);
    }
}

void OttoEmojiDisplay::TickCourseLoading() {
    // Called from lv_timer on the LVGL task — never from esp_timer (4KB stack overflow).
    if (!load_visible_ || load_overlay_ == nullptr) {
        return;
    }
    load_dot_ = static_cast<uint8_t>((load_dot_ + 1) % 4);
    PaintCourseLoadingDots();
}

void OttoEmojiDisplay::PauseCourseLoadingTicks() {
    DisplayLockGuard lock(this);
    if (load_lv_timer_ != nullptr) {
        lv_timer_pause(load_lv_timer_);
    }
}

void OttoEmojiDisplay::ShowCourseLoading(const char* text) {
    load_base_text_ = (text && text[0]) ? text : "Đang tải khóa học";
    load_dot_ = 0;
    {
        DisplayLockGuard lock(this);
        lv_obj_t* screen = lv_screen_active();
        if (screen == nullptr) {
            ESP_LOGW(TAG, "ShowCourseLoading: no screen");
            return;
        }

        if (load_overlay_ == nullptr) {
            load_overlay_ = lv_obj_create(screen);
            lv_obj_remove_style_all(load_overlay_);
            lv_obj_set_size(load_overlay_, width_, height_);
            lv_obj_set_style_bg_opa(load_overlay_, LV_OPA_TRANSP, 0);
            lv_obj_align(load_overlay_, LV_ALIGN_CENTER, 0, 0);
            lv_obj_clear_flag(load_overlay_, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(load_overlay_, LV_OBJ_FLAG_CLICKABLE);

            for (int i = 0; i < 3; ++i) {
                load_dots_[i] = lv_obj_create(load_overlay_);
                lv_obj_remove_style_all(load_dots_[i]);
                lv_obj_set_size(load_dots_[i], 14, 14);
                lv_obj_set_style_radius(load_dots_[i], LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_bg_color(load_dots_[i], lv_color_hex(0x1aa7a1), 0);
                lv_obj_set_style_bg_opa(load_dots_[i], LV_OPA_40, 0);
                lv_obj_align(load_dots_[i], LV_ALIGN_BOTTOM_MID, (i - 1) * 28, -72);
                lv_obj_clear_flag(load_dots_[i], LV_OBJ_FLAG_SCROLLABLE);
            }

            lv_obj_t* bar = lv_obj_create(load_overlay_);
            lv_obj_remove_style_all(bar);
            lv_obj_set_size(bar, width_, 46);
            lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_70, 0);
            lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

            load_label_ = lv_label_create(bar);
            lv_obj_set_width(load_label_, width_ - 16);
            lv_label_set_long_mode(load_label_, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(load_label_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(load_label_, lv_color_white(), 0);
            auto* theme = static_cast<LvglTheme*>(current_theme_);
            if (theme != nullptr && theme->text_font() != nullptr) {
                lv_obj_set_style_text_font(load_label_, theme->text_font()->font(), 0);
            }
            lv_obj_center(load_label_);
        }

        PaintCourseLoadingDots();
        if (preview_image_ != nullptr) {
            lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (gif_controller_) {
            gif_controller_->Stop();
        }
        if (emoji_box_ != nullptr) {
            lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(emoji_box_);
        }
        lv_obj_remove_flag(load_overlay_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(load_overlay_);
        load_visible_ = true;
        if (load_lv_timer_ == nullptr) {
            load_lv_timer_ = lv_timer_create(
                [](lv_timer_t* t) {
                    auto* self = static_cast<OttoEmojiDisplay*>(lv_timer_get_user_data(t));
                    if (self != nullptr) {
                        self->TickCourseLoading();
                    }
                },
                400, this);
        } else {
            lv_timer_resume(load_lv_timer_);
            lv_timer_reset(load_lv_timer_);
        }
    }
    ESP_LOGI(TAG, "ShowCourseLoading: %s", load_base_text_.c_str());
}

void OttoEmojiDisplay::HideCourseLoading() {
    if (!load_visible_ && (load_overlay_ == nullptr || lv_obj_has_flag(load_overlay_, LV_OBJ_FLAG_HIDDEN))) {
        return;
    }
    DisplayLockGuard lock(this);
    if (load_lv_timer_ != nullptr) {
        lv_timer_pause(load_lv_timer_);
    }
    if (load_overlay_ != nullptr) {
        lv_obj_add_flag(load_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
    load_visible_ = false;
    if (preview_image_ != nullptr && preview_image_cached_ != nullptr) {
        lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(preview_image_);
        RaiseStatusChrome();
        return;
    }
    if (emoji_box_ != nullptr) {
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        if (gif_controller_) {
            gif_controller_->Start();
        }
    }
    RaiseStatusChrome();
}

void OttoEmojiDisplay::ShowQrCode(const char* text) {
    if (text == nullptr || text[0] == '\0') {
        ESP_LOGW(TAG, "ShowQrCode: empty text");
        return;
    }

    constexpr int kMaxVersion = 6;
    uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(kMaxVersion)];
    uint8_t qrcode[qrcodegen_BUFFER_LEN_FOR_VERSION(kMaxVersion)];
    if (!qrcodegen_encodeText(text, temp, qrcode, qrcodegen_Ecc_MEDIUM, 1, kMaxVersion,
                              qrcodegen_Mask_AUTO, true)) {
        ESP_LOGE(TAG, "ShowQrCode: encode failed for %s", text);
        return;
    }

    const int modules = qrcodegen_getSize(qrcode);
    const int quiet = 2;
    const int grid = modules + quiet * 2;
    int scale = width_ / grid;
    if (scale < 1) {
        scale = 1;
    }
    // Keep QR a bit smaller than full screen for quiet margin.
    while (grid * scale > width_ - 16 && scale > 1) {
        --scale;
    }
    const int img = grid * scale;
    const size_t buf_size = static_cast<size_t>(img) * static_cast<size_t>(img) * 2;

    auto* pixels = static_cast<uint16_t*>(
        heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pixels == nullptr) {
        pixels = static_cast<uint16_t*>(heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (pixels == nullptr) {
        ESP_LOGE(TAG, "ShowQrCode: OOM %u bytes", (unsigned)buf_size);
        return;
    }

    constexpr uint16_t kWhite = 0xFFFF;
    constexpr uint16_t kBlack = 0x0000;
    for (size_t i = 0; i < buf_size / 2; ++i) {
        pixels[i] = kWhite;
    }
    for (int y = 0; y < modules; ++y) {
        for (int x = 0; x < modules; ++x) {
            if (!qrcodegen_getModule(qrcode, x, y)) {
                continue;
            }
            const int px0 = (x + quiet) * scale;
            const int py0 = (y + quiet) * scale;
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    pixels[(py0 + dy) * img + (px0 + dx)] = kBlack;
                }
            }
        }
    }

    DisplayLockGuard lock(this);
    if (load_visible_ && load_overlay_ != nullptr) {
        lv_obj_add_flag(load_overlay_, LV_OBJ_FLAG_HIDDEN);
        load_visible_ = false;
    }
    lv_obj_t* screen = lv_screen_active();
    if (screen == nullptr) {
        heap_caps_free(pixels);
        ESP_LOGE(TAG, "ShowQrCode: no active screen");
        return;
    }

    if (qr_overlay_ == nullptr) {
        qr_overlay_ = lv_obj_create(screen);
        lv_obj_remove_style_all(qr_overlay_);
        lv_obj_set_size(qr_overlay_, width_, height_);
        lv_obj_set_style_bg_color(qr_overlay_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(qr_overlay_, LV_OPA_COVER, 0);
        lv_obj_align(qr_overlay_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(qr_overlay_, LV_OBJ_FLAG_SCROLLABLE);

        qr_image_ = lv_image_create(qr_overlay_);
        lv_obj_center(qr_image_);
    }

    qr_image_holder_ = std::make_unique<LvglAllocatedImage>(
        pixels, buf_size, img, img, img * 2, LV_COLOR_FORMAT_RGB565);
    lv_image_set_src(qr_image_, qr_image_holder_->image_dsc());
    lv_image_set_scale(qr_image_, 256);
    lv_image_set_rotation(qr_image_, 0);

    if (gif_controller_) {
        gif_controller_->Stop();
    }
    if (emoji_box_ != nullptr) {
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }
    if (preview_image_ != nullptr) {
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(qr_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(qr_overlay_);
    RaiseStatusChrome();
    qr_visible_ = true;

    if (qr_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback =
                [](void* arg) {
                    auto* self = static_cast<OttoEmojiDisplay*>(arg);
                    Application::GetInstance().Schedule([self]() {
                        if (self != nullptr && self->qr_visible_) {
                            self->HideQrCode();
                        }
                    });
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "otto_qr_hide",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&args, &qr_timer_);
    }
    esp_timer_stop(qr_timer_);
    esp_timer_start_once(qr_timer_, kQrAutoHideMs * 1000ULL);

    ESP_LOGI(TAG, "ShowQrCode: %s (%d modules, %d px RGB565)", text, modules, img);
}

void OttoEmojiDisplay::HideQrCode() {
    if (qr_timer_ != nullptr) {
        esp_timer_stop(qr_timer_);
    }

    {
        DisplayLockGuard lock(this);
        if (qr_overlay_ != nullptr) {
            lv_obj_add_flag(qr_overlay_, LV_OBJ_FLAG_HIDDEN);
        }
        qr_image_holder_.reset();
        qr_visible_ = false;
        if (!KeepTeachingCardOnTop() && emoji_box_ != nullptr) {
            lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (!SpeakLesson::Get().IsOn() || preview_image_cached_ == nullptr) {
        SetEmotion("staticstate");
    }
    ConfigureChatLayout();
    ESP_LOGI(TAG, "HideQrCode: restored %s",
             (SpeakLesson::Get().IsOn() && preview_image_cached_) ? "teaching card" : "emoji GIF");
}

void OttoEmojiDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    if (qr_visible_) {
        // Don't let camera preview steal the QR screen.
        return;
    }
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        ESP_LOGE(TAG, "Preview image is not initialized");
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        if (gif_controller_) {
            gif_controller_->Start();
        }
        ConfigureChatLayout();
        return;
    }

    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    lv_image_set_src(preview_image_, img_dsc);
    // Camera frames are landscape; teaching cards are square 240x240 with baked text.
    // Keep square cards upright even after the unit finishes (on_=false).
    const bool square_card = img_dsc->header.w > 0 && img_dsc->header.w == img_dsc->header.h;
    lv_image_set_rotation(preview_image_, square_card ? 0 : 900);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        lv_image_set_scale(preview_image_, 256 * width_ / img_dsc->header.w);
    }

    if (load_visible_) {
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_box_ != nullptr) {
            lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(emoji_box_);
        }
        if (load_overlay_ != nullptr) {
            lv_obj_remove_flag(load_overlay_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(load_overlay_);
        }
        RaiseStatusChrome();
        return;
    }

    // Hide emoji_box_
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(preview_image_);
    if (bottom_bar_ != nullptr && !lv_obj_has_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(bottom_bar_);
    }
    RaiseStatusChrome();
    esp_timer_stop(preview_timer_);
    if (!SpeakLesson::Get().IsOn() && !square_card) {
        ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
    }
}
