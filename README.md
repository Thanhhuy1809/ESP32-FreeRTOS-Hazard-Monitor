# ESP32-C3 Environmental & Hazard Monitoring System (FreeRTOS)

Đồ án Giữa kỳ / Cuối kỳ: Hệ thống giám sát môi trường và cảnh báo nguy hiểm sử dụng ESP32-C3 và FreeRTOS.

## 🌟 Tính năng chính
Hệ thống được thiết kế chuẩn cấu trúc FreeRTOS (Task & Queue Management - theo Chương 4 & 5):
- **Giám sát Môi trường (DHT22):** Theo dõi Nhiệt độ và Độ ẩm.
- **Cảnh báo Cháy / Khí Gas (MQ2):** Đọc nồng độ Gas qua bộ chuyển đổi ADC (Analog-to-Digital). Tự động hú còi khi vượt ngưỡng an toàn (Mặc định: 2000). Tích hợp cảnh báo cháy nếu Nhiệt độ > 60°C.
- **Cảnh báo Rung chấn (MPU6050):** Theo dõi gia tốc kế qua chuẩn giao tiếp I2C. Báo động tức thời khi phát hiện rung lắc mạnh (Magnitude > 1.5g).
- **Hệ thống Cảnh báo Thủ công (Button & Buzzer):** Sử dụng Nút bấm có tích hợp chống nhiễu (Debounce 1 giây) để chủ động bật/tắt còi báo động.

## 🔌 Sơ đồ đấu nối (Pinout)
| Thiết bị | Chân thiết bị | Chân ESP32-C3 | Ghi chú |
| :--- | :--- | :--- | :--- |
| **MQ2 (Gas)** | `A0` (Analog) | `GPIO 0` | Cấp nguồn 5V. |
| **Button** | `Tín hiệu` | `GPIO 1` | Đầu còn lại nối GND (Code đã bật Pull-up). |
| **DHT22** | `DATA` | `GPIO 2` | Kéo trở 10k lên VCC. |
| **Buzzer** | `I/O` | `GPIO 3` | Còi chủ động (Active-High). |
| **MPU6050** | `SDA` | `GPIO 4` | Giao tiếp I2C. |
| **MPU6050** | `SCL` | `GPIO 5` | Giao tiếp I2C. |

## 🏗 Cấu trúc FreeRTOS
Hệ thống gồm 4 Task chạy đa nhiệm song song và giao tiếp qua 1 Queue (`xAlarmQueue`):
1. `vControllerTask` (Priority 4): Xử lý trung tâm. Chặn (Block) chờ tín hiệu từ Queue để đóng/mở còi báo động.
2. `vFastSensorTask` (Priority 3): Quét MPU6050 và MQ2 ở tốc độ cao (100ms/lần). Đẩy cờ báo động vào Queue nếu phát hiện nguy hiểm.
3. `vButtonTask` (Priority 3): Quét nút bấm (50ms/lần), xử lý Debounce 1 giây và đẩy lệnh Toggle vào Queue.
4. `vDhtTask` (Priority 2): Đo nhiệt độ, độ ẩm mỗi 2 giây, in báo cáo tổng hợp ra Terminal và kích hoạt báo cháy nếu nhiệt độ cao.

## 🚀 Kết quả Terminal
Dưới đây là hình ảnh Terminal theo dõi dữ liệu mượt mà, không bị nhiễu (Floating pin đã được xử lý triệt để):

![Terminal Output](docs/terminal_output.png)

## 🛠 Hướng dẫn Build và Nạp (ESP-IDF v5)
```bash
idf.py build
idf.py -p COM10 flash monitor
```
