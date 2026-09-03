#pragma once

#include "display/lcd_display.h"

/**
 * @brief Otto face: squircle cyan eyes (LVGL); sad/angry = tam giác bo góc trên canvas + blink.
 */
class OttoEmojiDisplay : public SpiLcdDisplay {
public:
    OttoEmojiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                     int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                     bool swap_xy);

    virtual ~OttoEmojiDisplay();

    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetTheme(Theme* theme) override;

private:
    void SetupGifContainer();
    void CreateRobotEyes();
    void ApplyEmotionStyle(const char* emotion);
    void StartBlinkTimer();
    void DoBlinkClose();
    void StartLookTimer();
    void StopLookTimer();
    /** Căn `eyes_row_` với `look_offset_x_` (dao động nhìn trái/phải). */
    void SyncEyesAlign();

    static void OnBlinkTimer(lv_timer_t* timer);
    /** Sau khung “nửa nhắm” (pill), kéo slot xuống mức đóng. */
    static void OnBlinkToClosed(lv_timer_t* timer);
    static void OnReopenAfterBlink(lv_timer_t* timer);
    static void OnLookTimer(lv_timer_t* timer);

    lv_timer_t* blink_timer_ = nullptr;
    lv_timer_t* look_timer_ = nullptr;
    /** Chỉ số bước trong sóng tam giác (nhìn trái/phải). */
    uint8_t look_phase_ = 0;
    lv_coord_t look_offset_x_ = 0;

    char last_emotion_[32] = "neutral";

    /** Vùng giữa (flex grow): chỉ căn mắt ở đây, tránh chữ chat đè lên mắt. */
    lv_obj_t* face_area_ = nullptr;
    lv_obj_t* eyes_row_ = nullptr;

    /** Slots: rect mặc định, hoặc canvas tam giác (buồn / giận dùng chung buffer). */
    lv_obj_t* eye_left_ = nullptr;
    lv_obj_t* eye_right_ = nullptr;
    lv_obj_t* eye_left_rect_ = nullptr;
    lv_obj_t* eye_right_rect_ = nullptr;
    lv_obj_t* eye_left_sad_ = nullptr;
    lv_obj_t* eye_right_sad_ = nullptr;
};
