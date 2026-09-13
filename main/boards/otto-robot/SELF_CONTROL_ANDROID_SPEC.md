# Brief: Self-Control web `:8080` trên Xiaozhi Android

Spec để agent port **đúng hành vi** Self-Control đang chạy ổn trên Otto ESP32 sang source Xiaozhi Android.

Tham chiếu Otto (repo này):

| File | Vai trò |
|---|---|
| `self_control.html` | UI trang cấu hình |
| `websocket_control_server.cc` | HTTP `GET /`, `GET/POST /api/config` |
| `otto_course_units.h` | Danh sách sách con + unit |
| `device_identity_presets.h` | MAC preset 1–5 + pool daily chat |
| `application.cc` → `ApplyDeviceIdentity()` | Đổi Device-Id nhanh + auto chào |
| `otto_controller.cc` | MCP tools Self-Control |

---

## Mục tiêu

1. HTTP server LAN **port 8080** phục vụ trang cấu hình (mở bằng IP; QR nếu có màn hình).
2. UI chọn **khóa học / Unit / tên / MAC** **giống hệt** Otto (`self_control.html`): cùng layout, cùng cách đổi cấu hình, cùng URL `http://<LAN_IP>:8080`.
3. Voice / MCP tools Self-Control giống Otto.
4. Đổi khóa (= đổi Device-Id/MAC) **nhanh, không reboot, không màn đăng nhập**, rồi **tự chào** như vừa gọi wake word.
5. Hỗ trợ đổi khóa trên **cả WebSocket và MQTT** (Android có cả hai protocol).

---

## Khái niệm cốt lõi

- **Device-Id** = MAC khóa học gửi lên cloud (header WebSocket / MQTT client identity). Đây là thứ quyết định agent/khóa trên server.
- **Client-Id** = UUID máy — **random mỗi boot** và **mỗi lần đổi khóa A→B** (tránh server Xiaozhi timeout/spam). Không giữ UUID cố định khi đổi MAC liên tục.
- Đổi khóa = đổi Device-Id đang dùng + random Client-Id mới, đóng session cloud cũ, mở session mới.

---

## Danh sách khóa (`preset_mac_idx` 0..7)

| idx | id | Tên UI | Device-Id / MAC | Có unit? |
|---|---|---|---|---|
| 0 | custom | Tự cấu hình | MAC chip; hoặc `custom_mac` nếu nhập | Không |
| 1 | explorers | Explorers | 2 giọng × pool 10 MAC: Song ngữ `ba:10–19`, English `fe:10–19` | Có — **5 sách con** |
| 2 | younginnovators | Young Innovators | 2 giọng × pool 10: Việt `ef:10–19`, Anh `c6:fe:10–19` (riêng Explorers) | Có — **3 sách QUEST** |
| 3 | futureleaders | Future Leaders | 2 giọng × pool 10: Việt `c7:ef:10–19`, Anh `c8:ef:10–19` | Có — **1 sách Summit** |
| 4 | ielts | IELTS | 2 giọng × pool 10: Việt `c2:ef:10–19`, Anh `c3:ef:10–19` | Mock Unit 1–6 |
| 5 | toeic | TOEIC | 2 giọng × pool 10: Việt `c9:ef:10–19`, Anh `c1:ef:10–19` | Mock Unit 1–6 |
| 6 | manual_mac | Tự nhập MAC | Bắt buộc `custom_mac` `aa:bb:cc:dd:ee:ff` | Không |
| 7 | daily_chat | Giao tiếp hằng ngày | Random 1/20 MAC trong pool; mỗi lần boot (hoặc lần chọn mới) random lại, ghi `custom_mac` | Không |

**Mặc định** khi chưa có cấu hình: `preset_mac_idx = 1` (Explorers).

### Client-Id (UUID) — random bắt buộc (anti spam server)

**Client-Id** = UUID v4 (header WS / payload OTA), **khác** Device-Id (MAC).

Xiaozhi cloud dễ timeout / coi spam nếu cùng Client-Id gắn nhiều session đổi MAC liên tục. Vì vậy:

