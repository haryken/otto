#include "otto_emoji_display.h"
#include "config.h"
#include "lvgl_theme.h"

#include <esp_log.h>
#include <esp_random.h>

#include <cmath>
#include <cstring>

#include "display/lcd_display.h"

LV_DRAW_BUF_DEFINE_STATIC(sad_eye_buf_l, 120, 120, LV_COLOR_FORMAT_RGB565);
LV_DRAW_BUF_DEFINE_STATIC(sad_eye_buf_r, 120, 120, LV_COLOR_FORMAT_RGB565);

extern "C" {
#include "src/draw/lv_draw_triangle.h"
#include "src/misc/lv_area.h"
}

#define TAG "OttoEmojiDisplay"

namespace {

/** Mắt vuông bo squircle giống emoji; `face_area_` tách khỏi chữ. */
constexpr int kEyeSize = 94;
/**
 * Hàng mắt cố tình hẹp hơn DISPLAY_WIDTH: khi dịch “nhìn trái/phải” (±8px) vẫn nằm trong khung,
 * tránh tràn ngang → LVGL hiện scrollbar.
 */
constexpr int kEyesGap = 8;
/** Dịch hàng mắt lên so với giữa `face_area_` (y < 0 = lên). */
constexpr lv_coord_t kEyesRowOffsetY = -14;
constexpr int kEyeSlotSize = (DISPLAY_WIDTH - kEyesGap - 16) / 2;
constexpr int kSadCanvasSize = 102;
/** Pill nháy cùng tỉ lệ với mắt. */
constexpr int kBlinkPillW = 100;
constexpr int kBlinkPillH = 33;

void StyleSingleEye(lv_obj_t* eye, lv_coord_t w, lv_coord_t h, lv_color_t fill, lv_opa_t fill_opa,
                    lv_color_t glow) {
    lv_obj_set_size(eye, w, h);
    /* Squircle kiểu emoji: bo góc lớn hơn (tỉ lệ % cạnh ngắn, có clamp). */
    lv_coord_t short_side = (w < h ? w : h);
    lv_coord_t r = short_side * 36 / 100;
    const lv_coord_t max_r = short_side / 2 - 1;
    if (r > max_r) {
        r = max_r;
    }
    if (r < 16) {
        r = 16;
    }
    lv_obj_set_style_radius(eye, r, 0);
    lv_obj_set_style_pad_all(eye, 0, 0);
    lv_obj_set_style_bg_color(eye, fill, 0);
    lv_obj_set_style_bg_opa(eye, fill_opa, 0);
    lv_obj_set_style_border_color(eye, lv_color_hex(0x006080), 0);
    lv_obj_set_style_border_width(eye, 1, 0);
    lv_obj_set_style_border_opa(eye, LV_OPA_60, 0);
    lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE);

    /* Không dùng shadow: trên ESP32 + LVGL sw-render shadow rất tốn CPU, dễ gây cảm giác “giật/lag”. */
    lv_obj_set_style_shadow_width(eye, 0, 0);
    lv_obj_set_style_shadow_opa(eye, LV_OPA_TRANSP, 0);
    (void)glow;
}

static void FlipPointX(lv_point_precise_t* p, int32_t cw) {
    lv_point_t t = lv_point_from_precise(p);
    t.x = cw - 1 - t.x;
    *p = lv_point_to_precise(&t);
}

static void AppendStraightF(float x0,
                            float y0,
                            float x1,
                            float y1,
                            int steps,
                            bool skip_first,
                            float* rx,
                            float* ry,
                            int& n,
                            int cap) {
    if (steps < 1) {
        steps = 1;
    }
    const int i0 = skip_first ? 1 : 0;
    for (int i = i0; i <= steps; ++i) {
        if (n >= cap) {
            return;
        }
        const float t = (float)i / (float)steps;
        rx[n] = x0 + (x1 - x0) * t;
        ry[n] = y0 + (y1 - y0) * t;
        ++n;
    }
}

