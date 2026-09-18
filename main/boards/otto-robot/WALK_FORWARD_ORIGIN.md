# Đi tiến (Walk forward) — gốc từng động cơ & bước cuối

Tài liệu mô tả **chu kỳ gốc** khi bấm **Tiến** trên Self-Control (`:8080`), theo code hiện tại trong `otto_movements.cc` → `Otto::Walk()`.

Nguồn web: `OttoWebControlAction("forward")` → `Walk(steps=3, period=700, dir=+1, amount=50)`.

---

## 1. Công thức góc lệnh (phần mềm)

Mỗi sample (~30 ms), oscillator tính:

```text
pos_cmd = 90 + O + A · sin(phase + φ)     // rồi cộng trim khi PWM
PWM_angle = clamp(pos_cmd + trim, 0…180)
```

| Ký hiệu | Ý nghĩa |
|--------|---------|
| **90** | Điểm neo lệnh (nest) — “gốc” cứng trong firmware |
| **O** | Offset quanh neo (điểm giữa dao động) |
| **A** | Biên độ (amplitude) |
| **φ** | Phase cố định của khớp |
| **trim** | Lệch lắp cánh (NVS `otto_trims`, chỉnh trên web) |
| **walk_tip** | Nhón chân khi đi (NVS `walk_tip`, mặc định **0**) |

Trim **không** đổi công thức dao động; chỉ lệch PWM so với `pos_cmd`.

---

## 2. Index khớp

| Index | Code | Tên | Pin (log điển hình) |
|------:|------|-----|---------------------|
| 0 | `LEFT_LEG` / LL | Hông trái | GPIO17 |
| 1 | `RIGHT_LEG` / RL | Hông phải | GPIO39 |
| 2 | `LEFT_FOOT` / LF | Bàn trái | GPIO18 |
| 3 | `RIGHT_FOOT` / RF | Bàn phải | GPIO38 |
| 4 | `LEFT_HAND` / LH | Tay trái | GPIO8 |
| 5 | `RIGHT_HAND` / RH | Tay phải | GPIO12 |

---

## 3. Tham số gốc khi **đi tiến** (`dir = +1`)

```text
A = {30, 30, 30, 30, amount, amount}   // amount=50 từ web; 0 nếu không vẫy tay
O = {0, 0, tip, -tip, 45-90, 45}       // tip = walk_foot_tip_ (mặc định 0)
φ = {0, 0, -90°, -90°, …}              // tiến: chân lệch hông −90°
```

`HAND_HOME_POSITION = 45` → offset tay: LH `O=-45`, RH `O=+45` (tâm lệnh tay ≈ 45° / 135° khi A=0).

### 3.1 Từng động cơ — điểm gốc & khoảng dao động

Giả sử **`walk_tip = 0`**, **`trim = 0`**, **`amount = 50`** (web Tiến).

| Khớp | Neo (90+O) | A | φ (tiến) | Góc lệnh min…max (xấp xỉ) | Ý nghĩa trong chu kỳ |
|------|------------:|--:|----------|---------------------------|----------------------|
| **LL hông trái** | **90°** | 30 | **0°** | 60° … 120° | Hai hông **cùng pha** — xoay thân/đẩy bước |
| **RL hông phải** | **90°** | 30 | **0°** | 60° … 120° | Cùng pha LL |
| **LF bàn trái** | **90°** (+tip) | 30 | **−90°** | 60° … 120° | Lệch hông **−90°** — nhấc/hạ bàn so với hông |
| **RF bàn phải** | **90°** (−tip) | 30 | **−90°** | 60° … 120° | Cùng pha LF (hai bàn cùng pha nhau) |
| **LH tay trái** | **45°** (90−45) | 50 | **= φ RL = 0°** | ~−5→95 → clamp **0…95** | Cùng pha **chân phải** (vẫy đối bước) |
| **RH tay phải** | **135°** (90+45) | 50 | **= φ LL = 0°** | 85…185 → clamp **85…180** | Cùng pha **chân trái** |

Nếu `walk_tip = 5` (kiểu Otto cổ điển):

- LF neo lệnh = **95°**, RF neo = **85°** → luôn hơi nhón (dễ nhìn như “co”).

### 3.2 Quan hệ pha (tiến)

```text
Hông LL ████████████████  sin(ωt + 0)
Hông RL ████████████████  sin(ωt + 0)     ← cùng pha hông

Bàn LF  ░░░░████████░░░░  sin(ωt − 90°)   ← chậm hông 1/4 chu kỳ
Bàn RF  ░░░░████████░░░░  sin(ωt − 90°)   ← cùng pha bàn

Tay LH  ████████████████  theo RL
Tay RH  ████████████████  theo LL
```

Một **chu kỳ** = một vòng `sin` đầy đủ = `period` ms (**700 ms** trên web).

Web mỗi lần Tiến: **`steps = 3`** → khoảng **3 × 700 ms ≈ 2,1 s** dao động (chưa kể bước thêm lúc kết).

---

## 4. Điểm “gốc đứng” vs “gốc đi”

| Trạng thái | LL | RL | LF | RF | LH | RH |
|------------|---:|---:|---:|---:|---:|---:|
| **Đứng nghỉ (Home / Xem thử trim)** | 90 | 90 | 90 | 90 | 45 | 135 |
| **Tâm dao động khi đi (O, tip=0)** | 90 | 90 | 90 | 90 | 45 | 135 |
| **Biên khi đi (A chân=30, tay=50)** | ±30 quanh 90 | ±30 | ±30 | ±30 | ±50 quanh 45 | ±50 quanh 135 |

Gốc lệnh luôn lấy **90° (+ O)**; trim chỉ lệch PWM thực tế.

---

## 5. Bước cuối khi **Tiến** (hiện tại)

Sau `Walk` xong, `ActionTask` gọi **`Home()`** nếu hàng đợi trống.

`Home` (theo source mới): thời gian `clamp(500 + max_delta×9, 500…1700)` ms + `MoveServos` **ease-out cubic** — tránh phanh gấp/té khi đi xong về neo.

Không còn bước thêm / soft-stand riêng / ghi góc tay trên web.

---

## 6. Chỉnh tay liên quan (web)

| Tham số | Ảnh hưởng |
|---------|-----------|
| Trim LL/RL/LF/RF/LH/RH | Lệch gốc lắp — đi/đứng lệch |
| **walk_tip (TIP)** | Neo LF/RF khi đi: `+tip` / `−tip` — tăng thì nhón/co hơn |
| Ghi góc bước cuối | Log/xuất JSON; **không** đổi gait cho đến khi bạn nhờ áp vào code |

---

## 7. File code liên quan

- `main/boards/otto-robot/otto_movements.cc` — `Otto::Walk`
- `main/boards/otto-robot/oscillator.cc` — `Refresh()` → `90 + O + A·sin`
- `main/boards/otto-robot/otto_controller.cc` — web Tiến: `steps=3`, `period=700`, `amount=50`
