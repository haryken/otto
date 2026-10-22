# Chế độ AI trực tiếp (OpenAI Realtime / Grok Voice / Gemini Live)

Tài liệu cho người (và AI agent) sửa code sau này: chế độ này làm gì, vì sao thiết kế như vậy,
và những bất biến không được phá. Nhánh: `direct-ai` (tách từ `master` `6a6b741`).

## Ý đồ của chủ dự án

- Robot Otto nói chuyện **thẳng** với OpenAI / xAI / Google bằng API key của chủ robot.
  **Không** dùng máy chủ trung gian (relay, wire-pod, server Xiaozhi tự host…). Đây là yêu cầu rõ
  ràng — đừng "đơn giản hóa" bằng cách thêm server.
- Chọn chế độ và nhập key ở trang Self-Control **web :8080**, tab "🤖 AI". Không có UI nào khác.
- Khi chọn "Xiaozhi" (mặc định), firmware phải chạy **y như trước**: cùng luồng OTA/kích hoạt,
  cùng MQTT/WebSocket protocol, không có hành vi mới nào.
- Trải nghiệm giống Xiaozhi: wake word → nói → robot trả lời → tự nghe tiếp; AI điều khiển được
  thân robot qua các MCP tool sẵn có; nói "tạm biệt" thì dừng.
- Song song có bản tương tự cho robot Vector (repo wire-os, `vic-cloud`); prompt, model, giọng mặc
  định được giữ giống bên đó.

## Tổng quan kiến trúc

```
 Mic → AFE (16 kHz PCM) ──raw PCM (bỏ Opus)──► DirectAiProtocol ──TLS WebSocket──► OpenAI / xAI / Gemini
                                                   │  ▲
                     tts/stt JSON giả lập server   │  │ audio base64 24 kHz, transcript, tool call
                                                   ▼  │
                                   Application (state machine Xiaozhi, KHÔNG đổi)
                                                   │
 Loa ◄── AudioService::EnqueuePlaybackPcm ◄────────┘ (resample về sample rate codec)
```

Ý tưởng chính: `DirectAiProtocol` là một `Protocol` như `WebsocketProtocol`/`MqttProtocol`.
Nó dịch sự kiện của AI thành **đúng JSON mà server Xiaozhi gửi**, nên `Application` dùng lại
nguyên state machine (Idle / Connecting / Listening / Speaking) mà gần như không phải sửa:

| Sự kiện AI | JSON phát cho Application |
|---|---|
| Bắt đầu có audio trả lời | `{"type":"tts","state":"start"}` |
| Câu trả lời (transcript) | `{"type":"tts","state":"sentence_start","text":…}` |
| Người dùng nói (transcript) | `{"type":"stt","text":…}` |
| Hết lượt trả lời | `{"type":"tts","state":"stop"}` |

## File

| File | Vai trò |
|---|---|
| `main/protocols/direct_ai_config.h/.cc` | Cấu hình trong NVS, JSON cho web (`ToWebJson`/`ApplyWebJson`), backend đang chạy, bộ đếm generation |
| `main/protocols/direct_ai_protocol.h/.cc` | Toàn bộ logic phiên AI: kết nối, setup từng backend, audio, tool, lượt nói |
| `main/application.cc` | `ActivationTask`: chế độ AI thì bỏ `CheckNewVersion()`, chỉ `MarkCurrentVersionValid()`. `InitializeProtocol`: tạo `DirectAiProtocol` trước mọi lựa chọn khác |
| `main/audio/audio_service.h/.cc` | `SetRawPcmUplink(bool)` + `PushPcmToSendQueue()`: AFE output đi thẳng vào send queue dạng PCM |
| `main/protocols/protocol.h` | `AudioStreamPacket::pcm` (payload là PCM 16-bit thay vì Opus) |
| `main/mcp_server.h/.cc` | `GetAiToolJsons()`: danh sách tool không phải user-only |
| `main/boards/otto-robot/websocket_control_server.h/.cc` | `GET/POST /api/ai` |
| `main/boards/otto-robot/self_control.html` | Tab "🤖 AI" |
| `main/CMakeLists.txt` | Thêm 2 file `.cc` |
| `components/78__esp-ml307/…/web_socket.h/.cc` | `close_reason()` (status line handshake lỗi / close frame). **Thư mục `components/` bị gitignore** — xem phần Lưu ý |

