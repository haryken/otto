# Self-Control Web - Otto Robot

## Tổng quan

Có **3 tính năng** trên trang Self-Control (port 8080):

| # | Tính năng | Cách dùng | Mô tả |
|---|---|---|---|
| 1 | **Đổi cấp độ học** | Web hoặc Giọng nói | Chuyển giữa 6 cấp độ (0-5) |
| 2 | **Chọn Unit + Điền tên** | Chỉ qua Web (QR) | Tick unit cho cấp độ đang chọn, điền tên học viên |
| 3 | **Robot biết tên người dùng** | Giọng nói | Hỏi robot "tao là ai?" → robot trả lời từ `student_name` đã lưu |

---

## Tính năng 1: Đổi cấp độ học

### Qua Web (port 8080)
- Hiện 6 radio button: 0=Custom, 1=explorers, 2=younginnovators, 3=futureleaders, 4=ielts, 5=toeic
- Bỏ qua custom MAC (index 6)
- Chọn xong → bấm Lưu → lưu `preset_mac_idx` vào NVS
- **UI realtime**: khi đổi cấp độ → danh sách Unit đổi theo ngay lập tức (không cần reload)
- **Áp dụng MAC mới ngay** (không reboot) — xem mục dưới

### Qua Giọng nói
- Nói với robot: "đổi sang cấp độ ielts"
- Robot gọi MCP tool `self.otto.set_course` → đổi `preset_mac_idx` → lưu NVS → **cùng luồng áp dụng MAC mới ngay**

---

## Áp dụng cấp độ mới ngay (không reboot) — LÀM ĐƯỢC

### Vì sao chỉ Lưu NVS chưa đủ
- **Device-Id** = MAC khóa học, đọc từ NVS lúc handshake.
- **Client-Id** = UUID robot, **không đổi**.
- MQTT/WS cloud lấy `client_id / username / password` lúc **boot** (`ota.CheckVersion()` gửi header `Device-Id`).
- Session đang chạy vẫn khóa cũ. Chỉ reconnect MQTT với credential cũ → **vẫn khóa cũ**.

### Plan kỹ thuật (không `esp_restart`)

Sau khi `preset_mac_idx` thật sự đổi (web Save hoặc giọng nói):

```
1. Ghi NVS wifi/preset_mac  (đã có)
2. Nếu đang nói chuyện: CloseAudioChannel + abort speaking
3. Gọi lại OTA CheckVersion()
   → HTTP header Device-Id = MAC mới
   → server trả mqtt/websocket config mới → ghi NVS mqtt
4. protocol_.reset() rồi InitializeProtocol() + Start()
5. Robot idle, lần nói tiếp theo dùng khóa mới
```

Port **8080 không tắt** — điện thoại giữ tab web được.

### Rủi ro
- Đang hội thoại: câu đang nói bị cắt (chấp nhận).
- MAC khóa học chưa kích hoạt trên server → có thể hiện mã activation (giống máy mới). Các preset đang dùng thường đã kích hoạt sẵn thì OK.
- CheckVersion fail → giữ session cũ, báo lỗi trên web / LCD, không reboot mù.

### Không làm
- Không restart WiFi / không tắt server 8080.
- Không đổi Client-Id / UUID.

---

## Tính năng 2: Chọn Unit + Điền tên học viên

### Chỉ qua Web (QR → port 8080)

1. Nói với robot "mở cài đặt" → robot hiện QR trên LCD chứa `http://<IP>:8080`
2. Quét QR → mở trang Self-Control
3. Trang web đọc cấp độ hiện tại từ NVS → hiện:
   - **Cấp độ đang chọn** (radio button — có thể đổi, xem tính năng 1)
   - **Tên học viên** (ô input text)
   - **Danh sách Unit** checkbox tương ứng cấp độ hiện tại
     - Cấp độ 1-5: hiện Unit checkbox (mock 10 unit)
     - Cấp độ 0 (custom): không hiện Unit
   - Khi đổi cấp độ trên radio → danh sách Unit **cập nhật realtime** trên UI, hiện đúng unit đã lưu của cấp độ mới
4. Điền tên, tick unit → bấm Lưu → lưu NVS

---

## Tính năng 3: Robot biết tên người dùng

- Robot đọc `student_name` từ NVS khi cần
- Khi người dùng hỏi "tao là ai?", "tên tao là gì?" → robot trả lời từ data đã lưu
- Thêm MCP tool `self.otto.get_student_info` trả về `{ student_name, course, unit }`
- Robot (LLM) dùng thông tin này để trả lời tự nhiên

---

## Quy tắc lưu Unit — MỖI CẤP ĐỘ LƯU RIÊNG

