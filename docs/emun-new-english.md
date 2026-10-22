# Chế độ tiếng Anh mới (Emun)

Tài liệu để mang sang **thiết bị khác** (cùng họ Otto/ESP32-S3 hoặc board mới): kiến trúc, identity, Gemini, server bài học, UI, API, MCP, NVS, và những bất biến không được phá.

Nhánh làm việc: `study-firmware`. HTML Self-Control nằm trong firmware (`main/boards/otto-robot/self_control.html`) — sửa HTML **phải flash** mới thấy trên `:8080`.

Không clone thương hiệu / Pro / Facebook của app mẫu. Layout học hỏi (pill nav, thẻ chủ đề ảnh phải) nhưng sản phẩm là **Emun**.

---

## 1. Ý đồ sản phẩm

Có **hai thế giới tách hẳn**, vì sau này có thể **xóa World Edu** mà app mới vẫn sống:

| | Tiếng Anh **cũ** | Tiếng Anh **mới** (sản phẩm chính) |
|---|---|---|
| UI | Desktop World Edu trên `:8080` | App điện thoại: Trang chủ / Lộ trình / Buddy / Bổ trợ / Xếp hạng / Hồ sơ |
| Identity | `preset_mac` 0–7 (chip, World Edu, custom, daily pool) | 3 slot riêng trong NVS `speak`: dạy học / giao tiếp hằng ngày / tự dán MAC |
| AI | Xiaozhi MQTT/WS **hoặc** Direct AI World Edu (`direct_ai`) | Overlay riêng: Gemini/OpenAI/Grok của slot đang chọn, **không** rơi xuống key World Edu |
| Giáo trình | Unit World Edu trong firmware | JSON + JPG trên máy LAN `:8765`, firmware **không** nhét full giáo trình |
| Mặc định khi vào mới | — | **Giao tiếp hằng ngày** (slot daily), không phải đang dạy thẻ |

Phần cứng dùng chung (đi, nhạc, servo, OTA, log) **không** phải nội dung học. App mới mở overlay robot khi cần, nút «← Về app học».

Nói «học tiếng Anh» / «về giao tiếp hằng ngày» phải **đóng phiên cũ, kết nối lại, chào** — cùng cảm giác đổi cấp độ World Edu (`ApplyDeviceIdentity`).

---

## 2. Kiến trúc tổng

```
 Điện thoại / PC
    │  http://<IP-robot>:8080     UI Self-Control (HTML nhúng firmware)
    │  POST /api/speak            đổi style, start/stop, identity, base URL
    ▼
 ESP32-S3 (Otto)
    │  SpeakLesson  NVS namespace "speak"   (≤16 KB, đừng nhét JSON giáo trình)
    │  DirectAiConfig overlay khi style=new
    │  SystemInfo::GetMacAddress() / Board::GetUuid() → MAC+Client-Id của slot đang chọn
    │
    ├─ Xiaozhi (backend=xiaozhi) ──MQTT/WebSocket──► server Xiaozhi (Device-Id = MAC slot)
    │
    └─ Gemini Live (backend=gemini) ──TLS WS DirectAiProtocol──► Google
           key/model/voice/instr lấy từ slot (dkey_/akey_), search=tắt
           MCP tools self.speak.* vẫn gọi được trong phiên Gemini

 Máy trong LAN (PC)
    python serve_8765.py
    http://<IP-PC>:8765/speak/a1/u01.json
    http://<IP-PC>:8765/speak/a1/u01/elephant.jpg
         ▲
         └── robot GET khi Start / ShowCurrent
         └── trình duyệt GET khi mở Lộ trình / lật thẻ
```

Nguyên tắc: **firmware chỉ giữ bookmark** (level, unit, index, identity). Đổi bài / thêm ảnh = sửa file trên PC, **không flash**, miễn path và field JSON không đổi.

---

## 3. Hai kiểu AI (Gemini vs Xiaozhi)

`DirectAiConfig::Load()` khi `SpeakLesson::UsesNewIdentity()`:

1. Đọc `ActiveAiBackendName()` của slot đang chọn (`dai_` dạy / `ddai_` daily / custom = luôn Xiaozhi).
2. Parse `gemini` | `openai` | `grok` → bật Direct AI, **key/model/voice/instructions của slot đó**.
3. Parse `xiaozhi` hoặc tên lạ → `kOff`: đi MQTT/WS Xiaozhi, **cấm** dùng key World Edu `direct_ai`.