| Thời điểm | Hành vi |
|---|---|
| **Mỗi lần boot / mở app** | `RegenerateUuid()` → ghi NVS → Client-Id mới (giữ Device-Id theo MAC đang lưu / vừa pick từ pool) |
| **Đổi cấu hình khóa A → B** (web Save hoặc `set_course`) | Trong `ApplyDeviceIdentity`: **random Client-Id mới** trước khi mở session cloud mới |
| Chỉ đổi tên / unit / sách con | **Không** random Client-Id |

WebSocket: mỗi lần `OpenAudioChannel` gửi header `Client-Id` = UUID mới.  
MQTT: patch `client_id` theo MAC như cũ; Client-Id UUID vẫn dùng cho hello/OTA/header nếu codebase có.

### Explorers (idx=1) — 2 giọng + pool MAC

Field `ex_voice` (NVS `wifi/ex_voice`):

| `ex_voice` | Giọng UI | Pool |
|---|---|---|
| **0** (mặc định) | Song ngữ Anh – Việt | `ba:53:9e:c5:ba:10` … `ba:19` |
| **1** | Tiếng Anh | `ba:53:9e:c5:fe:10` … `fe:19` |

| Thời điểm | Hành vi |
|---|---|
| **Boot** mà `preset_mac_idx == 1` | Random 1 MAC trong pool theo `ex_voice` → `custom_mac` |
| **Đổi sang** idx 1 | Random pool theo `ex_voice` → ApplyDeviceIdentity |
| **Đổi `ex_voice`** khi đang idx 1 | Random lại pool giọng mới → ApplyDeviceIdentity |
| Chỉ sửa tên/unit/sách con | Không random lại MAC |

#### Pool Song ngữ (`ex_voice=0`)

```
ba:53:9e:c5:ba:10 … ba:53:9e:c5:ba:19
```

#### Pool Tiếng Anh (`ex_voice=1`)

```
ba:53:9e:c5:fe:10 … ba:53:9e:c5:fe:19
```

### Young Innovators (idx=2) — 2 giọng + pool MAC (riêng Explorers)

Field `yi_voice` (NVS `wifi/yi_voice`) — **không dùng chung** `ex_voice`.

| `yi_voice` | Giọng UI | Pool |
|---|---|---|
| **0** (mặc định) | Tiếng Việt | `ba:53:9e:c5:ef:10` … `ef:19` |
| **1** | Tiếng Anh | `ba:53:9e:c6:fe:10` … `fe:19` |

| Thời điểm | Hành vi |
|---|---|
| **Boot** mà `preset_mac_idx == 2` | Random 1 MAC theo `yi_voice` → `custom_mac` |
| **Đổi sang** idx 2 | Random pool theo `yi_voice` → ApplyDeviceIdentity |
| **Đổi `yi_voice`** khi đang idx 2 | Random lại pool giọng mới → ApplyDeviceIdentity |

### Future Leaders (idx=3) — 2 giọng + pool MAC

Field `fl_voice` (NVS `wifi/fl_voice`) — riêng với `ex_voice` / `yi_voice`.

| `fl_voice` | Giọng UI | Pool |
|---|---|---|
| **0** (mặc định) | Tiếng Việt | `ba:53:9e:c7:ef:10` … `ef:19` |
| **1** | Tiếng Anh | `ba:53:9e:c8:ef:10` … `ef:19` |

| Thời điểm | Hành vi |
|---|---|
| **Boot** mà `preset_mac_idx == 3` | Random 1 MAC theo `fl_voice` → `custom_mac` |
| **Đổi sang** idx 3 | Random pool theo `fl_voice` → ApplyDeviceIdentity |
| **Đổi `fl_voice`** khi đang idx 3 | Random lại pool giọng mới → ApplyDeviceIdentity |

### IELTS (idx=4) — 2 giọng + pool MAC

Field `ielts_voice` (NVS `wifi/ielts_voice`).

| `ielts_voice` | Giọng UI | Pool |
|---|---|---|
| **0** (mặc định) | Tiếng Việt | `ba:53:9e:c2:ef:10` … `ef:19` |
| **1** | Tiếng Anh | `ba:53:9e:c3:ef:10` … `ef:19` |

| Thời điểm | Hành vi |
|---|---|
| **Boot** mà `preset_mac_idx == 4` | Random 1 MAC theo `ielts_voice` → `custom_mac` |
| **Đổi sang** idx 4 | Random pool theo `ielts_voice` → ApplyDeviceIdentity |
| **Đổi `ielts_voice`** khi đang idx 4 | Random lại pool giọng mới → ApplyDeviceIdentity |