Unit được lưu **riêng biệt** cho từng cấp độ. Đổi cấp độ không làm mất unit đã lưu.

### Mặc định ban đầu
- **Tất cả 5 cấp độ (1-5) đều mặc định Unit 1 đã được chọn sẵn** khi chưa ai lưu gì.

### Ví dụ:
```
Ban đầu: tất cả cấp độ đều có Unit 1 ✓ (mặc định)
Bước 1: Đang cấp độ 1 (explorers) → giữ Unit 1 → Lưu
Bước 2: Đổi sang cấp độ 2 (younginnovators) → tick thêm Unit 3 → Lưu
Bước 3: Đổi quay lại cấp độ 1 (explorers) → vẫn thấy Unit 1 đang tick ✓ (không mất)
```

### NVS Storage:
```
Namespace: otto
├── student_name   = "Minh"           (string — dùng chung, không theo cấp độ)
├── units_1        = "1"              (string — units của explorers, mặc định "1")
├── units_2        = "1"              (string — units của younginnovators, mặc định "1")
├── units_3        = "1"              (string — units của futureleaders, mặc định "1")
├── units_4        = "1"              (string — units của ielts, mặc định "1")
├── units_5        = "1"              (string — units của toeic, mặc định "1")

Namespace: wifi
├── preset_mac     = 1                (int32 — cấp độ đang chọn)
```

---

## Luồng hoạt động

```
=== ĐỔI CẤP ĐỘ (Tính năng 1) ===

  [Qua giọng nói]
  Người dùng nói "đổi sang ielts"
    → Robot đổi preset_mac_idx = 4 → lưu NVS → xong

  [Qua web]
  Mở trang 8080 → chọn radio cấp độ → danh sách Unit đổi realtime
    → Bấm Lưu → lưu preset_mac_idx + units vào NVS → xong

=== CHỌN UNIT + TÊN (Tính năng 2) ===

  Người dùng nói "mở cài đặt"
    → Robot lấy IP LAN
    → Tạo QR code chứa http://<IP>:8080
    → Hiện QR lên LCD 240x240
  
  Người dùng quét QR → trình duyệt mở trang
    → Trang gọi GET /api/config
    → Server trả: { preset_mac_idx, student_name, all_units: {1:"1", 2:"1,3", ...} }
    → Trang hiện cấp độ hiện tại + tên + units đã lưu
    → Đổi radio cấp độ → Unit list đổi realtime (load units đã lưu của cấp độ đó)
  
  Người dùng sửa → bấm Lưu
    → POST /api/config { preset_mac_idx, student_name, all_units }
    → Server lưu NVS
  
  Tắt nguồn → bật lại → mở lại web → dữ liệu vẫn còn ✓

=== ROBOT BIẾT TÊN (Tính năng 3) ===

  Người dùng hỏi: "tao là ai?"
    → Robot gọi MCP tool self.otto.get_student_info
    → Trả về: { student_name: "Minh", course: "ielts", unit: "5,7" }
    → Robot trả lời: "Bạn là Minh, đang học khóa IELTS, Unit 5 và 7"
```

---

## Các file cần tạo/sửa

| File | Hành động |
|---|---|
| `main/boards/otto-robot/websocket_control_server.cc` | HTTP `GET /`, `GET/POST /api/config`; sau Save nếu đổi cấp độ → schedule apply identity |
| `main/application.cc` / `.h` | Thêm `ApplyDeviceIdentity()`: CheckVersion + reset protocol cloud |
| `main/boards/otto-robot/otto_controller.cc` | `set_course` / `show_config_page` / `get_student_info`; set_course gọi ApplyDeviceIdentity |
| `main/boards/otto-robot/self_control.html` | Trang Self-Control |
| `main/CMakeLists.txt` | Embed HTML vào binary |

---

## Mock Data (tạm thời)

Thay bằng danh sách Unit thật theo cấp độ (MAC preset 1–5 giữ nguyên):

| idx | Device-Id preset | Tên hiện trên web | Số unit |
|---|---|---|---|
| 1 | explorers | Explorers | unit phẳng |
| 2 | younginnovators | **Young Innovators** | **3 sách con QUEST** (One A, One B, Three A) |
| 3 | futureleaders | Future Leaders | unit phẳng |
| 4 | ielts | SPARK Two A | unit phẳng |
| 5 | toeic | SPARK Three A | unit phẳng |

Young Innovators: chọn sách QUEST (radio) rồi chọn **1 unit**. Đổi sách QUEST **không** reconnect MAC. Đổi Explorers ↔ Young Innovators mới reconnect.

Mặc định vẫn tick Unit đầu tiên (`"1"`) của mỗi khóa.
Sau này thay bằng data thật.
