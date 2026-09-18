//--------------------------------------------------------------
//-- Oscillator.pde
//-- Generate sinusoidal oscillations in the servos
//--------------------------------------------------------------
//-- (c) Juan Gonzalez-Gomez (Obijuan), Dec 2011
//-- (c) txp666 for esp32, 202503
//-- GPL license
//--------------------------------------------------------------
#include "oscillator.h"

#include <driver/ledc.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>

static const char* TAG = "Oscillator";

extern unsigned long IRAM_ATTR millis();

Oscillator::Oscillator(int trim) {
    trim_ = trim;
    diff_limit_ = 0;
    is_attached_ = false;

    sampling_period_ = 30;
    period_ = 2000;
    number_samples_ = period_ / sampling_period_;
    inc_ = 2 * M_PI / number_samples_;

    amplitude_ = 45;
    phase_ = 0;
    phase0_ = 0;
    offset_ = 0;
    stop_ = false;
    rev_ = false;

    pos_ = 90;
    previous_millis_ = 0;
}

Oscillator::~Oscillator() {
    Detach();
}

uint32_t Oscillator::AngleToCompare(int angle) {
    return (angle - SERVO_MIN_DEGREE) * (SERVO_MAX_PULSEWIDTH_US - SERVO_MIN_PULSEWIDTH_US) /
               (SERVO_MAX_DEGREE - SERVO_MIN_DEGREE) +
           SERVO_MIN_PULSEWIDTH_US;
}

bool Oscillator::NextSample() {
    current_millis_ = millis();

    if (current_millis_ - previous_millis_ > sampling_period_) {
        previous_millis_ = current_millis_;
        return true;
    }

    return false;
}

void Oscillator::Attach(int pin, bool rev, int channel) {
    // Same pin already driving — do not steal another LEDC channel / retune timer
    // (that made sibling hips twitch during live trim).
    if (is_attached_ && pin_ == pin) {
        rev_ = rev;
        return;
    }

    if (is_attached_) {
        Detach();
    }

    pin_ = pin;
    rev_ = rev;

    static bool timer_ready = false;
    if (!timer_ready) {
        ledc_timer_config_t ledc_timer = {.speed_mode = LEDC_LOW_SPEED_MODE,
                                          .duty_resolution = LEDC_TIMER_13_BIT,
                                          .timer_num = LEDC_TIMER_1,
                                          .freq_hz = 50,
                                          .clk_cfg = LEDC_AUTO_CLK};
        esp_err_t terr = ledc_timer_config(&ledc_timer);
        if (terr != ESP_OK) {
            ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(terr));
            return;
        }
        timer_ready = true;
    }

    // Stable channel per servo, but NEVER use LEDC_CHANNEL_0 — backlight / camera
    // XCLK on this board own channel 0; fighting it makes LEFT_LEG buzz forever.
    // Old pool used channels 1..7; map servo index 0..5 → LEDC 1..6.
    int ledc_ch = channel;
    if (ledc_ch < 0 || ledc_ch > 6) {
        static int last_channel = 0;
        last_channel = last_channel % 6 + 1;  // 1..6
        ledc_ch = last_channel;
    } else {
        ledc_ch = channel + 1;  // 0→1 … 5→6
    }
    ledc_channel_ = (ledc_channel_t)ledc_ch;

    ledc_channel_config_t ledc_channel = {.gpio_num = pin_,
                                          .speed_mode = LEDC_LOW_SPEED_MODE,
                                          .channel = ledc_channel_,
                                          .intr_type = LEDC_INTR_DISABLE,
                                          .timer_sel = LEDC_TIMER_1,
                                          .duty = 0,
                                          .hpoint = 0};
    esp_err_t cerr = ledc_channel_config(&ledc_channel);
    if (cerr != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config pin=%d ch=%d failed: %s", pin_, ledc_ch,
                 esp_err_to_name(cerr));
        return;
    }

    ledc_speed_mode_ = LEDC_LOW_SPEED_MODE;

    previous_servo_command_millis_ = millis();

    is_attached_ = true;
}

void Oscillator::Detach() {
    if (!is_attached_)
        return;

    // Do not abort the whole chip if LEDC stop fails (brownout / bad channel).
    esp_err_t err = ledc_stop(ledc_speed_mode_, ledc_channel_, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ledc_stop ch=%d failed: %s", (int)ledc_channel_, esp_err_to_name(err));
    }

    is_attached_ = false;
}

void Oscillator::SetT(unsigned int T) {
    period_ = T;

    number_samples_ = period_ / sampling_period_;
    inc_ = 2 * M_PI / number_samples_;
}

void Oscillator::SetPosition(int position) {
    Write(position);
}

void Oscillator::Refresh() {
    if (NextSample()) {
        if (!stop_) {
            int pos = std::round(amplitude_ * std::sin(phase_ + phase0_) + offset_);
            if (rev_)
                pos = -pos;
            Write(pos + 90);
        }

        phase_ = phase_ + inc_;
    }
}

void Oscillator::Write(int position) {
    if (!is_attached_)
        return;

    long currentMillis = millis();
    if (diff_limit_ > 0) {
        int limit = std::max(
            1, (((int)(currentMillis - previous_servo_command_millis_)) * diff_limit_) / 1000);
        if (abs(position - pos_) > limit) {
            pos_ += position < pos_ ? -limit : limit;
        } else {
            pos_ = position;
        }
    } else {
        pos_ = position;
    }
    previous_servo_command_millis_ = currentMillis;

    int angle = pos_ + trim_;

    angle = std::min(std::max(angle, 0), 180);

    uint32_t duty = (uint32_t)(((angle / 180.0) * 2.0 + 0.5) * 8191 / 20.0);

    // Never abort here: a brief 5V sag while walking used to reboot via ESP_ERROR_CHECK.
    esp_err_t err = ledc_set_duty(ledc_speed_mode_, ledc_channel_, duty);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ledc_set_duty ch=%d failed: %s", (int)ledc_channel_, esp_err_to_name(err));
        return;
    }
    err = ledc_update_duty(ledc_speed_mode_, ledc_channel_);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ledc_update_duty ch=%d failed: %s", (int)ledc_channel_, esp_err_to_name(err));
    }
}