/** Bo tròn góc lồi: A–B–C theo thứ tự đường biên CCW, cung nằm trong đa giác. */
static void AppendRoundedCornerFillet(float ax,
                                      float ay,
                                      float bx,
                                      float by,
                                      float cx,
                                      float cy,
                                      float r,
                                      int n_arc,
                                      bool skip_first,
                                      float* rx,
                                      float* ry,
                                      int& n,
                                      int cap,
                                      float max_t) {
    float ux = ax - bx;
    float uy = ay - by;
    float vx = cx - bx;
    float vy = cy - by;
    const float lu = hypotf(ux, uy);
    const float lv = hypotf(vx, vy);
    if (lu < 1e-4f || lv < 1e-4f) {
        return;
    }
    ux /= lu;
    uy /= lu;
    vx /= lv;
    vy /= lv;
    float cphi = ux * vx + uy * vy;
    cphi = fmaxf(-1.f, fminf(1.f, cphi));
    const float phi = acosf(cphi);
    if (phi < 1e-4f) {
        return;
    }
    float tan_half = tanf(phi * 0.5f);
    if (fabsf(tan_half) < 1e-5f) {
        return;
    }
    float tcut = r / tan_half;
    if (tcut > max_t) {
        tcut = max_t;
    }
    const float t1x = bx + ux * tcut;
    const float t1y = by + uy * tcut;
    const float t2x = bx + vx * tcut;
    const float t2y = by + vy * tcut;

    const float sin_h = sinf(phi * 0.5f);
    if (fabsf(sin_h) < 1e-5f) {
        return;
    }
    const float bisx = ux + vx;
    const float bisy = uy + vy;
    float bl = hypotf(bisx, bisy);
    if (bl < 1e-4f) {
        return;
    }
    const float inv_bl = 1.f / bl;
    const float dist_c = r / sin_h;
    const float cx_c = bx + bisx * inv_bl * dist_c;
    const float cy_c = by + bisy * inv_bl * dist_c;

    float a1 = atan2f(t1y - cy_c, t1x - cx_c);
    float a2 = atan2f(t2y - cy_c, t2x - cx_c);
    float dang = a2 - a1;
    while (dang > static_cast<float>(M_PI)) {
        dang -= 2.f * static_cast<float>(M_PI);
    }
    while (dang <= -static_cast<float>(M_PI)) {
        dang += 2.f * static_cast<float>(M_PI);
    }
    /* CCW biên: chọn cung ngắn cùng dấu với dang mong muốn (vào trong tam giác). */
    if (dang < 0.f) {
        dang += 2.f * static_cast<float>(M_PI);
    }

    const int na = (n_arc < 3) ? 3 : n_arc;
    const int i0 = skip_first ? 1 : 0;
    for (int k = i0; k <= na; ++k) {
        if (n >= cap) {
            return;
        }
        const float tt = (float)k / (float)na;
        const float ang = a1 + dang * tt;
        const float rad = hypotf(t1x - cx_c, t1y - cy_c);
        rx[n] = cx_c + cosf(ang) * rad;
        ry[n] = cy_c + sinf(ang) * rad;
        ++n;
    }
}

static float ShoelaceSignedArea(const float* x, const float* y, int nv) {
    float a = 0.f;
    for (int i = 0; i < nv; ++i) {
        const int j = (i + 1) % nv;
        a += x[i] * y[j] - x[j] * y[i];
    }
    return a * 0.5f;
}

static bool PointInOpenTriangle(float px,
                                float py,
                                float ax,
                                float ay,
                                float bx,
                                float by,
                                float cx,
                                float cy) {
    constexpr float eps = 2e-2f;
    /* Barycentric */
    const float v0x = bx - ax;
    const float v0y = by - ay;
    const float v1x = cx - ax;
    const float v1y = cy - ay;
    const float v2x = px - ax;
    const float v2y = py - ay;
    const float d00 = v0x * v0x + v0y * v0y;
    const float d01 = v0x * v1x + v0y * v1y;
    const float d11 = v1x * v1x + v1y * v1y;
    const float d20 = v2x * v0x + v2y * v0y;
    const float d21 = v2x * v1x + v2y * v1y;
    const float den = d00 * d11 - d01 * d01;
    if (fabsf(den) < 1e-8f) {
        return false;
    }
    const float inv = 1.f / den;
    const float v = (d11 * d20 - d01 * d21) * inv;
    const float w = (d00 * d21 - d01 * d20) * inv;
    const float u = 1.f - v - w;
    return (u > eps && v > eps && w > eps);
}

