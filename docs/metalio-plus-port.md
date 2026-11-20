# Metalio E-Ink4 Plus port

The `metalio_eink4_plus` target combines WitchHunt 2.53 with the Metalio S31
board and EPDiy driver from FreeInk SDK. The hardware reference is CrossPoint
`feat-metalio-eink4plus` at `ea534874bd60074058b8862c0f57bf59def1f0ee`.
The SDK integration combines `ef99c6c8bd48fb357bf034b303e307acd1a0fe25`
and `99901125a1eee287e120b88d9edc7810fca8b904`.

Build with pioarduino Core 6.2.0. The environment selects Arduino-ESP32 4.0 RC1
and ESP-IDF 6.1. On Windows, set `PYTHONUTF8=1` before running the build.

```text
npm ci
pio run -e metalio_eink4_plus
```

The app artifact is `.pio/build/metalio_eink4_plus/firmware.bin`. Metalio
uses the 14 pt and 18 pt bitmap masters for built-in reader fonts and scales
the other sizes to fit the 5 MiB slot. SD font support is available.

The separate `metalio-plus-build` directory holds the delivered images.
The `DUAL-FULL.bin` image uses the official CloudZao 2.0.81 bootloader and
assets, with CloudZao in `ota_0` as the initial firmware and WitchHunt in
`ota_1`. It adds private WitchHunt NVS in the unused flash tail. This is a
whole-flash image and installing it replaces the internal data on the device.
The two `-app.bin` files identify the exact slot images for the switch tool.

The WitchHunt updater is disabled because the other slot can hold CloudZao.
The CloudZao updater can replace the inactive slot and therefore may replace
WitchHunt. The combined image has been validated offline. The app-only
build reached Home on an ESP32-S31 unit with the dpReader slot layout.
Serial diagnostics confirmed SDMMC mount, successful EPD refresh calls,
and a nonblank framebuffer. Hardware checks on one unit covered sleep
and short-press wake, touch and bezel keys, and Wi-Fi discovery. The
combined CloudZao image has not been flashed.

The Metalio build uses the same WitchHunt touch UI and default Lyra theme as
the Picco build. Fresh Metalio settings open Home entries and list rows with
one tap. A saved touch activation preference takes precedence.
At boot the app watches the Metalio power key through the input sampler so
the touch event queue resumes once the power key is released.

The footer aligns three buttons with the Metalio front bezel. The bezel Menu
key sends Back on tap and defaults to Go Home on hold. The bezel page keys
send Left and Right, independently of the two side volume keys (Up and Down).
The side BOOT/AI key is WitchHunt's Confirm key. Power uses the board's
expander and the WitchHunt sleep action. CloudZao voice and media functions
belong to the CloudZao slot.

The 4.7-inch portrait UI uses 78 px minimum list rows, 84 px tabs, and a
96 px footer at the normal UI font size. Lists use the full content width.

The Windows bench script uses esptool 5.6.0. With only `--port` it reports
the current slot and app descriptions. A switch requires the destination slot
and the exact app image installed there. It verifies a byte-for-byte match
before updating one OTA metadata sector and rebooting.

```text
python scripts/metalio_boot_slot.py --port COM3
```

Once the dual image is installed and verified on the hardware, use
`--slot 0 --image <CloudZao app.bin>` to select CloudZao or
`--slot 1 --image <WitchHunt app.bin>` to select WitchHunt from the PC.

WitchHunt uses the optional `witch_nvs` partition for startup preferences
and leaves the other firmware's NVS alone. When that partition is absent,
startup preferences in NVS are unavailable. Normal reader state stays on
microSD. Sleep / Screen Off parks the panel, card and light, then enters
deep sleep with the expander's wake interrupt armed. If wake cannot be
armed, firmware waits for Power and restarts. Light sleep is disabled on
the S31 RC toolchain because the FreeInk bring-up reported PSRAM corruption
after wake.

