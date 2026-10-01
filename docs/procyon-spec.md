# Procyon: Spec trình quản lý tác vụ đa nền tảng

Oct 1, 2026 · @Manh Pham Van

## Tổng quan sản phẩm

Một trình quản lý tác vụ GUI chạy trên Windows, macOS và Linux, sâu như Task Manager của Windows nhưng nhẹ hơn.

**Vấn đề:** chưa có app nào vừa đa nền tảng, vừa có giao diện đồ họa hiện đại, vừa đủ sâu. Người dùng nhiều hệ điều hành phải học 3 công cụ khác nhau; công cụ đa nền tảng duy nhất phổ biến (btop) chạy trong terminal.

**Đối tượng người dùng:**

| Nhóm | Nhu cầu chính | Ưu tiên |
| --- | --- | --- |
| Người dùng phổ thông | Tìm app làm máy chậm, tắt app treo, xem app tốn pin | Cao (MVP) |
| Developer / power user | Dùng nhiều OS, cần xem GPU, port, file bị khóa, tiến trình con | Cao |
| Game thủ, người làm đồ họa | Nhiệt độ, GPU, theo dõi khi đang chơi/render | Trung bình |
| IT / quản lý vài máy | Xem nhiều máy cùng lúc, cảnh báo | Sau v1 |

**Nguyên tắc thiết kế:**

1. Nhẹ là tính năng số một: app theo dõi tài nguyên không được tự ngốn tài nguyên.
2. Một trải nghiệm thống nhất trên mọi OS; chỗ nào OS không hỗ trợ thì ẩn hoặc ghi rõ, không hiển thị số sai.
3. Dễ hiểu cho người thường, sâu dần cho người cần (progressive disclosure).
4. Module bật/tắt được: mỗi module có chi phí CPU riêng.
5. Không thu thập dữ liệu người dùng; mọi thứ chạy cục bộ.

## Phân tích đối thủ

Không app nào trong 6 app tiêu biểu vừa là GUI vừa chạy trên cả ba hệ điều hành; mỗi app mạnh ở một mảng riêng.