/** Ear-clipping: đa giác đơn (có lõm) — tô kín; fan 1 neo trước đây để lỗ → nền đen lộ. */
static int TriangulateEar(const float* x,
                          const float* y,
                          int nv,
                          int* i0,
                          int* i1,
                          int* i2,
                          int tri_cap) {
    if (nv < 3) {
        return 0;
    }
    float xs[128];
    float ys[128];
    for (int i = 0; i < nv; ++i) {
        xs[i] = x[i];
        ys[i] = y[i];
    }
    const float sa = ShoelaceSignedArea(xs, ys, nv);
    const bool ccw = sa > 0.f;
    if (!ccw) {
        for (int i = 0; i < nv / 2; ++i) {
            const int j = nv - 1 - i;
            float txx = xs[i];
            xs[i] = xs[j];
            xs[j] = txx;
            float tyy = ys[i];
            ys[i] = ys[j];
            ys[j] = tyy;
        }
    }

    int idx[128];
    int rem = nv;
    for (int i = 0; i < rem; ++i) {
        idx[i] = i;
    }

    int nt = 0;
    int guard = 0;
    while (rem > 2 && nt < tri_cap && guard++ < 512) {
        bool progressed = false;
        for (int i = 0; i < rem; ++i) {
            const int iv0 = idx[(i + rem - 1) % rem];
            const int iv1 = idx[i];
            const int iv2 = idx[(i + 1) % rem];
            const float ax = xs[iv0];
            const float ay = ys[iv0];
            const float bx = xs[iv1];
            const float by = ys[iv1];
            const float cx = xs[iv2];
            const float cy = ys[iv2];
            const float e1x = bx - ax;
            const float e1y = by - ay;
            const float e2x = cx - bx;
            const float e2y = cy - by;
            const float cr = e1x * e2y - e1y * e2x;
            if (!(ccw ? (cr > 1e-4f) : (cr < -1e-4f))) {
                continue;
            }
            bool ok = true;
            for (int k = 0; k < rem && ok; ++k) {
                const int ik = idx[k];
                if (ik == iv0 || ik == iv1 || ik == iv2) {
                    continue;
                }
                if (PointInOpenTriangle(xs[ik], ys[ik], ax, ay, bx, by, cx, cy)) {
                    ok = false;
                }
            }
            if (!ok) {
                continue;
            }
            i0[nt] = iv0;
            i1[nt] = iv1;
            i2[nt] = iv2;
            ++nt;
            for (int k = i; k < rem - 1; ++k) {
                idx[k] = idx[k + 1];
            }
            --rem;
            progressed = true;
            break;
        }
        if (!progressed) {
            break;
        }
    }
    return nt;
}

void DrawSadEyeCanvas(lv_obj_t* canvas, bool mirror) {
    lv_canvas_fill_bg(canvas, lv_color_hex(0x000000), LV_OPA_TRANSP);

    const int32_t cw = (int32_t)lv_obj_get_width(canvas);
    const int32_t ch = (int32_t)lv_obj_get_height(canvas);
    const float margin = (float)(cw * 9 / 100);

    /*
     * Tam giác vuông phẳng (một mắt): góc vuông ở mé trong dưới (I).
     * O → đáy ngoài trái, I → trong đáy, T → trong trên; biên O → I → T → O.
     */
    float Ox = margin + 2.f;
    float Oy = (float)ch * 0.86f;
    float Ix = (float)cw - margin - 2.f;
    float Iy = Oy;
    float Tx = Ix;
    float Ty = (float)ch * 0.13f;

    if (mirror) {
        const float m = (float)(cw - 1);
        Ox = m - Ox;
        Ix = m - Ix;
        Tx = m - Tx;
    }

    const lv_color_t fill = lv_color_hex(0x32EEFF);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    lv_point_precise_set(&dsc.p[0], (lv_value_precise_t)Ox, (lv_value_precise_t)Oy);
    lv_point_precise_set(&dsc.p[1], (lv_value_precise_t)Ix, (lv_value_precise_t)Iy);
    lv_point_precise_set(&dsc.p[2], (lv_value_precise_t)Tx, (lv_value_precise_t)Ty);
    dsc.color = fill;
    dsc.opa = LV_OPA_COVER;
    dsc.grad.dir = LV_GRAD_DIR_NONE;
    lv_draw_triangle(&layer, &dsc);

    lv_canvas_finish_layer(canvas, &layer);
    lv_obj_invalidate(canvas);
}

