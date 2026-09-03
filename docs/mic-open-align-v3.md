# Mic mở sau TTS — căn theo `xiaozhi-esp32-main (3)`

Tài liệu này mô tả thay đổi ngày 2026-06-08: logic mở micro sau TTS được **đơn giản hóa giống bản tham chiếu `(3)`**, giữ thêm tính năng Otto (tút `popup.ogg`, silence prompt, music mode).

## Mục tiêu

- Mở micro **nhanh hơn** sau khi server phát TTS xong.
- Bỏ các lớp chờ / chống echo phức tạp gây cảm giác “tút xong mà micro chưa sẵn sàng”.

## So sánh với bản cũ (trước khi align v3)

| Hành vi | Trước (echo-guard) | Sau (align v3) | `(3)` gốc |
|--------|---------------------|----------------|-----------|
| Chờ loa ở `tts stop` | Có `WaitForPlaybackQueueEmpty()` | **Không** | Không |
| Chờ loa ở `listening` | Có (lần 2, trùng) | **1 lần** (AutoStop) | 1 lần |
| Kiểm tra `IsAudioChannelOpened()` trước mở mic | Có | **Không** (chỉ `if (protocol_)`) | Không |
| UDP TTS trễ sau `tts stop` | Vẫn phát + tắt mic | **Bỏ qua** (chỉ nhận khi `speaking`) | Bỏ qua |
| `uplink_muted_for_downlink_` | Có | **Đã xóa** | Không có |
| `MaybeResumeUplinkAfterDownlink()` | Có (clock 1 Hz) | **Đã xóa** | Không có |
| Tiếng tút khi icon micro | Có (`popup.ogg`) | **Vẫn có** | Chỉ wake word (`play_popup_on_listening_`) |
| Warmup mic | 120 ms | 120 ms (chưa đổi) | 120 ms |

## File đã sửa

- `main/application.cc`
- `main/application.h`

## Chi tiết thay đổi trong code

### 1. `tts stop` — chuyển `listening` ngay

```cpp
// Giống (3): không WaitForPlaybackQueueEmpty, không check channel
if (GetDeviceState() == kDeviceStateSpeaking) {
    if (listening_mode_ == kListeningModeManualStop)
        SetDeviceState(kDeviceStateIdle);
    else
        SetDeviceState(kDeviceStateListening);
}
```

### 2. `OnIncomingAudio` — chỉ `speaking`

```cpp
if (GetDeviceState() == kDeviceStateSpeaking)
    audio_service_.PushPacketToDecodeQueue(std::move(packet));
// Gói UDP đến trễ sau tts stop → drop (giống v3)
```

### 3. `kDeviceStateListening` — mở mic giống (3)

```cpp
if (!IsCaptureSuspended() && !IsAudioProcessorRunning()) {
    if (listening_mode_ == kListeningModeAutoStop)
        WaitForPlaybackQueueEmpty();  // duy nhất 1 lần chờ loa
    if (protocol_)
        SendStartListening(...);
    EnableVoiceProcessing(true);
}
// Sau đó: tút popup (Otto, không có trong v3 sau mỗi TTS)
```

### 4. Đã xóa hoàn toàn

- Biến `uplink_muted_for_downlink_`
- Hàm `MaybeResumeUplinkAfterDownlink()`
- Gọi `MaybeResumeUplinkAfterDownlink` trong clock tick
- Nhánh nhận UDP khi `listening` + mute uplink

### 5. Giữ nguyên (Otto)

- `suppress_listening_chime_` + `PrepareAudioChannelClose()` — không tút khi goodbye/disconnect
- `PlayNotificationSound(OGG_POPUP)` mỗi lần vào `listening`
- Silence prompt, music mode, `IsCaptureSuspended`

## Rủi ro / trade-off

1. **Cắt cuối câu TTS (hiếm):** `tts stop` JSON có thể đến trước khi loa phát hết → chuyển `listening` sớm; phần UDP còn lại bị **drop** thay vì phát nốt.
2. **Echo nhẹ (hiếm):** không còn tắt mic khi UDP trễ → mic có thể bật khi loa vẫn còn tiếng (nếu server gửi `stop` quá sớm).
3. **Sau tút vẫn ~120 ms warmup** trước khi capture thật — giống v3; muốn nhanh hơn nữa có thể giảm `audio_input_need_warmup_` trong `audio_service.cc`.

## Cách revert về bản echo-guard

1. `git diff main/application.cc main/application.h` — xem diff hiện tại.
2. Hoặc restore thủ công các phần sau trong `application.cc`:

**A. `tts stop`** — thêm lại:

```cpp
audio_service_.WaitForPlaybackQueueEmpty();
if (GetDeviceState() != kDeviceStateSpeaking) return;
// ...
} else if (protocol_ && protocol_->IsAudioChannelOpened()) {
    SetDeviceState(kDeviceStateListening);
} else {
    SetDeviceState(kDeviceStateIdle);
}
```

**B. `OnIncomingAudio`** — thêm lại nhánh `listening` + `uplink_muted_for_downlink_`.

**C. `application.h`** — thêm lại:

```cpp
bool uplink_muted_for_downlink_ = false;
void MaybeResumeUplinkAfterDownlink();
```

**D. Clock tick** — thêm lại:

```cpp
if (uplink_muted_for_downlink_ && GetDeviceState() == kDeviceStateListening)
    MaybeResumeUplinkAfterDownlink();
```

**E. `MaybeResumeUplinkAfterDownlink()`** — khôi phục implementation (xem git history trước commit align v3).

## Test nhanh sau flash

```powershell
idf.py build
idf.py -p COM6 flash monitor
```

1. Wake word → bot trả lời TTS → sau câu nói: icon micro + tút → nói ngay (nhanh hơn trước).
2. Im lặng 15s → silence prompt vẫn hoạt động (1 lần / phiên listening).
3. Goodbye / ngắt MQTT → **không** tút.
4. Nếu nghe câu TTS bị cắt cụt hoặc STT nhận echo → revert theo mục trên.

## Tham chiếu

- Bản gốc: `xiaozhi-esp32-main (3)/xiaozhi-esp32-main/main/application.cc`
  - `HandleStateChangedEvent` → `kDeviceStateListening`
  - `OnIncomingAudio`
  - `tts` JSON `stop`