### TOEIC (idx=5) — 2 giọng + pool MAC

Field `toeic_voice` (NVS `wifi/toeic_voice`).

| `toeic_voice` | Giọng UI | Pool |
|---|---|---|
| **0** (mặc định) | Tiếng Việt | `ba:53:9e:c9:ef:10` … `ef:19` |
| **1** | Tiếng Anh | `ba:53:9e:c1:ef:10` … `ef:19` |

| Thời điểm | Hành vi |
|---|---|
| **Boot** mà `preset_mac_idx == 5` | Random 1 MAC theo `toeic_voice` → `custom_mac` |
| **Đổi sang** idx 5 | Random pool theo `toeic_voice` → ApplyDeviceIdentity |
| **Đổi `toeic_voice`** khi đang idx 5 | Random lại pool giọng mới → ApplyDeviceIdentity |

### Giao tiếp hằng ngày (idx=7) — random 1/20 MAC (bắt buộc)

Đây là **MAC (Device-Id)**, không phải IP WiFi.

Mục đích: mỗi lần vào khóa “Giao tiếp hằng ngày” (hoặc mỗi lần boot khi đang chọn khóa này) máy lấy **ngẫu nhiên 1 MAC** trong pool 20 cái bên dưới, ghi vào `custom_mac`, rồi dùng MAC đó làm Device-Id khi nói chuyện với cloud (quota/xoay máy ảo).

#### Khi nào phải random MAC (idx=7)

| Thời điểm | Hành vi |
|---|---|
| **Boot / mở app** mà `preset_mac_idx == 7` | Random lại 1/20 MAC → ghi `custom_mac` → Device-Id = MAC đó |
| **Save web / `set_course`** đổi **sang** idx 7 | Random 1/20 → ghi `custom_mac` → **ApplyDeviceIdentity** (đóng phiên cũ, mở mới, random Client-Id, tự chào) |
| Đang idx 7, chỉ sửa tên | **Không** random lại MAC |
| Đổi **từ** idx 7 sang khóa khác | Dùng MAC/pool của khóa mới; random Client-Id khi Apply |

#### Cách random & lưu (idx=7)

1. Chọn đều ngẫu nhiên 1 phần tử trong pool 20 MAC (copy nguyên list dưới).
2. Normalize dạng `aa:bb:cc:dd:ee:ff` (lowercase).
3. Ghi storage: `preset_mac_idx = 7`, `custom_mac = <mac vừa pick>`.
4. Runtime `GetMacAddress()` khi idx=7 **phải đọc `custom_mac`**, không dùng MAC chip.
5. Nếu vừa đổi sang 7: sau khi lưu → **ApplyDeviceIdentity** (WS + MQTT) + **RegenerateUuid**.

#### Pool daily chat (20 MAC) — copy nguyên đúng từng dòng

```
1c:db:d4:b5:73:3c
58:a0:23:a6:fe:31
a8:b5:44:dd:e3:cf
1c:db:d4:b5:74:7c
1c:db:d4:b5:6a:d8
1c:db:d4:b5:74:54
1c:db:d4:b5:72:ec
1c:db:d4:b5:71:d4
1c:db:d4:b5:74:d4
1c:db:d4:a9:48:84
1c:db:d4:a9:59:b4
1c:db:d4:a9:5b:a0
28:df:eb:02:6c:7d
bc:fc:e7:8a:d8:06
dc:b4:d9:0c:a4:9c
dc:b4:d9:0c:a4:80
dc:b4:d9:0c:a6:00
dc:b4:d9:03:4e:f4
dc:b4:d9:0c:a5:38
dc:b4:d9:03:43:38
```

---

## Cấu trúc sách / Unit (bắt buộc giống Otto)

- Unit chọn **1 cái (radio)**, không multi-check.
- Mỗi khóa lưu unit **riêng**. Đổi sách con **không** đổi MAC. Đổi khóa 1↔2↔… mới đổi Device-Id.

### Explorers (idx=1)

5 sách con (`ex_sub` 0..4), mỗi sách nhớ unit riêng (`ex_u0`…`ex_u4`, mặc định `"1"`).