## Lần sửa tiếp theo — 2026-11-19

Ba phím cảm ứng dưới viền đã được người dùng xác nhận hoạt động trên máy.

- **Vuốt ngang Back/Forward.** Bản thử hiện chưa có thao tác này. Thêm điều
  hướng bằng vuốt trái/phải và kiểm tra trên máy thật, kể cả khi có vùng chạm
  hoặc thanh trượt trên màn hình.
- **Giao diện 4,7 inch (684 × 1216 px).** Chỉnh lại phân cấp chữ, bố cục và
  kích thước nút. Làm cho các vùng chạm dễ bấm bằng ngón tay, đủ cách nhau
  và khớp với vị trí hiển thị.
- **Nút cứng cạnh hông dưới cùng.** Đặt thao tác bấm mặc định là về HOME.
  Xác nhận đúng nút trên máy và kiểm tra thao tác nguồn/Sleep khi giữ nút.
- **Thanh trượt chỉnh đèn đọc (frontlight).** Thêm điều khiển kéo bằng ngón
  tay. Ảnh BOOX là tham chiếu cho cách bố trí thanh trượt và nút tăng/giảm.
  Số thanh điều khiển theo số kênh đèn thực tế của Metalio.
- **Phản hồi rung khi chạm (haptic).** Người dùng ghi nhận máy có rung khi
  chạm. Mã CloudZao có phản hồi rung cho vùng chạm trên màn hình và phím
  cảm ứng dưới viền. Khi sửa giao diện, kiểm tra riêng hai đường chạm trên
  WitchHunt để phản hồi rung xác nhận thao tác đã được nhận.

### Bố cục đề xuất tham chiếu BOOX và Kindle

**Trang chủ**, từ trên xuống:

1. **Ngày tháng năm** ở đầu màn hình, hiển thị gọn theo ngôn ngữ thiết bị.
2. **Sách đang đọc** là khối nổi bật, với bìa, tên sách và thao tác **Đọc tiếp**.
3. **Tiến độ đọc** nằm trong khối sách, ngay dưới tên sách, gồm thanh tiến độ
   và phần trăm của cuốn đang đọc.
4. **Menu bên dưới khối sách**, ưu tiên nút lớn cho Thư viện, Sách gần đây và
   Cài đặt. Các mục khác có thể nằm trong menu nếu còn chỗ.

**Màn đọc** ưu tiên diện tích trang sách. Chạm giữa để mở Mục lục, Aa, Đèn
và Đánh dấu. Vuốt ngang lật trang theo chiều đọc của sách. Cử chỉ bắt đầu
trên slider hoặc thanh công cụ thuộc về điều khiển đó.

**Cài đặt** dùng danh sách một cột, tiêu đề nhóm rõ, tên và giá trị ở hai
đầu dòng. Cả dòng là vùng chạm. **Bảng đèn đọc** mở ở nửa dưới, có Bật/Tắt,
slider độ sáng và ấm/lạnh nếu hai kênh hoạt động, cùng nút trừ/cộng. Đèn
phản hồi khi kéo và lưu giá trị khi nhấc tay.

Mốc thử trên máy ở 684 × 1216 px: lề ngang 28 px, chữ danh sách 28–32 px,
tiêu đề 36–40 px, vùng chạm chính 84–96 px và cách nhau ít nhất 12 px.