## Cấu hình (NVS namespace `direct_ai`)

| Key | Ý nghĩa |
|---|---|
| `backend` | `""` (Xiaozhi) / `openai` / `grok` / `gemini` |
| `lang` | `vi` (mặc định) / `en` / `auto` — gợi ý ngôn ngữ cho transcription |
| `instr` | Lời dặn thêm của chủ robot, ≤ 2000 byte UTF-8 (web giới hạn 600 ký tự) |
| `search` | Cho phép web search (Grok `web_search`, Gemini `googleSearch`) |
| `oa_*`, `gk_*`, `gm_*` | Theo backend: `_key` (≤ 300), `_model`, `_voice` (≤ 80). Rỗng = mặc định |

Mặc định: OpenAI `gpt-realtime-mini`/`coral`, Grok `grok-voice-latest`/`eve`,
Gemini `gemini-3.8-live`/`Kore`.

Ghi NVS qua class `NvsWriter` riêng (không dùng `Settings::SetString`) vì `Settings` **abort khi
lỗi NVS**, mà partition NVS chỉ 16 KB — đầy thì phải báo lỗi lên web chứ không được reboot.

### Web API

- `GET /api/ai` → `{backend, running, language, instructions, search, openai|grok|gemini:{has_key,
  key_hint, model, voice, default_model, default_voice}}`. **Không bao giờ trả key thật**, chỉ
  `key_hint` dạng `sk-…abcd`.
- `POST /api/ai` (≤ 4096 byte) → `{success, reboot, error}`. Mỗi backend nhận `{key (chỉ ghi khi
  không rỗng), clear_key, model, voice}`.
- Đổi **Xiaozhi ↔ AI** → reboot (đổi loại protocol và luồng khởi động). Đổi **AI ↔ AI** → không
  reboot: `RunningBackend` cập nhật, phiên mới đọc lại config (`OpenAudioChannel` gọi
  `DirectAiConfig::Load()` mỗi lần).
- Mỗi lần lưu tăng `Generation()`; worker thấy generation đổi (và robot không đang nói) thì đóng
  phiên để lần sau dùng cấu hình mới.

## Luồng khởi động ở chế độ AI

`ActivationTask` không gọi server OTA Xiaozhi (không check version, không mã kích hoạt, không
lấy cấu hình MQTT/WebSocket), chỉ đánh dấu firmware hợp lệ. Hệ quả: không có cập nhật tự động
từ server Xiaozhi, không có giờ server (`has_server_time_ = false`). Cập nhật firmware bằng tab
"🔄 Cập nhật" trên web.

## Phiên (session)

- `Start()` không kết nối gì. Kết nối khi `OpenAudioChannel()` (wake word / nút).
- `Connect()`: tạo WebSocket TLS (cert bundle FULL), gửi setup, chờ `session.updated`
  (OpenAI/Grok) hoặc `setupComplete` (Gemini) tối đa 10 s. Lỗi → `SetError("AI: …")` hiện lên
  màn hình (key sai, hết quota, model sai…).
- Web search bật mà phiên bị từ chối → thử lại không có search và nhớ `no_search_[backend]`
  (trong RAM) — key miễn phí thường không có quyền search.
- Wake word: `ContinueWakeWordInvoke` gửi `SendWakeWordDetected("xin chào")` (với "Hi,Jason"/
  "Hi,ESP"; cần `CONFIG_SEND_WAKE_WORD_DATA=y`, đang bật) → protocol gửi như tin nhắn người dùng
  → AI chào trước.
- Silence prompt của Application (`"khơi gợi lại"`, sau ~30 s im lặng): lần 1 đổi thành câu nhắc
  AI gợi chuyện; lần 2 liên tiếp → đóng phiên.