1. **Pre-Discovery A** (8): Unit Routines; Unit Welcome The Magic Forest; Unit One Let's Draw; Unit Two Let's Play; Unit Review One; Unit Three The Big Monster; Unit Special Day One; Unit Special Day Two
2. **Pre-Discovery B** (11): Unit Routines Review; Unit Four My Family; Unit Review Two; Unit Five Where's My Bird?; Unit Midterm Test; Unit Six Let's Tidy Up!; Unit Review Three; Unit Special Day Three; Unit Special Day Four; Unit Review Four; Unit Final Test
3. **PRE-SPARK A** (8): Unit Routines; Unit Welcome Let's Remember; Unit One The Surprise; Unit Two The Brown Mouse; Unit Review One; Unit Three Where Is Greenman?; Unit Special Day One; Unit Special Day Two
4. **SPARK Two A** (5): Unit Our Friends; Unit One Weather; Unit Two Our Families; Unit Three Our Places; Unit Four Our Colors and Shapes
5. **SPARK Three A** (6): Unit Hello Again; Unit One Our Day; Unit Two Dinner Time; Unit Three With My Friends; Unit Four Our Colors and Shapes; Unit Four Our Faces

### Young Innovators (idx=2)

3 QUEST (`yi_sub` 0..2), unit riêng (`yi_u0`…`yi_u2`).

1. **QUEST One A** (6): Unit One Hello; Unit Two My School; Unit Three Favourite Toys; Unit Four My Family; Unit Five Our Pets; Unit Six My Face
2. **QUEST Two B** (6): Unit Seven At the Farm; Unit Eight My Town; Unit Nine Our Clothes; Unit Ten Our Hobbies; Unit Eleven My Birthday; Unit Twelve Our Holiday
3. **QUEST Three A** (8): Unit Hello; Unit One Family Matters; Unit Two Home Sweet Home; Unit Values One and Two; Unit Three A Day in the Life; Unit Four In the City; Unit Values Three and Four; Unit YLE Movers Listening Skills Practice

### Future Leaders (idx=3)

**Summit One A** (`fl_sub`, `fl_u0`) — 7 unit:

Unit Welcome; Unit One Having a Good Time; Unit Two Spending Money; Unit Three We Are What We Eat; Unit Four All in the Family; Unit Five No Place Like Home; Unit Six Friends Forever

### IELTS / TOEIC (idx 4 / 5)

Mock Unit 1..6; lưu `units_4` / `units_5` (string số unit, mặc định `"1"`).

Có chọn giọng Việt/Anh (`ielts_voice` / `toeic_voice`) + random MAC pool như Future Leaders.

### idx 0 / 6 / 7

Không hiện Unit.

---

## Trang web Self-Control (LAN `:8080`)

### Bắt buộc giống Otto 100% (UI + IP + đổi cấu hình)

Agent **không được tự thiết kế lại** trang. Phải clone hành vi/layout từ `self_control.html` của Otto.

| Hạng mục | Yêu cầu |
|---|---|
| **URL mở trang** | `http://<LAN_IP>:8080` — IP WiFi LAN của máy (Android/robot), port **8080** cố định |
| **QR / voice** | `show_config_page` trả đúng URL trên; nếu có màn thì hiện QR cùng URL đó (giống Otto) |
| **Layout** | Mobile-first: card trắng nền `#f4f6f8`, tiêu đề, toast góc trên, nút Lưu xanh full-width |
| **Thứ tự card** | 1) Tên học viên → 2) Cấp độ học → 3) Địa chỉ MAC (Device-Id) → 4) Sách con (ẩn/hiện) → 5) Chọn Unit → 6) Lưu |
| **Cấp độ** | Radio 0..7, hiện tên + MAC preset cạnh tên (idx 1..5); đổi radio → sách/unit đổi **realtime**, chưa Lưu thì chưa ghi server |
| **MAC field** | Ô `custom_mac` + hint + dòng monospace `Device-Id đang dùng: …` |
| **Unit** | Radio **1 unit**; label tick xanh khi chọn |
| **API** | `GET/POST /api/config` đúng shape Otto; CORS `Access-Control-Allow-Origin: *` nếu cần |
| **Toast Save** | Đổi khóa/MAC: `Đã lưu — đang đổi Device-Id (~vài giây), rồi nói tiếp` rồi `loadConfig` lại sau ~3s; chỉ tên/unit: `Đã lưu tên / unit` |
| **Server 8080** | Đổi khóa **không** tắt web; điện thoại giữ tab được |

