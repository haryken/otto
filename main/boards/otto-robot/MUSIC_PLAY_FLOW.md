# Otto music play — luồng & vì sao “kêu phát” mà không nghe nhạc

**Phạm vi:** chỉ phân tích / log luồng nhạc (`OttoMusic`, MCP `self.otto.music.*`).  
**Không sửa** luồng giao tiếp (WS/MQTT chat, STT/TTS, listening/speaking state machine) — đang ổn.

---

## Kết luận nhanh (theo log bạn gửi)

Log có:

```text
<< % self.otto.music.play...
Abort speaking
speaking -> idle
Music-only mode: standby, wake word or BOOT to stop
```

**Không có** bất kỳ dòng `OttoMusic:` / `MCP self.otto.music.play` nào.

Ý nghĩa:

1. Cloud **đọc TTS** chuỗi có chữ `self.otto.music.play` → firmware vào **Music-only mode** (đóng kênh thoại, standby).
2. Tool MCP thật **hầu như chưa chạy** (hoặc chưa tới lúc tạo task `otto_music`) → **không search / không stream MP3** → không có tiếng nhạc.

Music-only ≠ đã phát nhạc. Nó chỉ “nhường loa / tắt uplink thoại” để chờ (hoặc giả định) luồng nhạc local.

---

## Hai đường độc lập (dễ nhầm)

```text
Người dùng: "phát bài phép màu"
        │
        ▼
   Cloud LLM
        │
        ├── A) TTS sentence chứa "% self.otto.music.play..."
        │         → Application (sentence_start) thấy substring
        │         → SetLocalPlaybackActive + EnterMusicOnlyMode
        │         → Abort speaking, CloseAudioChannel, idle + wake word
        │         ❌ Đây KHÔNG gọi OttoMusic::PlayFirstSearchResult
        │
        └── B) MCP JSON type=mcp / tools/call name=self.otto.music.play
                  → McpServer::DoToolCall (Schedule main thread)
                  → OttoController tool → OttoMusic::PlayFirstSearchResult
                  → task "otto_music" → search youtube.kytuoi.com
                  → stream /api/stream/mp3 → decode → EnqueuePlaybackPcm
                  ✅ Đây mới là phát nhạc thật
```

Với log hiện tại: **chỉ thấy A**, **không thấy B**.

---

## Luồng phát nhạc thật (B) — file & bước

| Bước | File | Việc |
|---|---|---|
| 1 | `otto_controller.cc` | Đăng ký MCP `self.otto.music.play` / `.stop` |
| 2 | `mcp_server.cc` | `tools/call` → `DoToolCall` → `Schedule` → `Call` |
| 3 | `otto_music_player.cc` `PlayFirstSearchResult` | Tạo task `otto_music` (trả JSON `status:starting` ngay) |
| 4 | `SearchAndPlayTask` | `GET https://youtube.kytuoi.com/api/search?q=...&limit=1` |
| 5 | `StreamMp3Task` | `GET .../api/stream/mp3?id=...&format=mp3` → MP3 decode → PCM loa |
| 6 | (trong stream) | Gọi lại `EnterMusicOnlyMode` / hiện “Đang phát nhạc” |

API host: `youtube.kytuoi.com` (HTTP connect id = **2**, tránh đụng WS id 1).

---

## Vì sao log của bạn khớp “không phát”

| Hiện tượng | Giải thích |
|---|---|
| `<< % self.otto.music.play...` | TTS / “công bố tool” dạng text, **không** phải log MCP |
| `Music-only mode: standby...` | Nhánh A trong `application.cc` (cấm đụng ở đợt này) |
| Không có `OttoMusic:` | `PlayFirstSearchResult` chưa được gọi |
| `httpd_sock_err ... 104` | Client Self-Control `:8080` disconnect — **không liên quan** phát nhạc |

Giả thuyết chính (cần xác nhận bằng log mới):

1. Cloud **chỉ TTS tên tool**, quên / không gửi `tools/call`, hoặc gửi sau khi kênh đã đóng.
2. Ít gặp hơn: `tools/call` có nhưng fail trước khi vào OttoMusic (sẽ thấy `MCP:` / `tools/call:` error).

---

## Log đã thêm (chỉ nhạc) — sau khi flash build này

Filter serial: `OttoMusic` hoặc `MCP self.otto.music`.

### Nếu MCP chạy đúng (mong đợi)

```text
MCP self.otto.music.play query="..."
OttoMusic: PlayFirstSearchResult called ...
OttoMusic: PlayFirstSearchResult: otto_music task started ...
OttoMusic: SearchAndPlayTask enter ...
OttoMusic: HTTP GET begin: https://youtube.kytuoi.com/api/search?...
OttoMusic: Search hit id=... title="..."
OttoMusic: StreamMp3Task enter ...
OttoMusic: Streaming: ... 
```

### Nếu vẫn chỉ Music-only như lần này

```text
Music-only mode: standby...
(không có MCP self.otto.music.play / OttoMusic: PlayFirstSearchResult)
```

→ Vẫn là **cloud không gọi tool**; firmware nhạc chưa được kích hoạt. Sửa phía prompt/agent cloud hoặc thứ tự MCP vs TTS — **không** sửa luồng chat trong đợt debug này.

### Nếu MCP có nhưng search/stream lỗi

Sẽ thấy `Search failed` / `HTTP open failed` / `Stream HTTP ...` / `No MP3 frame sync`.

---

## Việc **không** làm trong đợt này

- Không đổi `application.cc` nhánh `sentence_start` / `EnterMusicOnlyMode` / abort speaking.
- Không đổi protocol WS/MQTT chat, STT, silence prompt.
- Không “vá tạm” bằng cách tự gọi `PlayFirstSearchResult` từ TTS text (sẽ đụng luồng giao tiếp / dễ double-play).

Khi đã có log mới mà vẫn fail ở HTTP/search, mới xử lý riêng trong `otto_music_player.cc`.