| App | Nền tảng | Điểm mạnh nhất | Điểm nên học |
| --- | --- | --- | --- |
| [Windows Task Manager](https://www.bleepingcomputer.com/news/microsoft/closer-look-at-windows-11s-new-task-manager/) | Windows | Chuẩn mực quen thuộc, đủ tab | Efficiency mode, Startup impact, bảng lệnh Ctrl+K, chỉnh tốc độ cập nhật |
| [System Informer](https://www.neowin.net/software/system-informer-3225011/) | Windows | Phân tích sâu, mã nguồn mở | Tìm tiến trình giữ file, đóng kết nối mạng, quản lý service, stack trace, portable |
| [Activity Monitor](https://www.howtogeek.com/227240/how-to-monitor-your-macs-health-with-activity-monitor) | macOS | Tab năng lượng | Energy Impact hiện tại + trung bình 8 giờ, app ngăn máy ngủ, biểu đồ trên icon Dock |
| [Stats](https://github.com/exelban/stats) | macOS | Luôn hiển thị trên menu bar | 9 module, widget tùy chỉnh, cảm biến/quạt, dashboard web từ xa, tắt module để tiết kiệm |
| [Mission Center](https://linuxiac.com/mission-center-system-monitoring-app/) | Linux | Giao diện đẹp, Rust + GTK4 | CPU theo luồng, GPU encode/decode, biểu đồ vẽ bằng OpenGL để giảm tải |
| [btop](https://tracker.pardus.org.tr/yirmibir/btop) | Linux, macOS, BSD (Windows qua btop4win) | Nhẹ, gần đa nền tảng | Lọc, cây tiến trình, gửi signal, preset, menu cấu hình đầy đủ |

**Khoảng trống thị trường:** một GUI đa nền tảng có độ sâu ngang System Informer, tab năng lượng kiểu Activity Monitor, widget kiểu Stats và lịch sử dài hạn mà chưa app nào có cho mọi tài nguyên.

## Tính năng desktop

Tính năng chia 3 tầng: P0 là MVP bắt buộc, P1 để ngang hàng các app tốt, P2 là phần tạo khác biệt.

### P0: MVP

| Module | Tính năng | Ghi chú |
| --- | --- | --- |
| Tiến trình | Danh sách tiến trình: tên, PID, user, CPU %, RAM, disk I/O, mạng | Cột tùy chọn, lưu cấu hình cột |
| Tiến trình | Gom theo app (vd. mọi tiến trình Chrome dưới 1 dòng) và dạng cây cha-con | Chuyển đổi giữa dạng phẳng, nhóm, cây |
| Tiến trình | Tìm kiếm, lọc, sắp xếp theo mọi cột | Phím tắt focus ô tìm kiếm |
| Tiến trình | End task (đóng nhẹ) và End process tree (cưỡng bức) | Hỏi xác nhận với tiến trình hệ thống |
| Hiệu năng | Biểu đồ thời gian thực: CPU (tổng và từng nhân), RAM/swap, disk, mạng | Giữ 60 giây gần nhất |
| Hệ thống | Thông tin máy: CPU, RAM, ổ đĩa, OS, uptime |  |
| Giao diện | Light/dark theo hệ thống, chỉnh tốc độ cập nhật (0,5–5 giây, tạm dừng) |  |

### P1: Ngang hàng đối thủ

| Module | Tính năng | Ghi chú |
| --- | --- | --- |
| GPU | Mức dùng tổng, VRAM, nhiệt độ, encode/decode; GPU theo tiến trình nơi OS cho phép | NVIDIA, AMD, Intel, Apple Silicon |
| Tiến trình | Đặt priority/nice, CPU affinity, suspend/resume, gửi signal (Unix) | Một số cần quyền admin |
| Tiến trình | Chi tiết tiến trình: đường dẫn, dòng lệnh, biến môi trường, thời gian chạy, luồng | Mở vị trí file, copy thông tin |
| Khởi động | Danh sách app chạy cùng hệ thống, mức ảnh hưởng, bật/tắt | Mỗi OS một cơ chế |
| Service | Xem và start/stop/restart service (Windows Services, systemd, launchd) |  |
| Lệnh nhanh | Bảng lệnh kiểu Ctrl+K: end task, đổi priority, mở vị trí file |  |
| Tray / menu bar | Widget nhỏ hiển thị CPU, RAM, mạng, nhiệt độ | Module chọn được |
| Pin | Mức pin, tình trạng pin, app ngăn máy ngủ | Laptop |

### P2: Khác biệt

| Module | Tính năng | Ghi chú |
| --- | --- | --- |
| Lịch sử | Lưu lịch sử mọi tài nguyên theo app (24 giờ đến 30 ngày), xem lại đỉnh tải | Lưu cục bộ, nén, giới hạn dung lượng |
| Năng lượng | Tab năng lượng kiểu Activity Monitor cho cả Windows và Linux | Ước lượng nơi OS không có số chính xác |
| Phân tích sâu | Tiến trình nào đang giữ file/thư mục; app nào đang mở port, kết nối tới đâu, đóng kết nối | Học từ System Informer |
| Cảm biến | Nhiệt độ CPU/GPU/ổ đĩa, tốc độ quạt | Phụ thuộc phần cứng |
| Cảnh báo | Thông báo khi app ngốn RAM/CPU bất thường hoặc máy quá nóng | Ngưỡng tùy chỉnh |
| Giải thích | Mô tả dễ hiểu "tiến trình này là gì, có nên tắt không" | Cơ sở dữ liệu tiến trình phổ biến |
| Mở rộng | Hệ thống plugin/module bật tắt riêng | Giữ lõi nhỏ |

## Yêu cầu phi chức năng

App phải nhẹ hơn Task Manager gốc khi chạy nền; các con số dưới đây là mục tiêu đề xuất, cần đo lại trên máy thật.

| Nhóm | Yêu cầu | Mục tiêu đề xuất |
| --- | --- | --- |
| Hiệu năng | CPU khi mở cửa sổ, cập nhật 1 giây | < 1–2% trên máy 4 nhân |
| Hiệu năng | CPU khi chỉ chạy widget tray | < 0,5% |
| Hiệu năng | RAM | < 80 MB khi mở, < 30 MB khi chạy nền |
| Hiệu năng | Thời gian khởi động | < 1 giây |
| Hiệu năng | Kích thước bộ cài | < 20 MB |
| Độ chính xác | Số liệu lệch so với công cụ gốc của OS | < 5% |
| Quyền | Chạy bằng quyền thường; chỉ xin quyền admin/root khi thao tác cần (qua helper riêng) |  |
| Bảo mật | Thao tác nguy hiểm (tắt tiến trình hệ thống) phải xác nhận |  |
| Quyền riêng tư | Không gửi dữ liệu ra ngoài; telemetry nếu có thì opt-in |  |
| Khả năng truy cập | Điều hướng hoàn toàn bằng bàn phím, hỗ trợ trình đọc màn hình |  |
| Quốc tế hóa | Tiếng Việt và tiếng Anh từ v1; đơn vị theo locale |  |
| Phân phối | Windows: MSI/winget; macOS: DMG/Homebrew, đã notarize; Linux: Flatpak, AppImage, .deb |  |

## Hỗ trợ theo nền tảng

Phần cơ bản làm được trên cả ba OS; GPU theo tiến trình và mạng theo tiến trình là hai chỗ khó nhất, đặc biệt trên macOS.

| Dữ liệu / thao tác | Windows | Linux | macOS |
| --- | --- | --- | --- |
| Tiến trình, CPU, RAM | NtQuerySystemInformation, PDH | /proc | libproc, host\_statistics |
| Disk I/O theo tiến trình | Có | /proc/\[pid\]/io (cần quyền với tiến trình khác) | Có (rusage) |
| Mạng theo tiến trình | GetExtendedTcpTable + ETW | /proc/net + eBPF (cần quyền) | Hạn chế, chủ yếu qua nettop/NetworkStatistics |
| GPU tổng | DXGI, PDH | NVML, sysfs (AMD/Intel) | IOKit |
| GPU theo tiến trình | PDH GPU Engine counters | NVML, fdinfo (DRM) | Rất hạn chế |
| Nhiệt độ, quạt | WMI, thường cần driver hãng | hwmon/lm-sensors | SMC, IOHID (Apple Silicon) |
| Năng lượng theo app | Ước lượng | Ước lượng (RAPL nếu có) | Có sẵn (powermetrics cần root) |
| Startup apps | Registry Run, Startup folder, Task Scheduler | XDG autostart, systemd user | Login Items, LaunchAgents |
| Service | Service Control Manager | systemd (D-Bus) | launchd |
| Tắt tiến trình | TerminateProcess | kill/signal | kill/signal |
| Tiến trình giữ file | Restart Manager API, handle enumeration | /proc/\[pid\]/fd | lsof/libproc |

Các giá trị "Ước lượng" và "Rất hạn chế" cần prototype kiểm chứng trước khi hứa với người dùng.

## Kiến trúc

Lõi C++ dùng chung lo việc giao tiếp với OS; mỗi nền tảng có giao diện native riêng để đạt hiệu năng và cảm giác tự nhiên cao nhất.

| Lớp | Công nghệ | Vai trò |
| --- | --- | --- |
| Core | C++ | Thu thập số liệu, thao tác tiến trình/service, lịch sử, cảnh báo; không phụ thuộc UI |
| Adapter OS | C++ theo từng OS | Gọi API ở bảng "Hỗ trợ theo nền tảng", trả về cấu trúc dữ liệu chung |
| UI Windows | Win32 | Cửa sổ chính, tray widget |
| UI macOS | SwiftUI | Cửa sổ chính, menu bar widget; gọi core qua Swift–C++ interop |
| UI Linux | GTK | Cửa sổ chính, widget/indicator |
| Helper đặc quyền | C++ | Tiến trình riêng chạy quyền admin/root cho thao tác cần quyền |

**Nguyên tắc:**

1. Core đưa ra một API ổn định (C++ hoặc C ABI) mà cả ba UI cùng dùng; logic nghiệp vụ không nằm ở UI.
2. Core gom và tính toán số liệu một lần mỗi chu kỳ; UI chỉ hiển thị phần đang nhìn thấy.
3. Mỗi tính năng làm 3 lần ở tầng UI, nên UI giữ mỏng và phần dùng chung đẩy hết xuống core.

## Mô hình kinh doanh

Freemium: bản miễn phí đủ thay Task Manager gốc; một số tính năng nâng cao phải trả phí (bản Pro).

| Gói | Bao gồm | Lý do |
| --- | --- | --- |
| Miễn phí | Toàn bộ P0 và P1 | Đủ dùng hằng ngày, là thứ kéo người dùng về và tạo uy tín |
| Pro (đề xuất) | Lịch sử dài hạn (quá 24 giờ), phân tích sâu (file bị giữ, port, đóng kết nối), cảnh báo tùy chỉnh, tab năng lượng | Phần khác biệt so với đối thủ, chủ yếu hữu ích cho power user |
| Luôn miễn phí | Tắt tiến trình, xem tài nguyên, giải thích tiến trình | Tính năng cốt lõi không khóa sau tường phí |

**Nguyên tắc:**

1. License kiểm tra offline (khóa ký số), không gọi về server mỗi lần mở app, đúng nguyên tắc chạy cục bộ.
2. Bản miễn phí không quảng cáo, không nhắc nâng cấp liên tục; tính năng Pro chỉ hiện nhãn nhỏ.
3. Tính năng Pro nằm trong cùng bản cài, mở khóa bằng license; không phát hành bản cài riêng.
4. Không bán qua Mac App Store hay Microsoft Store; tự bán qua cổng thanh toán (vd. Paddle, Lemon Squeezy) và phân phối qua kênh ở mục "Phân phối".

## Lộ trình và câu hỏi còn mở

Bắt đầu bằng prototype kiểm chứng adapter, vì GPU và mạng theo tiến trình có thể không làm được trên mọi OS.

| Giai đoạn | Nội dung | Cổng để sang giai đoạn sau |
| --- | --- | --- |
| 1. Prototype | Kiểm chứng adapter trên 3 hệ điều hành, đo overhead CPU/RAM | Đạt mục tiêu hiệu năng |
| 2. MVP | Tính năng P0 trên 2 OS chính, beta kín | Beta ổn định, có phản hồi người dùng |
| 3. v1 | Tính năng P1, đủ Windows, macOS, Linux, phát hành công khai | v1 chạy ổn trên cả 3 OS |
| 4. v2 | Tính năng P2: lịch sử, năng lượng, cảnh báo | |

Chưa đặt mốc thời gian; mỗi giai đoạn chỉ bắt đầu khi qua cổng kiểm tra phía trước.

**Câu hỏi còn mở:**

- [ ] Hai OS nào làm trước cho MVP?
- [ ] GTK: dùng C API (GTK4) trực tiếp hay gtkmm?
- [ ] Phiên bản OS tối thiểu (SwiftUI và Swift Charts cần macOS 13 trở lên)?
- [ ] Giá Pro: mua một lần (kèm cập nhật 1 năm) hay thuê bao?
- [ ] Cổng thanh toán: Paddle hay Lemon Squeezy (cả hai lo thuế VAT/sales tax)?
- [ ] Chốt danh sách tính năng Pro sau khi có phản hồi beta

## Nguồn

- [Windows 11 Task Manager mới (BleepingComputer)](https://www.bleepingcomputer.com/news/microsoft/closer-look-at-windows-11s-new-task-manager/)
- [Task Manager Windows 11 24H2: Ctrl+K, GPU/NPU (kluczesoft)](https://kluczesoft.pl/wiedza/poradniki/task-manager-menedzer-zadan-windows-11-zaawansowane)
- [System Informer (Neowin)](https://www.neowin.net/software/system-informer-3225011/)
- [Activity Monitor (How-To Geek)](https://www.howtogeek.com/227240/how-to-monitor-your-macs-health-with-activity-monitor)
- [Stats (GitHub)](https://github.com/exelban/stats) và [mac-stats.com](https://mac-stats.com/)
- [Mission Center (Linuxiac)](https://linuxiac.com/mission-center-system-monitoring-app/) và [Mission Center 1.0 (OMG! Ubuntu)](https://www.omgubuntu.co.uk/2025/05/mission-center-1-0-adds-new-features)
- [btop (Pardus)](https://tracker.pardus.org.tr/yirmibir/btop)

Bảng API theo nền tảng dựa trên kiến thức chung, chưa kiểm chứng bằng tài liệu chính thức.