**IP:** người dùng mở trang bằng IP LAN (`192.168.x.x:8080`), không dùng localhost trên điện thoại khác máy. Không đổi port. Không thay bằng HTTPS trừ khi Otto cũng vậy.

**Nguồn copy:** ưu tiên nhúng/adapt nguyên `main/boards/otto-robot/self_control.html` (đổi title brand nếu cần, nhưng cấu trúc + field + logic Save giữ nguyên).

### Endpoints

| Method | Path | Mô tả |
|---|---|---|
| `GET` | `/` | HTML trang cấu hình (mobile-friendly) — clone Otto |
| `GET` | `/api/config` | JSON hiện trạng |
| `POST` | `/api/config` | JSON body lưu cấu hình |

### UI chi tiết (y như Otto)

1. Ô **Tên học viên** (`#student_name`)
2. Radio **Cấp độ học** (0..7) — đổi radio → UI unit/sách con đổi **realtime** (chưa Lưu thì chưa ghi storage / chưa đổi MAC)
3. Card **MAC / Device-Id**: ô `#custom_mac` + `#mac_hint` + `#device_id_view` (“Device-Id đang dùng: …”)
4. Card **Sách con** `#sub_card` (chỉ khi khóa có sub)
5. Card **Chọn Unit** `#unit_card` (radio 1 unit)
6. Nút **Lưu cấu hình** `#btn_save` + toast `#toast` + `#status`

### Hành vi MAC trên web (giống Otto)

| Khóa đang chọn | Ô `custom_mac` | Device-Id view |
|---|---|---|
| 1..5 (preset) | Readonly / auto-fill MAC preset; POST gửi `custom_mac` rỗng | MAC preset |
| 0 Tự cấu hình | Cho nhập; trống = MAC chip | `custom_mac` hoặc chip |
| 6 Tự nhập MAC | Bắt buộc nhập hợp lệ `aa:bb:cc:dd:ee:ff` | MAC vừa nhập |
| 7 Giao tiếp hằng ngày | Readonly / hint random; POST `custom_mac` rỗng — server tự pick pool | MAC random đã lưu |

### GET `/api/config` response (shape)

```json
{
  "preset_mac_idx": 1,
  "student_name": "ken",
  "device_id": "ba:53:9e:c5:ba:10",
  "custom_mac": "",
  "preset_macs": {
    "1": "ba:53:9e:c5:ba:10",
    "2": "ba:53:9e:c5:fa:11",
    "3": "ba:53:9e:c5:fa:12",
    "4": "ba:53:9e:c2:ef:10",
    "5": "ba:53:9e:c9:ef:10"
  },
  "all_units": { "1": "1", "2": "1", "3": "1", "4": "1", "5": "1" },
  "yi_sub": 0,
  "yi_voice": 0,
  "yi_units": { "0": "1", "1": "1", "2": "1" },
  "ex_sub": 0,
  "ex_voice": 0,
  "ex_units": { "0": "1", "1": "1", "2": "1", "3": "1", "4": "1" },
  "fl_sub": 0,
  "fl_voice": 0,
  "fl_units": { "0": "1" },
  "ielts_voice": 0,
  "toeic_voice": 0
}
```

### POST `/api/config` body

- `student_name`, `preset_mac_idx`, `custom_mac` (khi cần)
- `all_units` (cho 1..5 flat nếu dùng)
- `yi_sub`, `yi_voice`, `yi_units`, `ex_sub`, `ex_voice`, `ex_units`, `fl_sub`, `fl_voice`, `fl_units`, `ielts_voice`, `toeic_voice`

### Quy tắc Save

- Chỉ tên / unit / sách con → lưu storage, **không** reconnect cloud.
- `preset_mac_idx` đổi (hoặc custom MAC đổi khi idx 0/6, hoặc chọn idx 7) → lưu rồi gọi **ApplyDeviceIdentity**.
- Chọn idx 7 lần đầu / đổi sang 7 → pick random 1 MAC từ pool, ghi `custom_mac`, rồi Apply.
- Server 8080 **không tắt** khi đổi khóa.

