// Windows implementation of the shared UI's platform services (platform.hpp).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <atomic>
#include <string>
#include <vector>

#include "platform.hpp"

namespace procyon::ui::platform {

namespace {

const wchar_t *const kSettingsRoot = L"Software\\Procyon";

std::wstring settings_key(std::wstring_view group) {
    std::wstring key = kSettingsRoot;
    if (!group.empty()) {
        key += L'\\';
        key += group;
    }
    return key;
}

}  // namespace

bool system_prefers_dark() {
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    return value == 0;
}

std::wstring data_file(std::wstring_view name) {
    PWSTR folder = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder)) && folder) {
        base = folder;
        CoTaskMemFree(folder);
    }
    if (base.empty()) base = L".";
    base += L"\\Procyon\\";
    base += name;
    return base;
}

void create_parent_directories(std::wstring_view path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring_view::npos) return;
    const std::wstring parent(path.substr(0, slash));
    if (parent.size() <= 3) return;  // a drive root
    create_parent_directories(parent);
    CreateDirectoryW(parent.c_str(), nullptr);
}

namespace {

// The pixels of a 32-bit bitmap as premultiplied BGRA, top-down. Shell bitmaps arrive either
// premultiplied or straight; a channel above its alpha gives a straight one away.
std::shared_ptr<const Image> image_from_bitmap(HBITMAP bitmap, int pixels) {
    static std::atomic<uint64_t> next_id{1};
    BITMAP info{};
    if (!bitmap || GetObjectW(bitmap, sizeof(info), &info) == 0 || info.bmBitsPixel != 32) return nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return nullptr;
    BITMAPINFO header{};
    header.bmiHeader.biSize = sizeof(header.bmiHeader);
    header.bmiHeader.biWidth = info.bmWidth;
    header.bmiHeader.biHeight = -info.bmHeight;  // top-down
    header.bmiHeader.biPlanes = 1;
    header.bmiHeader.biBitCount = 32;
    header.bmiHeader.biCompression = BI_RGB;
    auto image = std::make_shared<Image>();
    image->id = next_id++;
    image->width = info.bmWidth;
    image->height = info.bmHeight;
    image->pixels.resize(static_cast<size_t>(info.bmWidth) * info.bmHeight);
    const int rows =
        GetDIBits(dc, bitmap, 0, static_cast<UINT>(info.bmHeight), image->pixels.data(), &header, DIB_RGB_COLORS);
    DeleteDC(dc);
    if (rows != info.bmHeight) return nullptr;
    bool any_alpha = false, straight = false;
    for (uint32_t p : image->pixels) {
        const uint32_t a = p >> 24;
        if (a) any_alpha = true;
        if (((p >> 16) & 0xFF) > a || ((p >> 8) & 0xFF) > a || (p & 0xFF) > a) straight = true;
    }
    if (!any_alpha) {
        // No alpha channel at all: treat as opaque.
        for (uint32_t &p : image->pixels) p |= 0xFF000000u;
    } else if (straight) {
        for (uint32_t &p : image->pixels) {
            const uint32_t a = p >> 24;
            const uint32_t r = ((p >> 16) & 0xFF) * a / 255, g = ((p >> 8) & 0xFF) * a / 255, b = (p & 0xFF) * a / 255;
            p = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    (void)pixels;
    return image;
}

}  // namespace

std::shared_ptr<const Image> app_icon(std::wstring_view path, int pixels) {
    if (path.empty() || pixels <= 0) return nullptr;
    const std::wstring file(path);
    // The shell's icon at the size it will be shown (it picks the nearest resolution itself).
    IShellItem *item = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&item))) && item) {
        IShellItemImageFactory *factory = nullptr;
        HBITMAP bitmap = nullptr;
        if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&factory))) && factory) {
            const SIZE size{pixels, pixels};
            if (FAILED(factory->GetImage(size, SIIGBF_ICONONLY | SIIGBF_RESIZETOFIT, &bitmap))) bitmap = nullptr;
            factory->Release();
        }
        item->Release();
        if (bitmap) {
            std::shared_ptr<const Image> image = image_from_bitmap(bitmap, pixels);
            DeleteObject(bitmap);
            if (image) return image;
        }
    }
    // Fallback: the file's large icon drawn into a 32-bit surface.
    SHFILEINFOW info{};
    if (!SHGetFileInfoW(file.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) || !info.hIcon)
        return nullptr;
    ICONINFO icon{};
    std::shared_ptr<const Image> image;
    if (GetIconInfo(info.hIcon, &icon)) {
        image = image_from_bitmap(icon.hbmColor, pixels);
        if (icon.hbmColor) DeleteObject(icon.hbmColor);
        if (icon.hbmMask) DeleteObject(icon.hbmMask);
    }
    DestroyIcon(info.hIcon);
    return image;
}

std::wstring clipboard_text() {
    std::wstring out;
    if (!OpenClipboard(nullptr)) return out;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (auto chars = static_cast<const wchar_t *>(GlobalLock(data))) {
            out = chars;
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return out;
}

void sampler_thread_begin() {
    // COM for the WMI temperature reads; a mismatch with another apartment is harmless.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
}

void sampler_thread_end() { CoUninitialize(); }

bool tray_available() { return true; }  // the notification area is always there

std::optional<int64_t> read_setting(std::wstring_view group, std::wstring_view name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, settings_key(group).c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return std::nullopt;
    DWORD value = 0, size = sizeof(value), type = 0;
    const LSTATUS status =
        RegQueryValueExW(key, std::wstring(name).c_str(), nullptr, &type, reinterpret_cast<BYTE *>(&value), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_DWORD) return std::nullopt;
    return static_cast<int64_t>(static_cast<int32_t>(value));
}

void write_setting(std::wstring_view group, std::wstring_view name, int64_t value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, settings_key(group).c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return;
    const DWORD dword = static_cast<DWORD>(static_cast<int32_t>(value));
    RegSetValueExW(key, std::wstring(name).c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE *>(&dword),
                   sizeof(dword));
    RegCloseKey(key);
}

}  // namespace procyon::ui::platform