- Đóng dự phòng: ở Listening 90 s không có hoạt động → đóng (phiên mở là tốn tiền).
- Tool nội bộ `self.conversation.end`: AI gọi khi người dùng tạm biệt → chờ phát hết lời chào
  → đóng phiên (giống server Xiaozhi gửi goodbye).

## Từng backend

**OpenAI Realtime** — `wss://api.openai.com/v1/realtime?model=…`, header `Authorization: Bearer`.
- Schema GA (`session.type = "realtime"`, `audio.input/output`). Model có chữ `preview` → schema
  beta + header `OpenAI-Beta: realtime=v1`.
- `server_vad` 700 ms, `interrupt_response: false` (mic tắt khi robot nói; ngắt lời bằng wake
  word), transcription `gpt-4o-mini-transcribe`, noise reduction `far_field`. Không có web search.

**Grok Voice** — `wss://api.x.ai/v1/realtime?model=…`. Giống Realtime nhưng `voice`/
`turn_detection` nằm ở cấp session, `reasoning.effort = none`, `transcription.language_hint`,
tool `{"type":"web_search"}` nếu bật. Delta audio có thể nằm ở field `audio` thay vì `delta`.

**Gemini Live** — `wss://generativelanguage.googleapis.com/ws/…BidiGenerateContent`, header
`x-goog-api-key`.
- Uplink 16 kHz trực tiếp (`realtimeInput.audio`, `audio/pcm;rate=16000`), không resample.
- `automaticActivityDetection.silenceDurationMs = 700`, input/output transcription,
  `contextWindowCompression.slidingWindow` (không có thì phiên audio chết sau ~15 phút),
  `sessionResumption` (handle giữ trong RAM).
- Model chứa `2.5` → `thinkingBudget: 0` (Gemini 3 từ chối field này).
- Tool: `behavior: "BLOCKING"`, schema lọc còn tập con Gemini chấp nhận (`GeminiSchema`), bỏ
  `parameters` nếu không có property.

## Audio

- **Uplink**: khi `DirectAiProtocol` tồn tại, `AudioService` ở chế độ raw PCM: frame 60 ms từ AFE
  vào `audio_send_queue_` với `pcm = true` (đầy thì bỏ gói cũ nhất). OpenAI/Grok: resample
  16k→24k (`esp_ae_rate_cvt`), base64, `input_audio_buffer.append`. Gói Opus (dữ liệu wake word)
  bị bỏ qua trong `SendAudio`.
- **Downlink**: base64 PCM (24 kHz, Gemini lấy rate từ mimeType) → resample về rate của codec →
  `EnqueuePlaybackPcm` từng khúc 60 ms.
- **Cổng phát (gate)** — quan trọng: vào Speaking thì `Application` gọi `ResetDecoder()` (xóa hàng
  đợi phát). Nếu đẩy audio trước khi state change được xử lý thì câu đầu bị mất. Vì vậy
  `BeginSpeaking()` phát `tts start` rồi `Schedule` **hai lần** (lần 2 chạy sau handler state
  change) mới mở gate; worker chờ gate tối đa 3 s.
- Hết lượt: phát `tts stop` ngay sau khúc audio cuối; Application (AutoStop) chuyển Listening và
  `WaitForPlaybackQueueEmpty()` trước khi mở mic, nên không cắt đuôi câu.

## Lượt nói (turn)

- **Realtime**: kết thúc khi `response.done` mà không còn tool đang chạy và không cần follow-up.
  Tool xong → gửi `function_call_output` → `response.create` (follow-up).
- **Gemini**: kết thúc ở `turnComplete`. Sau khi gửi `toolResponse` Gemini thường nói tiếp trong
  cùng lượt nên có cửa sổ chờ 4 s (`kToolGraceUs`). `interrupted` → kết thúc lượt ngay (lượt bị
  ngắt không có `turnComplete`).
- **Abort** (wake word khi đang nói): `muted_ = true`, Realtime gửi `response.cancel`, xóa hàng đợi
  phát, phát `tts stop`; audio đến sau đó bị bỏ cho tới khi lượt cũ kết thúc thật.

## Tool (MCP)