---

## Storage

Android: SharedPreferences / DataStore / SQLite — tương đương NVS Otto.

| Key | Ý nghĩa |
|---|---|
| `preset_mac` / `preset_mac_idx` | 0..7 |
| `custom_mac` | MAC string khi idx 0/6/7 |
| `student_name` | tên |
| `units_1`…`units_5` | unit flat (legacy / IELTS / TOEIC) |
| `ex_sub`, `ex_u0`…`ex_u4` | Explorers |
| `yi_sub`, `yi_u0`…`yi_u2` | Young Innovators |
| `fl_sub`, `fl_u0` | Future Leaders |
| `ielts_voice`, `toeic_voice` | IELTS / TOEIC giọng (0 Việt / 1 Anh) |

### Device-Id runtime (`GetMacAddress`)

- idx 1 (Explorers) → `custom_mac` từ pool Explorers (boot/switch đã pick)
- idx 2..5 → MAC preset cố định
- idx 0 → `custom_mac` nếu có, không thì MAC máy
- idx 6 → `custom_mac` bắt buộc
- idx 7 → `custom_mac` từ pool daily (boot/switch đã pick)

---

## ApplyDeviceIdentity — fast path (bắt buộc)

**Không** gọi CheckVersion / không hiện UI activation / không reboot.

Android source hỗ trợ **cả WebSocket và MQTT** → phải implement ApplyDeviceIdentity cho **cả hai**. Không được chỉ làm một protocol. UX sau khi đổi khóa phải giống nhau: đóng phiên cũ → mở phiên mới với Device-Id mới → **tự chào** (WakeWordInvoke / tương đương).

### Bước chung (mọi protocol)

```
1. Abort speaking, đóng audio channel / session cloud hiện tại
2. Tránh callback "channel closed → idle" đè mất session mới (flag suppress)
3. Đọc Device-Id mới từ storage (MAC theo preset_mac_idx / custom_mac / pool)
4. **RegenerateUuid()** — Client-Id mới (bắt buộc khi đổi khóa A→B)
5. Áp dụng identity theo protocol đang dùng (xem 2 nhánh dưới)
6. Initialize / start protocol lại nếu cần
7. Tự WakeWordInvoke (wake word gần nhất, fallback "Hi,Jason")
   → server chào → mic sẵn sàng nói
```

Chỉ tên/unit/sách con đổi → **không** chạy ApplyDeviceIdentity.

### Nhánh A — WebSocket (bắt buộc)

Device-Id gắn **mỗi lần mở kênh** (HTTP header).

```
1. CloseAudioChannel / đóng WebSocket cũ (goodbye nếu có)
2. Mở WebSocket mới tới URL/token đang lưu
3. Set header:
   - Device-Id = MAC mới (từ storage)
   - Client-Id = UUID **mới** vừa RegenerateUuid()
   - Authorization / Protocol-Version giữ như cũ
4. Gửi hello như session bình thường
5. Auto greet (WakeWordInvoke)
```

Không cần xin credential mới từ server nếu token/URL websocket đã có sẵn.

### Nhánh B — MQTT (+ UDP audio nếu có) (bắt buộc)

Identity nằm trong **MQTT client_id / username** (thường `GID_xxx@@@mac_safe@@@mac_safe`, `:` thành `_`).

```
1. CloseAudioChannel / goodbye session hiện tại
2. Disconnect MQTT client cũ
3. Patch credential local theo MAC mới:
   - client_id: thay phần sau @@@ bằng mac_safe@@@mac_safe
   - username: nếu đang dạng MAC (aa:bb:... hoặc aa_bb_...) thì đổi sang MAC mới
   - password / endpoint / publish_topic: giữ nguyên (trừ khi codebase Android có rule khác tương đương)
4. Connect MQTT lại với credential đã patch
5. OpenAudioChannel / hello như session bình thường
6. Auto greet (WakeWordInvoke)
```

### Chọn nhánh lúc runtime

- Đang chạy **WebSocket** → dùng nhánh A.
- Đang chạy **MQTT** → dùng nhánh B.
- App có thể chuyển protocol theo config server lúc boot — **cả hai path phải sẵn sàng**, không hard-code một protocol.
- Nếu cùng lúc có cả WS URL và MQTT endpoint trong storage: làm theo protocol **đang active** của session hiện tại (giống Otto: ưu tiên WS khi có URL lúc đổi khóa; nếu chỉ MQTT thì patch MQTT).