/** Hai tam giác vuông “giận” (mép hướng trong xuống): góc vuông ở đáy ngoài, bo 3 góc — cùng lớp viền CCW như buồn. */
void DrawAngryEyeCanvas(lv_obj_t* canvas, bool mirror) {
    lv_canvas_fill_bg(canvas, lv_color_hex(0x000000), LV_OPA_TRANSP);

    const int32_t cw = (int32_t)lv_obj_get_width(canvas);
    const int32_t ch = (int32_t)lv_obj_get_height(canvas);
    const float margin = (float)(cw * 9 / 100);

    /*
     * Góc vuông ở O = đáy ngoài trái. I = đáy trong phải, T = đỉnh ngoài (cạnh dọc trái).
     * Biên CCW: O → I → T → O (đáy ngang, huyền xuống trong, cạnh dọc ngoài).
     */
    const float Ox = margin + 2.f;
    const float Oy = (float)ch * 0.86f;
    const float Ix = (float)cw - margin - 2.f;
    const float Iy = Oy;
    const float Tx = Ox;
    const float Ty = (float)ch * 0.13f;

    const float edge_oi = hypotf(Ix - Ox, Iy - Oy);
    const float edge_it = hypotf(Tx - Ix, Ty - Iy);
    const float edge_to = hypotf(Ox - Tx, Oy - Ty);
    const float r_raw = fminf((float)cw, (float)ch) * 0.058f;
    float r = r_raw;
    if (r > 9.5f) {
        r = 9.5f;
    }
    if (r < 3.5f) {
        r = 3.5f;
    }
    const float max_t = 0.38f * fminf(edge_oi, fminf(edge_it, edge_to));

    auto corner_tcut = [&](float ax, float ay, float bx, float by, float cx, float cy) -> float {
        float ux = ax - bx;
        float uy = ay - by;
        float vx = cx - bx;
        float vy = cy - by;
        const float lu = hypotf(ux, uy);
        const float lv = hypotf(vx, vy);
        if (lu < 1e-4f || lv < 1e-4f) {
            return 0.f;
        }
        ux /= lu;
        uy /= lu;
        vx /= lv;
        vy /= lv;
        float cph = ux * vx + uy * vy;
        cph = fmaxf(-1.f, fminf(1.f, cph));
        const float phi = acosf(cph);
        const float th = tanf(phi * 0.5f);
        if (fabsf(th) < 1e-5f) {
            return 0.f;
        }
        float tc = r / th;
        if (tc > max_t) {
            tc = max_t;
        }
        return tc;
    };

    const float t_O = corner_tcut(Tx, Ty, Ox, Oy, Ix, Iy);
    const float t_I = corner_tcut(Ox, Oy, Ix, Iy, Tx, Ty);
    const float t_T = corner_tcut(Ix, Iy, Tx, Ty, Ox, Oy);

    const float inv_to = 1.f / edge_to;
    const float inv_oi = 1.f / edge_oi;
    const float inv_it = 1.f / edge_it;
    const float T1ox = Ox + (Tx - Ox) * inv_to * t_O;
    const float T1oy = Oy + (Ty - Oy) * inv_to * t_O;
    const float T1ix = Ix + (Ox - Ix) * inv_oi * t_I;
    const float T1iy = Iy + (Oy - Iy) * inv_oi * t_I;
    const float T1tx = Tx + (Ix - Tx) * inv_it * t_T;
    const float T1ty = Ty + (Iy - Ty) * inv_it * t_T;

    constexpr int kArcSeg = 12;
    constexpr int kHypSeg = 22;
    constexpr int kRingCap = 160;
    float rx[kRingCap];
    float ry[kRingCap];
    int nring = 0;

    AppendRoundedCornerFillet(Tx, Ty, Ox, Oy, Ix, Iy, r, kArcSeg, false, rx, ry, nring, kRingCap, max_t);
    AppendStraightF(rx[nring - 1], ry[nring - 1], T1ix, T1iy, 3, true, rx, ry, nring, kRingCap);
    AppendRoundedCornerFillet(Ox, Oy, Ix, Iy, Tx, Ty, r, kArcSeg, true, rx, ry, nring, kRingCap, max_t);
    AppendStraightF(rx[nring - 1], ry[nring - 1], T1tx, T1ty, 3, true, rx, ry, nring, kRingCap);
    AppendRoundedCornerFillet(Ix, Iy, Tx, Ty, Ox, Oy, r, kArcSeg, true, rx, ry, nring, kRingCap, max_t);
    AppendStraightF(rx[nring - 1], ry[nring - 1], T1ox, T1oy, kHypSeg, true, rx, ry, nring, kRingCap);

    if (mirror) {
        for (int i = 0; i < nring; ++i) {
            rx[i] = (float)(cw - 1) - rx[i];
        }
    }

    constexpr int kMaxTri = 200;
    int t0[kMaxTri];
    int t1[kMaxTri];
    int t2[kMaxTri];
    int nt = TriangulateEar(rx, ry, nring, t0, t1, t2, kMaxTri);
    if (nt < nring - 2) {
        nt = 0;
    }

    const lv_color_t fill = lv_color_hex(0xFF6A4D);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    if (nt > 0) {
        for (int t = 0; t < nt; ++t) {
            lv_draw_triangle_dsc_t dsc;
            lv_draw_triangle_dsc_init(&dsc);
            lv_point_precise_set(&dsc.p[0], (lv_value_precise_t)rx[t0[t]], (lv_value_precise_t)ry[t0[t]]);
            lv_point_precise_set(&dsc.p[1], (lv_value_precise_t)rx[t1[t]], (lv_value_precise_t)ry[t1[t]]);
            lv_point_precise_set(&dsc.p[2], (lv_value_precise_t)rx[t2[t]], (lv_value_precise_t)ry[t2[t]]);
            dsc.color = fill;
            dsc.opa = LV_OPA_COVER;
            dsc.grad.dir = LV_GRAD_DIR_NONE;
            lv_draw_triangle(&layer, &dsc);
        }
    } else if (nring >= 3) {
        float sx = 0.f;
        float sy = 0.f;
        for (int i = 0; i < nring; ++i) {
            sx += rx[i];
            sy += ry[i];
        }
        sx /= (float)nring;
        sy /= (float)nring;
        lv_point_precise_t an;
        lv_point_precise_set(&an, (lv_value_precise_t)sx, (lv_value_precise_t)sy);
        for (int i = 0; i < nring; ++i) {
            const int j = (i + 1) % nring;
            lv_draw_triangle_dsc_t dsc;
            lv_draw_triangle_dsc_init(&dsc);
            dsc.p[0] = an;
            lv_point_precise_set(&dsc.p[1], (lv_value_precise_t)rx[i], (lv_value_precise_t)ry[i]);
            lv_point_precise_set(&dsc.p[2], (lv_value_precise_t)rx[j], (lv_value_precise_t)ry[j]);
            dsc.color = fill;
            dsc.opa = LV_OPA_COVER;
            dsc.grad.dir = LV_GRAD_DIR_NONE;
            lv_draw_triangle(&layer, &dsc);
        }
    }

    lv_canvas_finish_layer(canvas, &layer);
    lv_obj_invalidate(canvas);
}