- Mọi tool không phải user-only của `McpServer` được khai báo làm function. Tên `.` ↔ `__`
  (OpenAI chỉ cho `[a-zA-Z0-9_-]`).
- Gọi tool: tạo JSON-RPC `tools/call` (id từ 1000000) → `McpServer::ParseMessage`. Tool **chạy
  trên main task** (`Application::Schedule`), kết quả quay lại qua `SendMcpMessage` → inbox worker
  → gửi cho AI.

## Luồng (thread) và bất biến — đừng phá

| Thread | Làm gì |
|---|---|
| Task nhận SSL của esp-ml307 (stack 4 KB) | Chỉ copy message vào inbox (`Push`). Không parse, không chặn |
| Worker `direct_ai` (stack 8 KB **trong PSRAM**) | Parse JSON, phát audio, xử lý tool/lượt, `Tick()` 200 ms |
| Main task | `OpenAudioChannel`/`CloseAudioChannel`, `SendAudio`, abort, chạy tool |

- Worker có stack PSRAM → **không được đụng flash/NVS** trong worker (chỉ đọc atomic
  `Generation()`). Tool chạy trên main nên an toàn.
- `ws_` luôn truy cập dưới `ws_mutex_`. `conn_id_` tăng mỗi lần kết nối/đóng; message của kết nối
  cũ bị bỏ.
- Lambda `Schedule` giữ `std::weak_ptr` tới `alive_` — protocol có thể bị hủy trước khi lambda chạy.
- Inbox giới hạn 6 MB; đầy thì chỉ bỏ message có `delta`/`inlineData` (audio, transcript từng
  mẩu), không bỏ message điều khiển (`response.done`, tool call, `turnComplete`…).
- WebSocket gửi tối đa 65535 byte/frame (giới hạn esp-ml307) — setup quá lớn thì báo lỗi.

## Lưu ý / bẫy đã gặp

- `components/78__esp-ml307` là bản copy local của component (được `dependencies.lock` trỏ tới) và
  **bị gitignore**. Bản vá `close_reason()` nằm ở đó nên không vào git. `direct_ai_protocol.cc`
  gọi qua `CloseReason()` dùng `if constexpr (requires …)` → clone mới vẫn build được, chỉ mất
  thông báo lỗi chi tiết.
- Giới hạn độ dài lời dặn tính theo **byte** ở firmware nhưng theo **ký tự** ở HTML; tiếng Việt
  2–3 byte/ký tự, nên web 600 ký tự ↔ firmware 2000 byte. Đổi một bên thì đổi cả hai.
- Partition app còn ~131 KB sau khi thêm tính năng (build 0x3cfdb0 / 0x3f0000).
- Không đổi `Settings` thành ghi bằng `NvsWriter` cho cả project — chỉ phần AI cần báo lỗi lên web.

## Hạn chế đã biết / chưa làm

- **Chưa test trên robot thật** lúc viết tài liệu này (chỉ build pass).
- Chế độ listening Realtime (AEC trên thiết bị): mic vẫn mở khi robot nói, server VAD có thể bắt
  tiếng robot; chưa xử lý riêng (Otto mặc định AutoStop).
- Không có trí nhớ giữa các phiên (Gemini resumption chỉ trong RAM, mất khi reboot); không có
  camera/vision; OpenAI không có web search.
- Chưa có nút "kiểm tra key" trên web; lỗi key chỉ thấy khi bắt đầu trò chuyện.

## Build / thử

```
cmd.exe /c "call C:\Espressif\idf_cmd_init.bat esp-idf-20ee62e792ea89630ac6a777ab3ebc57 >nul 2>&1 && cd /d C:\Users\LEGION\Desktop\xiaozhi-esp32-main && idf.py build"
```

Nạp `build/xiaozhi.bin` qua tab "🔄 Cập nhật" (web :8080) hoặc `idf.py flash`. Vào tab "🤖 AI",
chọn nhà cung cấp, dán key, Lưu (robot reboot nếu từ Xiaozhi sang). Log tag: `DirectAI`,
`DirectAiConfig`, `WebSocket`.