Bất biến:

- Chế độ mới **không** đọc NVS `direct_ai` (key Gemini cũ của World Edu).
- Dạy học và giao tiếp **hai key / hai MAC**, trừ khi firmware copy daily → teach vì teach chưa cấu hình (xem §6).
- Direct AI instruction ≤ **2000 bytes UTF-8 (~600 chữ Việt)**. Prompt dạy thẻ nhét thêm `OttoSpeakDirectAiPrompt()` khi `on=true`.
- Gemini Live mặc định model `gemini-3.8-live`, giọng `Kore`. `search=false` ở overlay mới (googleSearch hay ăn quota 1011, cắt phiên đầu).

Khi đổi slot / start / daily: `DirectAiConfig::BumpGeneration()` rồi `Application::ApplyDeviceIdentity()`:

1. Đóng audio channel, `protocol_.reset()`.
2. Lấy MAC/Client-Id mới (`ActiveDeviceMac` / `ActiveClientId`).
3. `InitializeProtocol` (WS Xiaozhi hoặc Direct AI Gemini).
4. Fake wake `Hi,Jason` để server/Gemini chào.

Không gọi `ApplyDeviceIdentity` lúc `OnTalkIdle` (cắt câu tạm biệt).

---

## 4. Ba identity (slot) — không trộn World Edu

NVS `speak`, `ns` = slot:

| Slot | Tên JSON | MAC | Client-Id | AI |
|---|---|---|---|---|
| 0 | `teach` | `mac` | `uuid` | `dai` + `akey`/`amod`/`avoi`/`ains` |
| 1 | `daily` (mặc định) | `dmac` (locally administered, tự tạo) | `duid` | `ddai` + `dkey`/`dmod`/`dvoi`/`dins` |
| 2 | `custom` | `cmac` (user dán) | `cuid` | luôn Xiaozhi |

`es` = 1 → tiếng Anh mới; `es` = 0 → cũ, `SpeakLesson::Disarm()` khi lưu World Edu.

MAC daily tự sinh `MakeLocalMac()` (bit locally administered). Xiaozhi dạy học **bắt buộc dán MAC** nếu `dai=xiaozhi`. Gemini dạy học bắt buộc có `akey` (hoặc thừa kế `dkey`, §6).

World Edu `preset_mac` 0–7 **không** đụng 3 slot này. Xóa World Edu sau này: xóa UI cũ + `preset_mac`; app mới vẫn đọc `speak`.

---

## 5. Giáo trình LAN `:8765`

### 5.1 Chạy server

Thư mục pack hiện tại: `EMUN_SPEAK_A1_U01_U30_NEW_FLOW_FULL/`

```text
python serve_8765.py
```

- Bind `0.0.0.0:8765`, CORS `*`, `Cache-Control: no-store`.
- MIME: `.json` → `application/json`, `.jpg` → `image/jpeg`.
- **Chỉ một process** lắng nghe 8765. Hai server (pack cũ U01 + pack mới) → JSON/ảnh 404 lung tung.

Firmware mặc định `base` = `http://192.168.100.161:8765`. Đổi trên `:8080` → Cài đặt → Địa chỉ bài học (cùng LAN, IP máy chạy Python).

### 5.2 Cây file (bắt buộc)

```text
speak/
  a1/
    u01.json … u30.json
    u01/
      hello-whats-your-name.jpg
      elephant.jpg
      what-is-this-elephant.jpg
      …
    u02/
      apple.jpg
      …
```

URL firmware/UI:

```text
{base}/speak/{level}/{unit}.json          →  http://PC:8765/speak/a1/u01.json
{base}/speak/{level}/{unit}/{id}.jpg      →  http://PC:8765/speak/a1/u01/elephant.jpg
```

`level` luôn **lowercase** (`a1`). JSON field `"image": "https://cdn…"` **bị bỏ**; robot/UI tự ghép path local. Đổi ảnh = ghi đè JPG cùng `id`, không flash.

JPG pack hiện tại **240×240**. Decode trên ESP32, budget ~80 KB/file.

### 5.3 Schema một unit