### Kết quả bắt buộc sau Apply (WS hoặc MQTT)

- Session cloud mới dùng Device-Id mới (khóa học mới trên server).
- Robot/app **tự chào** trong vài giây, mic sẵn sàng — user không cần nói wake word lại.
- HTTP Self-Control `:8080` vẫn chạy.

---

## Voice / MCP tools

Tên có thể đổi namespace `self.otto.*` → `self.<board>.*` nhưng **hành vi giữ**.

### 1. `get_student_info`

Trả JSON chỉ 2 field: `student_name`, `unit_names` (tên unit đang chọn).

Dùng khi hỏi "tao là ai / đang học unit gì".

### 2. `show_config_page`

Trả URL đúng dạng `http://<LAN_IP>:8080` (IP WiFi LAN hiện tại, port 8080). Nếu có UI/màn hình thì hiện QR encode **cùng URL đó** — giống Otto. Không trả localhost, không đổi port.

Dùng khi "mở cài đặt / trang cấu hình".

### 3. `set_course`

Args: `course_idx` 0..7, `custom_mac` optional/required theo idx.

Lưu storage; nếu đổi identity → `ApplyDeviceIdentity` như Save web (cả WS lẫn MQTT tùy protocol đang dùng).

Dùng khi "đổi sang Explorers / giao tiếp hằng ngày / tự nhập MAC…".

---

## Acceptance checklist

- [ ] Mở `http://<LAN_IP>:8080` — trang **giống Otto** (thứ tự card, radio khóa, MAC+Device-Id, sách con, unit radio, toast Lưu)
- [ ] `show_config_page` / QR dùng đúng IP LAN + port 8080 (điện thoại khác máy mở được)
- [ ] GET/POST `/api/config` đúng shape; reload vẫn còn data
- [ ] Đổi unit/sách con không reconnect; đổi khóa reconnect nhanh ~vài giây + toast giống Otto
- [ ] **WebSocket:** đổi khóa → đóng WS cũ, mở WS mới với `Device-Id` mới, tự chào
- [ ] **MQTT:** đổi khóa → patch client_id/username MAC mới, reconnect MQTT, tự chào
- [ ] Đổi khóa xong robot/app **tự chào**, mic sẵn sàng (không cần gọi wake word lại)
- [ ] Không reboot, không màn đăng nhập khi đổi khóa
- [ ] **Client-Id:** mỗi boot random UUID mới; đổi khóa A→B cũng random UUID mới (WS header / storage)
- [ ] **Explorers idx=1:** chọn giọng Song ngữ / Tiếng Anh (`ex_voice`); pool `ba:10–19` / `fe:10–19`; boot/switch/đổi giọng đều pick `custom_mac`
- [ ] **Young Innovators idx=2 / Future Leaders idx=3:** `yi_voice` / `fl_voice` Việt–Anh + pool riêng
- [ ] **IELTS idx=4 / TOEIC idx=5:** `ielts_voice` / `toeic_voice` Việt–Anh; pool `c2`/`c3` và `c9`/`c1`
- [ ] **idx 7 Giao tiếp hằng ngày:** random đúng 1/20 MAC trong pool; ghi `custom_mac`; boot lại khi đang idx 7 thì random lại; đổi sang 7 từ khóa khác thì random + ApplyDeviceIdentity + tự chào
- [ ] MCP/voice: hỏi tên/khóa/unit; mở trang cấu hình; đổi khóa bằng giọng nói
- [ ] Client-Id **được random** khi boot và khi đổi khóa; không giữ Client-Id cố định suốt đời máy khi spam đổi MAC

---

## Không làm

- Không CheckVersion / OTA activate UI khi đổi khóa (trừ khi user yêu cầu rõ).
- Không tắt HTTP `:8080` khi reconnect cloud.
- Không multi-select unit.
- Không bịa thêm khóa/MAC ngoài bảng trên.
- Không chỉ implement một trong hai protocol (WS **hoặc** MQTT) — phải làm **cả hai**.
- Không redesign UI web khác Otto; không đổi port/URL scheme; không dùng localhost làm URL QR cho điện thoại.