/** Khung giữa khi chớp: hai viên “thuốc” nằm ngang (mí đang kéo xuống). */
void StyleBlinkPillEye(lv_obj_t* eye) {
    const lv_coord_t w = kBlinkPillW;
    const lv_coord_t h = kBlinkPillH;
    lv_obj_set_size(eye, w, h);
    lv_obj_set_style_radius(eye, h / 2, 0);
    lv_obj_set_style_pad_all(eye, 0, 0);
    lv_obj_set_style_bg_color(eye, lv_color_hex(0x32EEFF), 0);
    lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(eye, 0, 0);
    lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_shadow_width(eye, 0, 0);
    lv_obj_set_style_shadow_opa(eye, LV_OPA_TRANSP, 0);
    lv_obj_center(eye);
}

/** Căn mắt giữa `face_area_`; `extra_x` dịch trái/phải (hiệu ứng “nhìn quanh”). */
void AlignEyesRowCentered(lv_obj_t* eyes_row, lv_coord_t extra_x) {
    if (eyes_row == nullptr) {
        return;
    }
    lv_obj_t* parent = lv_obj_get_parent(eyes_row);
    if (parent) {
        lv_obj_update_layout(parent);
    }
    lv_obj_set_width(eyes_row, LV_SIZE_CONTENT);
    lv_obj_set_height(eyes_row, LV_SIZE_CONTENT);
    lv_obj_align(eyes_row, LV_ALIGN_CENTER, extra_x, kEyesRowOffsetY);
}

/** Sóng tam giác nhỏ (±8px) — nhẹ CPU, ~8s một vòng @ 500ms/bước. */
constexpr uint8_t kLookWaveLen = 16;
static const int8_t kLookWave[kLookWaveLen] = {
    0, 2, 4, 6, 8, 6, 4, 2, 0, -2, -4, -6, -8, -6, -4, -2,
};

}  // namespace

OttoEmojiDisplay::OttoEmojiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                   int width, int height, int offset_x, int offset_y, bool mirror_x,
                                   bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy) {
    SetupGifContainer();
}

OttoEmojiDisplay::~OttoEmojiDisplay() {
    StopLookTimer();
    if (blink_timer_) {
        lv_timer_delete(blink_timer_);
        blink_timer_ = nullptr;
    }
}

void OttoEmojiDisplay::SyncEyesAlign() {
    AlignEyesRowCentered(eyes_row_, look_offset_x_);
}

void OttoEmojiDisplay::StopLookTimer() {
    if (look_timer_) {
        lv_timer_delete(look_timer_);
        look_timer_ = nullptr;
    }
    look_phase_ = 0;
    look_offset_x_ = 0;
}

void OttoEmojiDisplay::StartLookTimer() {
    StopLookTimer();
    look_timer_ = lv_timer_create(OnLookTimer, 520, this);
    lv_timer_set_repeat_count(look_timer_, -1);
}

