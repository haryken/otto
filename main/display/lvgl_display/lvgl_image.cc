#include "lvgl_image.h"
#include <cbin_font.h>

#include "sdkconfig.h"
#ifndef CONFIG_IDF_TARGET_ESP32
#include "jpeg_to_image.h"
#endif

#include <esp_log.h>
#include <stdexcept>
#include <cstring>
#include <esp_heap_caps.h>

#define TAG "LvglImage"


LvglRawImage::LvglRawImage(void* data, size_t size) {
    bzero(&image_dsc_, sizeof(image_dsc_));
    image_dsc_.data_size = size;
    image_dsc_.data = static_cast<uint8_t*>(data);
    image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_dsc_.header.cf = LV_COLOR_FORMAT_RAW_ALPHA;
    image_dsc_.header.w = 0;
    image_dsc_.header.h = 0;
}

bool LvglRawImage::IsGif() const {
    auto ptr = (const uint8_t*)image_dsc_.data;
    return ptr[0] == 'G' && ptr[1] == 'I' && ptr[2] == 'F';
}

LvglCBinImage::LvglCBinImage(void* data) {
    image_dsc_ = cbin_img_dsc_create(static_cast<uint8_t*>(data));
}

LvglCBinImage::~LvglCBinImage() {
    if (image_dsc_ != nullptr) {
        cbin_img_dsc_delete(image_dsc_);
    }
}

LvglAllocatedImage::LvglAllocatedImage(void* data, size_t size) {
    bzero(&image_dsc_, sizeof(image_dsc_));
    image_dsc_.data_size = size;
    image_dsc_.data = static_cast<uint8_t*>(data);

    if (lv_image_decoder_get_info(&image_dsc_, &image_dsc_.header) == LV_RESULT_OK) {
        return;
    }

    // Otto (and most boards) ship without LV_USE_TJPGD. Decode baseline JPEG to RGB565
    // so LAN teaching cards and MCP preview_image can show on the LCD.
#ifndef CONFIG_IDF_TARGET_ESP32
    const auto* src = static_cast<const uint8_t*>(data);
    if (src != nullptr && size >= 2 && src[0] == 0xFF && src[1] == 0xD8) {
        uint8_t* rgb = nullptr;
        size_t rgb_len = 0;
        size_t width = 0;
        size_t height = 0;
        size_t stride = 0;
        if (jpeg_to_image(src, size, &rgb, &rgb_len, &width, &height, &stride) == ESP_OK &&
            rgb != nullptr && width > 0 && height > 0 && rgb_len > 0) {
            heap_caps_free(data);
            image_dsc_.data = rgb;
            image_dsc_.data_size = rgb_len;
            image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
            image_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
            image_dsc_.header.w = static_cast<uint32_t>(width);
            image_dsc_.header.h = static_cast<uint32_t>(height);
            image_dsc_.header.stride = static_cast<uint32_t>(stride);
            ESP_LOGI(TAG, "JPEG decoded %ux%u stride=%u", (unsigned)width, (unsigned)height,
                     (unsigned)stride);
            return;
        }
        if (rgb != nullptr) {
            heap_caps_free(rgb);
        }
    }
#endif

    ESP_LOGE(TAG, "Failed to get image info, data: %p size: %u", data, (unsigned)size);
    heap_caps_free(data);
    image_dsc_.data = nullptr;
    throw std::runtime_error("Failed to get image info");
}

LvglAllocatedImage::LvglAllocatedImage(void* data, size_t size, int width, int height, int stride, int color_format) {
    bzero(&image_dsc_, sizeof(image_dsc_));
    image_dsc_.data_size = size;
    image_dsc_.data = static_cast<uint8_t*>(data);
    image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_dsc_.header.cf = color_format;
    image_dsc_.header.w = width;
    image_dsc_.header.h = height;
    image_dsc_.header.stride = stride;
}

LvglAllocatedImage::~LvglAllocatedImage() {
    if (image_dsc_.data) {
        heap_caps_free((void*)image_dsc_.data);
        image_dsc_.data = nullptr;
    }
}