```json
{
  "level": "a1",
  "unit": "u01",
  "theme": "animals",
  "title_en": "Animals",
  "title_vi": "Động vật",
  "goal_vi": "…",
  "cards": [
    {
      "id": "elephant",
      "type": "word",
      "en": "elephant",
      "vi": "con voi",
      "teacher": "Từ này là elephant. Con nhìn hình rồi nói elephant nhé."
    }
  ]
}
```

| Field | Bắt buộc | Ý nghĩa |
|---|---|---|
| `id` | có | Tên file JPG, không dấu, khớp thư mục unit |
| `type` | có | `word` / `phrase` / `ask` |
| `en` | có | Chữ trên thẻ / UI |
| `vi` | có | Phụ đề phụ huynh |
| `teacher` | có | Câu giáo viên **đọc đúng**, không bịa |

`type=ask`: robot nói `teacher`, **mọi input mic đều tính đúng** rồi `next` (không chấm phát âm).

### 5.4 Flow A1 hiện tại (30 unit)

- **U01** (32 thẻ): 2 câu chào `ask` → với 6 con vật: WORD → ASK → THIS IS → I LIKE → còn lại WORD.
- **U02–U30** (36 thẻ): 12 nhóm WORD → ASK → PHRASE.

Tổng pack: 1076 thẻ, 1076 JPG. Sửa giáo trình chỉ sửa JSON/JPG trên PC.

Bookmark tiến độ: NVS `lv` / `u` / `i` (index thẻ đã qua). Hết unit → tự `GoDaily()`.

---

## 6. Luồng dạy nói (firmware)

Class `SpeakLesson` (`speak_lesson.cc`).

```
start
  ├ inherit daily Gemini nếu teach chưa có MAC/key
  ├ GET {base}/speak/a1/u01.json
  ├ slot = teach, on=true
  ├ ApplyDeviceIdentity (nếu vừa rời daily/custom)
  └ intro_pending = true

Hi Jason / phiên mới
  └ OnNewTalkSession → intro

self.speak.card  (lượt 1)
  └ phase=intro, say = "Mình có nhiệm vụ dạy tiếng Anh…"
     AI CHỈ nói field say, KHÔNG show, KHÔNG dạy từ

trẻ đáp → OnAssistantTurnDone → intro_ready
self.speak.card + self.speak.show
  └ JPEG lên mặt robot, AI nói đúng field teacher, chờ mic

trẻ nói gì cũng được
  └ khen "Đúng rồi!" → self.speak.next → show thẻ mới

self.speak.off / daily / hết unit
  └ GoDaily: slot=daily, on=false, reconnect + chào
```

`Start()` nếu teach `xiaozhi` mà trống MAC, **và** daily đang Gemini có `dkey`: copy `ddai/dkey/dmod/dvoi` sang teach. Mục đích: phụ huynh chỉ cấu hình Gemini giao tiếp vẫn bấm Học được.

`GoDaily()` nếu **đã** daily và không đổi backend → **không** reconnect (tránh cắt Gemini đang nói).

Không POST `identity` Xiaozhi mặc định mỗi lần bấm Học — chỉ `action=start`. Lưu MAC/key chỉ ở màn Cài đặt.

---

## 7. MCP (Gemini và Xiaozhi cùng thấy)

| Tool | Khi nào gọi |
|---|---|
| `self.speak.status` | `on`, `phase`, `need_intro` |
| `self.speak.start` | Bé nói «học tiếng Anh» / «học bằng Gemini». Optional `backend`, `level`, `unit`, `index=0` học lại |
| `self.speak.card` | Lấy thẻ / intro |
| `self.speak.show` | Hiện JPG; bỏ qua nếu `phase=intro` |
| `self.speak.next` | Bé đã nói |
| `self.speak.off` | Về giao tiếp, giữ tiến độ thẻ |
| `self.speak.daily` | Về daily; optional `backend` |

Prompt Xiaozhi/Gemini dạy thẻ (rút gọn): intro chỉ nói `say`; sau đó `card`+`show`+`teacher`; mọi mic = đúng; về daily = `off`/`daily`; đổi mode = reconnect+chào.

---

## 8. HTTP robot `:8080`

UI: `GET /` → `self_control.html`.

API dạy nói: `GET/POST /api/speak`

POST `{"action":"…"}`:

