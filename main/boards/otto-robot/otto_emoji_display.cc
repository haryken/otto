#include "otto_emoji_display.h"

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

void OttoEmojiDisplay::ConfigureChatLayout() {
    DisplayLockGuard lock(this);
    if (bottom_bar_ == nullptr || chat_message_label_ == nullptr) {
        return;
    }

    lv_obj_set_width(bottom_bar_, LV_HOR_RES);
    lv_obj_set_height(bottom_bar_, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lv_color_black(), 0);
    lv_obj_set_style_pad_top(bottom_bar_, 4, 0);
    lv_obj_set_style_pad_bottom(bottom_bar_, 4, 0);
    lv_obj_set_style_pad_left(bottom_bar_, 8, 0);
    lv_obj_set_style_pad_right(bottom_bar_, 8, 0);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_set_width(chat_message_label_, LV_HOR_RES - 16);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);

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
    SpiLcdDisplay::SetChatMessage(role, content);
    DisplayLockGuard lock(this);
    if (bottom_bar_ == nullptr) {
        return;
    }
    if (content != nullptr && content[0] != '\0' && !hide_subtitle_) {
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

LV_FONT_DECLARE(OTTO_ICON_FONT);
void OttoEmojiDisplay::SetEmotion(const char* emotion) {
    // Otto assets use GIF names (staticstate, happy, ...), not Font Awesome keys like "neutral".
    if (emotion != nullptr &&
        (strcmp(emotion, "neutral") == 0 || strcmp(emotion, "microchip_ai") == 0)) {
        emotion = "staticstate";
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
        lv_obj_set_style_text_font(status_label_, &OTTO_ICON_FONT, 0);
        lv_label_set_text(status_label_, "\xEF\x84\xB0");  // U+F130 麦克风图标
        lv_obj_clear_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(network_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    } else if (strcmp(status, Lang::Strings::SPEAKING) == 0) {
        lv_obj_set_style_text_font(status_label_, &OTTO_ICON_FONT, 0);
        lv_label_set_text(status_label_, "\xEF\x80\xA8");  // U+F028 说话图标
        lv_obj_clear_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(network_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    } else if (strcmp(status, Lang::Strings::CONNECTING) == 0) {
        lv_obj_set_style_text_font(status_label_, &OTTO_ICON_FONT, 0);
        lv_label_set_text(status_label_, "\xEF\x83\x81");  // U+F0c1 连接图标
        lv_obj_clear_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    } else if (strcmp(status, Lang::Strings::STANDBY) == 0) {
        lv_obj_set_style_text_font(status_label_, text_font, 0);
        lv_label_set_text(status_label_, "");
        lv_obj_clear_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(network_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_set_style_text_font(status_label_, text_font, 0);
    lv_label_set_text(status_label_, status);
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
        if (emoji_box_ != nullptr) {
            lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Recreate/start GIF emotion so eyes blink again after QR.
    SetEmotion("staticstate");
    ConfigureChatLayout();
    ESP_LOGI(TAG, "HideQrCode: restored emoji GIF");
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
    // 设置图片源并显示预览图片
    lv_image_set_src(preview_image_, img_dsc);
    lv_image_set_rotation(preview_image_, 900);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        // zoom factor 1.0
        lv_image_set_scale(preview_image_, 256 * width_ / img_dsc->header.w);
    }

    // Hide emoji_box_
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}
