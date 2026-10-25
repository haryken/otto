#pragma once

#include "display/lcd_display.h"

#include <memory>
#include <string>

/**
 * @brief Otto机器人GIF表情显示类
 * 继承SpiLcdDisplay，通过EmojiCollection添加GIF表情支持
 */
class OttoEmojiDisplay : public SpiLcdDisplay {
   public:
    /**
     * @brief 构造函数，参数与SpiLcdDisplay相同
     */
    OttoEmojiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy);

    virtual ~OttoEmojiDisplay() = default;
    virtual void SetStatus(const char* status) override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void ShowQrCode(const char* text) override;
    virtual void HideQrCode() override;
    /** Full-screen face text while JSON/JPEG fetch; hide when the card image lands. */
    void ShowCourseLoading(const char* text);
    void HideCourseLoading();
    virtual void SetupUI() override;
    virtual void RefreshChatOverlay() override;
    virtual void UpdateStatusBar(bool update_all = false) override;

   private:
    void InitializeOttoEmojis();
    void SetupPreviewImage();
    /** Full-screen emoji box so 2x scaled 120px GIFs are not clipped horizontally. */
    void ConfigureEmojiLayout();
    /** Keep subtitle at bottom; multiline text draws above the full-screen eye GIF. */
    void ConfigureChatLayout();
    void RaiseStatusChrome();
    void ShowTalkOnBattery(const char* utf8_icon);
    void RestoreBatteryIcon();
    bool WantSpeakSubtitle(const char* role) const;
    /** Keep the speaking-card JPEG on the face while TTS/emotion would restore the eye GIF. Caller holds the display lock. */
    bool KeepTeachingCardOnTop();

    lv_obj_t* qr_overlay_ = nullptr;
    lv_obj_t* qr_image_ = nullptr;
    lv_obj_t* load_overlay_ = nullptr;
    lv_obj_t* load_label_ = nullptr;
    std::unique_ptr<LvglImage> qr_image_holder_;
    esp_timer_handle_t qr_timer_ = nullptr;
    bool qr_visible_ = false;
    bool load_visible_ = false;
    bool talk_replaces_battery_ = false;
    const char* talk_icon_ = nullptr;
    std::string last_chat_role_;
    std::string last_chat_content_;
};