| action | Việc |
|---|---|
| `style` | `style=new\|old`, optional `slot=teach\|daily\|custom` |
| `start` | `level`, `unit`, `index`, optional `backend` |
| `stop` / `new_daily` | Về giao tiếp |
| `daily_identity` | Lưu MAC/key Gemini daily rồi bật daily |
| `identity` | Lưu MAC/key dạy học (chỉ Cài đặt) |
| `new_custom` | MAC Xiaozhi slot custom |
| `base` | URL `:8765` |
| `unit` | JSON unit đang chọn (robot fetch hộ) |
| `show` / `next` / `reset` | Thẻ |
| `profile` | tuổi, sở thích |
| `subtitles` | phụ đề robot / STT |

Log: `GET /api/log`, `GET /api/log/boot`.

---

## 9. UI tiếng Anh mới

Body class: `english-new app-mode`. Cột ~430px, font Nunito/Segoe UI Rounded, nền `#f3f5f8`.

Thanh CŨ/MỚI: nút xoay nhỏ, **không** đẩy layout. World Edu (`#app_robot`) ẩn khi app-mode.

### 9.1 Bottom nav (pill, 6 mục)

Trang chủ · Lộ trình · Buddy · Bổ trợ · Xếp hạng · Hồ sơ

MAC / Gemini / URL `:8765` **không** nằm các màn này → **Cài đặt** (banner «Cài đặt» hoặc bánh răng).

### 9.2 Màn hình

**Trang chủ**

- Avatar + «Xin chào {tên}»
- Banner kết nối teal: đã kết nối Emun / đang học; nút Cài đặt; pill «Giao tiếp hằng ngày» (không reconnect nếu đã daily)
- Ngân hàng phản xạ (số thẻ đã nói)
- Báo cáo hôm nay: từ / câu
- Timeline «Hoạt động hôm nay» (TRÒ CHUYỆN cam, HỌC teal, «5 phút»)

**Lộ trình**

- Header tím, 3 chip: Giao tiếp A1 / Bổ trợ / STEM
- Thẻ «CON ĐANG HỌC CHỦ ĐỀ n» + ảnh bìa — bấm = `start`
- **Thẻ đã học**: từng card, chữ trái, JPG phải, tag WORD/ASK/PHRASE, dấu ✓
- Chủ đề khác: 30 unit, ảnh bìa phải, bấm = chọn unit (xem thẻ, chưa start)

**Buddy**

- Nhóm Trò chuyện: giao tiếp, nhập vai (start), đố vui
- Nhóm Giải trí: nhạc (overlay điều khiển), truyện thẻ

**Bổ trợ** — list hoạt động lớn (xem thẻ, học A1, phụ đề, đố vui, mốc giờ nói).

**Xếp hạng** — lượt / từ / câu, cột tuần, mốc 1–400 giờ nói (ước lượng từ thẻ). Không BXH mạng, không clone ranking thương mại.

**Hồ sơ** — tổng quan, 3 thành tựu, sở thích (text NVS `likes`).

**Cài đặt** — `speak_base`, Xiaozhi/Gemini dạy, Gemini daily, MAC custom, phụ đề, link điều khiển/OTA.

### 9.3 Màu / layout (để clone đúng trên board khác)

- Teal kết nối `#3b9ea4` / `#1aa7a1`
- Tím lộ trình `#6b4dff` → `#8a6bff`
- Nav pill `border-radius: 32px`, `position: fixed` giữa dưới, `width: min(402px, calc(100% - 28px))`
- `.topic-row img { order: 2 }` — **ảnh bên phải**
- Icon Buddy tròn 48px + chevron

HTML cũ World Edu giữ nguyên trong `#app_robot` để xóa sau này một khối.

---

## 10. NVS `speak` (đừng phình)

Namespace 16 KB. Chỉ bookmark + identity, không JSON unit.

| Key | Ý |
|---|---|
| `es` | 0 cũ / 1 mới |
| `ns` | 0 teach / 1 daily / 2 custom |
| `on` | đang dạy thẻ |
| `base` | URL `:8765` |
| `lv` `u` `i` | level, unit, index |
| `mac` `uuid` `dai` `akey` `amod` `avoi` `ains` | teach |
| `dmac` `duid` `ddai` `dkey` `dmod` `dvoi` `dins` | daily |
| `cmac` `cuid` | custom Xiaozhi |
| `age` `likes` | hồ sơ |
| `sub_r` `sub_u` | phụ đề |