void OttoEmojiDisplay::OnLookTimer(lv_timer_t* timer) {
    auto* self = static_cast<OttoEmojiDisplay*>(lv_timer_get_user_data(timer));
    if (self == nullptr || self->eyes_row_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(self);
    self->look_phase_ = static_cast<uint8_t>((self->look_phase_ + 1U) % kLookWaveLen);
    self->look_offset_x_ = kLookWave[self->look_phase_];
    AlignEyesRowCentered(self->eyes_row_, self->look_offset_x_);
    (void)timer;
}

void OttoEmojiDisplay::CreateRobotEyes() {
    LV_DRAW_BUF_INIT_STATIC(sad_eye_buf_l);
    LV_DRAW_BUF_INIT_STATIC(sad_eye_buf_r);

    eyes_row_ = lv_obj_create(face_area_);
    lv_obj_add_flag(eyes_row_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(eyes_row_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(eyes_row_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(eyes_row_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(eyes_row_, 0, 0);
    lv_obj_set_style_pad_all(eyes_row_, 0, 0);
    lv_obj_set_style_pad_column(eyes_row_, kEyesGap, 0);
    lv_obj_set_flex_flow(eyes_row_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_main_place(eyes_row_, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_flex_cross_place(eyes_row_, LV_FLEX_ALIGN_CENTER, 0);

    auto setup_slot = [this](lv_obj_t** slot, lv_obj_t** rect, lv_obj_t** sad_cv, lv_draw_buf_t* buf) {
        *slot = lv_obj_create(eyes_row_);
        lv_obj_set_style_bg_opa(*slot, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(*slot, 0, 0);
        lv_obj_set_style_pad_all(*slot, 0, 0);
        lv_obj_set_size(*slot, kEyeSlotSize, kEyeSlotSize);

        *rect = lv_obj_create(*slot);
        lv_obj_center(*rect);

        *sad_cv = lv_canvas_create(*slot);
        lv_canvas_set_draw_buf(*sad_cv, buf);
        lv_obj_set_size(*sad_cv, kSadCanvasSize, kSadCanvasSize);
        lv_obj_center(*sad_cv);
        lv_obj_add_flag(*sad_cv, LV_OBJ_FLAG_HIDDEN);
    };

    setup_slot(&eye_left_, &eye_left_rect_, &eye_left_sad_, &sad_eye_buf_l);
    setup_slot(&eye_right_, &eye_right_rect_, &eye_right_sad_, &sad_eye_buf_r);

    ApplyEmotionStyle("neutral");
    StartBlinkTimer();
    StartLookTimer();
}

void OttoEmojiDisplay::StartBlinkTimer() {
    if (blink_timer_) {
        lv_timer_delete(blink_timer_);
        blink_timer_ = nullptr;
    }
    /* Nháy thưa hơn → ít vẽ lại/ít timer khi đang chạy UI+TTS. */
    blink_timer_ = lv_timer_create(OnBlinkTimer, 4800, this);
    lv_timer_set_repeat_count(blink_timer_, -1);
}

void OttoEmojiDisplay::OnBlinkTimer(lv_timer_t* timer) {
    auto* self = static_cast<OttoEmojiDisplay*>(lv_timer_get_user_data(timer));
    if (self) {
        self->DoBlinkClose();
        uint32_t next_ms = 4000U + (esp_random() % 4500U);
        lv_timer_set_period(timer, next_ms);
    }
}

void OttoEmojiDisplay::OnReopenAfterBlink(lv_timer_t* timer) {
    auto* self = static_cast<OttoEmojiDisplay*>(lv_timer_get_user_data(timer));
    if (self == nullptr) {
        return;
    }
    {
        DisplayLockGuard lock(self);
        self->ApplyEmotionStyle(self->last_emotion_);
        if (self->blink_timer_) {
            lv_timer_resume(self->blink_timer_);
        }
        if (self->look_timer_) {
            lv_timer_resume(self->look_timer_);
        }
    }
    (void)timer;
}

void OttoEmojiDisplay::DoBlinkClose() {
    DisplayLockGuard lock(this);
    if (!eye_left_ || !eye_right_ || !eye_left_rect_ || !eye_right_rect_) {
        return;
    }
    if (blink_timer_) {
        lv_timer_pause(blink_timer_);
    }
    if (look_timer_) {
        lv_timer_pause(look_timer_);
    }
    look_offset_x_ = 0;

    const bool is_sad = (strcmp(last_emotion_, "sad") == 0 || strcmp(last_emotion_, "crying") == 0);
    const bool is_angry = (strcmp(last_emotion_, "angry") == 0);
    if ((is_sad || is_angry) && eye_left_sad_ && eye_right_sad_) {
        lv_obj_add_flag(eye_left_sad_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(eye_right_sad_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(eye_left_rect_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(eye_right_rect_, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_set_size(eye_left_, kEyeSlotSize, kEyeSlotSize);
    lv_obj_set_size(eye_right_, kEyeSlotSize, kEyeSlotSize);
    StyleBlinkPillEye(eye_left_rect_);
    StyleBlinkPillEye(eye_right_rect_);
    AlignEyesRowCentered(eyes_row_, 0);

    lv_timer_t* to_closed = lv_timer_create(OnBlinkToClosed, 50, this);
    lv_timer_set_repeat_count(to_closed, 1);
}

void OttoEmojiDisplay::OnBlinkToClosed(lv_timer_t* timer) {
    OttoEmojiDisplay* self = static_cast<OttoEmojiDisplay*>(lv_timer_get_user_data(timer));
    if (self == nullptr) {
        return;
    }
    {
        DisplayLockGuard lock(self);
        if (self->eye_left_ && self->eye_right_) {
            lv_obj_set_height(self->eye_left_, 6);
            lv_obj_set_height(self->eye_right_, 6);
        }
        AlignEyesRowCentered(self->eyes_row_, 0);
    }

    lv_timer_t* reopen = lv_timer_create(OnReopenAfterBlink, 105, self);
    lv_timer_set_repeat_count(reopen, 1);
}

void OttoEmojiDisplay::ApplyEmotionStyle(const char* emotion) {
    if (!eye_left_ || !eye_right_ || !eye_left_rect_ || !eye_right_rect_ || !eye_left_sad_ ||
        !eye_right_sad_) {
        return;
    }

    if (!emotion) {
        emotion = "neutral";
    }

    const bool is_sad = (strcmp(emotion, "sad") == 0 || strcmp(emotion, "crying") == 0);
    const bool is_angry = (strcmp(emotion, "angry") == 0);

    if (is_sad) {
        lv_obj_set_size(eye_left_, kEyeSlotSize, kEyeSlotSize);
        lv_obj_set_size(eye_right_, kEyeSlotSize, kEyeSlotSize);
        lv_obj_add_flag(eye_left_rect_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(eye_right_rect_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(eye_left_sad_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(eye_right_sad_, LV_OBJ_FLAG_HIDDEN);
        DrawSadEyeCanvas(eye_left_sad_, false);
        DrawSadEyeCanvas(eye_right_sad_, true);
        strncpy(last_emotion_, emotion, sizeof(last_emotion_) - 1);
        last_emotion_[sizeof(last_emotion_) - 1] = '\0';
        SyncEyesAlign();
        return;
    }

    if (is_angry) {
        lv_obj_set_size(eye_left_, kEyeSlotSize, kEyeSlotSize);
        lv_obj_set_size(eye_right_, kEyeSlotSize, kEyeSlotSize);
        lv_obj_add_flag(eye_left_rect_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(eye_right_rect_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(eye_left_sad_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(eye_right_sad_, LV_OBJ_FLAG_HIDDEN);
        DrawAngryEyeCanvas(eye_left_sad_, false);
        DrawAngryEyeCanvas(eye_right_sad_, true);
        strncpy(last_emotion_, emotion, sizeof(last_emotion_) - 1);
        last_emotion_[sizeof(last_emotion_) - 1] = '\0';
        SyncEyesAlign();
        return;
    }

    lv_obj_set_size(eye_left_, kEyeSlotSize, kEyeSlotSize);
    lv_obj_set_size(eye_right_, kEyeSlotSize, kEyeSlotSize);
    lv_obj_add_flag(eye_left_sad_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(eye_right_sad_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(eye_left_rect_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(eye_right_rect_, LV_OBJ_FLAG_HIDDEN);

    lv_coord_t w = kEyeSize;
    lv_coord_t h = kEyeSize;
    lv_color_t fill = lv_color_hex(0x24F0FF);
    lv_opa_t opa = LV_OPA_COVER;
    lv_color_t glow = lv_color_hex(0x00B8E6);

    if (strcmp(emotion, "surprised") == 0 || strcmp(emotion, "shocked") == 0) {
        w = h = kEyeSize * 110 / 100;
    } else if (strcmp(emotion, "happy") == 0 || strcmp(emotion, "laughing") == 0 ||
               strcmp(emotion, "loving") == 0 || strcmp(emotion, "funny") == 0 ||
               strcmp(emotion, "confident") == 0 || strcmp(emotion, "winking") == 0 ||
               strcmp(emotion, "cool") == 0 || strcmp(emotion, "delicious") == 0 ||
               strcmp(emotion, "kissy") == 0 || strcmp(emotion, "silly") == 0) {
        w = h = kEyeSize * 105 / 100;
        fill = lv_color_hex(0x3DFFFF);
    } else if (strcmp(emotion, "thinking") == 0 || strcmp(emotion, "confused") == 0 ||
               strcmp(emotion, "embarrassed") == 0) {
        w = h = kEyeSize * 90 / 100;
        fill = lv_color_hex(0x1CD8F0);
        glow = lv_color_hex(0x0099CC);
    }

    StyleSingleEye(eye_left_rect_, w, h, fill, opa, glow);
    StyleSingleEye(eye_right_rect_, w, h, fill, opa, glow);

    strncpy(last_emotion_, emotion, sizeof(last_emotion_) - 1);
    last_emotion_[sizeof(last_emotion_) - 1] = '\0';
    SyncEyesAlign();
}

void OttoEmojiDisplay::SetupGifContainer() {
    DisplayLockGuard lock(this);

    StopLookTimer();

    if (emoji_label_) {
        lv_obj_del(emoji_label_);
    }

    if (chat_message_label_) {
        lv_obj_del(chat_message_label_);
    }
    if (content_) {
        lv_obj_del(content_);
    }

    face_area_ = nullptr;
    eyes_row_ = nullptr;
    eye_left_ = nullptr;
    eye_right_ = nullptr;
    eye_left_rect_ = nullptr;
    eye_right_rect_ = nullptr;
    eye_left_sad_ = nullptr;
    eye_right_sad_ = nullptr;

    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, 0, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(content_, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content_, 0, 0);
    /* Xếp từ trên: emoji ẩn → vùng mặt (chiếm phần còn lại) → chát cố định dưới cùng */
    lv_obj_set_style_flex_main_place(content_, LV_FLEX_ALIGN_START, 0);
    lv_obj_set_style_flex_cross_place(content_, LV_FLEX_ALIGN_CENTER, 0);

    emoji_label_ = lv_label_create(content_);
    lv_label_set_text(emoji_label_, "");
    lv_obj_set_width(emoji_label_, 0);
    lv_obj_set_style_border_width(emoji_label_, 0, 0);
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);

    face_area_ = lv_obj_create(content_);
    lv_obj_set_style_bg_opa(face_area_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(face_area_, 0, 0);
    lv_obj_set_style_pad_all(face_area_, 0, 0);
    lv_obj_set_width(face_area_, LV_PCT(100));
    lv_obj_set_flex_grow(face_area_, 1);
    lv_obj_set_style_min_height(face_area_, 0, 0);
    lv_obj_clear_flag(face_area_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(face_area_, LV_SCROLLBAR_MODE_OFF);

    CreateRobotEyes();

    chat_message_label_ = lv_label_create(content_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES * 0.9);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lv_color_white(), 0);
    lv_obj_set_style_border_width(chat_message_label_, 0, 0);
    lv_obj_set_style_outline_width(chat_message_label_, 0, 0);
    /* Trong suốt: không vạch/ảnh nền ngăn mắt với chữ (nền vẫn đen từ content_). */
    lv_obj_set_style_bg_opa(chat_message_label_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_ver(chat_message_label_, 2, 0);
    lv_obj_set_flex_grow(chat_message_label_, 0);

    auto& theme_manager = LvglThemeManager::GetInstance();
    auto theme = theme_manager.GetTheme("dark");
    if (theme != nullptr) {
        LcdDisplay::SetTheme(theme);
    }
    /* LcdDisplay::SetTheme đặt content_ thành nền trong suốt — Otto cần nền đen đục để mắt hiển thị đúng */
    if (content_) {
        lv_obj_set_style_bg_color(content_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(content_, LV_OPA_COVER, 0);
    }
    /* Một dòng cố định + cuộn vòng — không chiếm thêm chiều cao khi thoại dài. */
    if (chat_message_label_) {
        lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(chat_message_label_, LV_HOR_RES * 9 / 10);
        const lv_font_t* font = lv_obj_get_style_text_font(chat_message_label_, LV_PART_MAIN);
        const lv_coord_t line_h = font ? lv_font_get_line_height(font) : 20;
        lv_obj_set_height(chat_message_label_, line_h + 10);
    }
}

void OttoEmojiDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);

    const char* mapped = emotion;
    if (!mapped) {
        mapped = "neutral";
    }

    ApplyEmotionStyle(mapped);
    ESP_LOGD(TAG, "设置表情(eyes): %s", mapped);
}

void OttoEmojiDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        return;
    }

    if (content == nullptr || strlen(content) == 0) {
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        SyncEyesAlign();
        return;
    }

    const bool was_hidden = lv_obj_has_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(chat_message_label_, content);
    lv_obj_remove_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
    if (was_hidden) {
        SyncEyesAlign();
    }

    ESP_LOGD(TAG, "设置聊天消息 [%s]: %s", role, content);
}

void OttoEmojiDisplay::SetTheme(Theme* theme) {
    LcdDisplay::SetTheme(theme);
    DisplayLockGuard lock(this);
    if (content_) {
        lv_obj_set_style_bg_color(content_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(content_, LV_OPA_COVER, 0);
    }
    SyncEyesAlign();
}