Tham chiếu: [Menu đọc BOOX](https://support.boox.com/fee6/5a9b),
[Đèn đọc BOOX](https://shop.boox.com/blogs/news/boox-firmware-v4-1-overview-of-new-features-and-upgrades),
[Thanh công cụ đọc Kindle](https://digprjsurvey.amazon.com/csad/help/node/T5Y94BzSCGwm0vd75W).

### Bản triển khai — 2026-11-20

- Vuốt ngang lật một trang theo chiều đọc trên Metalio. Chạm hai cạnh vẫn lật
  trang trong chế độ chạm. Chế độ tắt điều khiển trang bằng cảm ứng vẫn tắt cả
  thao tác vuốt.
- Trang chủ dùng một khối sách đang đọc, tiến độ, ngày từ đồng hồ thiết bị và
  các mục điều hướng bên dưới. Sách gần đây mở thẳng tab tương ứng của Thư viện.
- Phím nguồn bấm ngắn mặc định về HOME. Giữ phím vẫn dùng thao tác Sleep.
  Cấu hình bấm ngắn do người dùng đặt được đọc lại khi khởi động.
- Bảng đèn đọc mở phía dưới màn hình bằng vuốt từ mép trên hoặc biểu tượng mặt
  trời trên thanh đọc EPUB. Kéo thanh độ sáng hoặc ấm/lạnh để đổi đèn trực tiếp.
- Motor rung của Metalio phát một nhịp khi nhận chạm màn hình hoặc thao tác phím
  cảm ứng dưới viền.

Kiểm tra trên máy qua USB với EPUB thử: vuốt trái/phải đổi đúng một trang,
biểu tượng mặt trời mở bảng đèn, nút cộng chỉnh ấm/lạnh và lưu khi nhấc tay,
bấm ngắn Power đưa về HOME. Người dùng đã xác nhận rung khi chạm màn hình
và khi dùng phím cảm ứng dưới viền. Nút cứng dưới cùng bên hông cũng đã được
xác nhận đưa về HOME. Bản thử đang ở slot WitchHunt, cạnh slot dpReader.
Người dùng đã xác nhận độ sáng thay đổi ngay trong lúc kéo thanh trượt.

Lỗi lặp khởi động khi nạp bìa JPEG đến từ việc ROM ESP32-S31 và thư viện
TJpgDec dùng cùng tên hàm với ABI khác nhau. Bản sửa đổi tên hai ký hiệu của
TJpgDec. Đã kiểm tra máy lên HOME, vẽ được bìa JPEG của EPUB thử, mở lại đúng
trang qua Đọc tiếp và vuốt ngang hai chiều. Người dùng đã mở lại thành công
cuốn từng gây lỗi từ Thư viện.

Chữ giao diện Metalio ở cỡ Normal dùng Inter 12 pt cho mục và giá trị,
14 pt cho tiêu đề. Bảng đèn có nút Quay lại để trở về màn hình đã mở bảng.
Ảnh chụp Settings trên máy cho thấy chiều cao chữ của một mục tăng từ
21 lên 24 px. Kiểm thử qua USB xác nhận nút Quay lại đóng bảng đèn và mở
lại đúng màn Settings.
Sleep giữ nguồn cho mạch đánh thức để thao tác bấm ngắn Power có thể bật
lại máy. Tín hiệu đánh thức cũng có thể đến từ các phím dùng chung ngắt
với Power trên bộ mở rộng GPIO. Đã thử trên máy qua USB: giữ Power để vào
Sleep, ngắt đánh thức được bật, bấm ngắn Power và máy quay lại HOME. Người
dùng cũng xác nhận thao tác bấm ngắn đã đánh thức được máy.

### Điều chỉnh Trang chủ — 2026-11-20

Trang chủ áp dụng ngay theme đã chọn và dùng các vùng chạm trực tiếp.
Lyra Carousel có lưới menu hai hàng với nhãn dưới biểu tượng. Metalio ẩn
hàng gợi ý ba phím viền trên toàn giao diện. Cài đặt có nút Quay lại ở
đầu màn hình. Trong menu và các màn hình phụ, vuốt từ mép trái vào để quay lại.

Khi đọc EPUB, mở menu đọc rồi chọn Sync. Nếu chưa có tài khoản, tab này
mở cài đặt KOReader. Sau khi lưu thông tin tài khoản, tab hiện các thao
tác tải xuống, đẩy lên và so sánh tiến độ.