Key Gemini ≤ 300 ký tự. Instruction slot ≤ 400 trong `SpeakLesson` (Direct AI tổng ≤ 2000 bytes).

---

## 11. File nguồn chính

| File | Vai trò |
|---|---|
| `main/boards/otto-robot/speak_lesson.cc/.h` | Bookmark, fetch JSON/JPG, slot, start/daily, inherit Gemini |
| `main/protocols/direct_ai_config.cc` | Overlay Gemini khi style=new |
| `main/protocols/direct_ai_protocol.cc` | Gemini Live WS + MCP tools |
| `main/application.cc` | `ApplyDeviceIdentity` reconnect + chào |
| `main/system_info.cc` / `main/boards/common/board.cc` | MAC / Client-Id theo slot |
| `main/boards/otto-robot/websocket_control_server.cc` | `/api/speak` |
| `main/boards/otto-robot/otto_controller.cc` | MCP `self.speak.*` |
| `main/boards/otto-robot/self_control.html` | UI cũ + app mới |
| `EMUN_SPEAK_A1_U01_U30_NEW_FLOW_FULL/` | JSON+JPG+`serve_8765.py` |
| `docs/direct-ai.md` | Direct AI (OpenAI/Grok/Gemini) lớp dưới |

---

## 12. Mang sang thiết bị khác — checklist

1. **Cùng LAN:** robot, điện thoại, PC `:8765`. Một process 8765. `base` = `http://<IP-PC>:8765` (không dùng `localhost` trên robot).
2. Flash firmware có `SpeakLesson` + HTML mới. NVS `speak` giữ style sau tắt nguồn.
3. `:8080` → Tiếng Anh **mới**. Mặc định daily.
4. Cài đặt → dán **Gemini API key giao tiếp** (và/hoặc Xiaozhi MAC daily). Lưu một lần.
5. Hi Jason → nói chuyện Gemini. Không bấm Lưu identity liên tục (cắt phiên).
6. Bấm thẻ tím «Con đang học» hoặc nói «học tiếng Anh» → robot fetch JSON+JPG, intro, rồi thẻ.
7. Lộ trình phải thấy JPG từ `:8765`. Nếu 404: sai `base`, sai `a1`/`A1`, hoặc server cũ U01-only.
8. World Edu / `direct_ai` key cũ **không** được dùng ngầm — nếu Gemini «đơ», kiểm tra đang overlay `dkey_` chứ không phải MQTT Xiaozhi trống MAC.
9. Hardware overlay (đi/nhạc/OTA) giữ nguyên; đừng nhét form identity vào Buddy.
10. Đổi giáo trình: thêm `u31.json` + folder JPG; firmware không cần flash nếu URL vẫn `speak/{lv}/{u}/{id}.jpg`.

---

## 13. Bẫy đã gặp (đừng lặp)

- POST `identity` với radio HTML mặc định **Xiaozhi** mỗi lần bấm Học/Chat → cắt Gemini, robot `:8080` timeout dù vẫn ping được.
- `GoDaily`/`saveDailyIdentity` reconnect khi **đã** daily → đơ giữa câu.
- UI lộ trình chỉ vẽ 30 chủ đề, **bỏ list thẻ** → mất ảnh / mất «đã học».
- Hai Python cùng 8765; path `speak/A1/` vs `speak/a1/` trên hệ case-sensitive.
- `ApplyDeviceIdentity` lần 2 trong lúc lần 1 đang chạy → `protocol_` đã reset, init bị skip → câm.
- Nhét full 1000 thẻ vào firmware / NVS 16 KB.

---

## 14. Hành vi người dùng (kỳ vọng)

1. Mở app mới → chào + banner kết nối + giao tiếp Gemini (sau khi lưu key).
2. Nói «học tiếng Anh» hoặc bấm lộ trình → intro một câu → ảnh thẻ trên mặt robot → bé nói mic → thẻ tiếp.
3. Nói «về giao tiếp» / hết unit → Gemini daily chào lại, tiến độ `index` còn.
4. Phụ huynh xem `:8080` Lộ trình: thẻ đã ✓, ảnh từ LAN, không cần tài khoản cloud giáo trình.
)
