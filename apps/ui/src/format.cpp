// Display formatting shared by every screen: a port of ProcyonDesign's Format (macOS).
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cwchar>

#include "ui.hpp"

namespace procyon::ui::fmt {

const wchar_t *const unavailable = L"—";

namespace {

std::wstring fixed(double value, int digits) {
    wchar_t buffer[64];
    (void)std::swprintf(buffer, 64, L"%.*f", digits, value);
    return buffer;
}

bool local_time(int64_t unix_seconds, tm &out) {
    const time_t t = static_cast<time_t>(unix_seconds);
#ifdef _WIN32
    return localtime_s(&out, &t) == 0;
#else
    return localtime_r(&t, &out) != nullptr;
#endif
}

}  // namespace

std::wstring number(double value, int digits) { return fixed(value, digits); }

std::wstring percent(double fraction, int digits) {
    if (!std::isfinite(fraction) || fraction < 0) return unavailable;
    return fixed(fraction * 100, digits) + L"%";
}

std::wstring cpu(double percent) {
    if (!std::isfinite(percent) || percent < 0) return unavailable;
    return fixed(percent, percent >= 100 ? 0 : 1) + L"%";
}

std::wstring bytes(double value) {
    static const wchar_t *const units[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB"};
    double amount = value;
    int unit = 0;
    while (amount >= 1024 && unit < 5) {
        amount /= 1024;
        ++unit;
    }
    if (unit == 0) return fixed(std::floor(amount), 0) + L" B";
    const int digits = amount >= 100 ? 0 : amount >= 10 ? 1 : 2;
    return fixed(amount, digits) + L" " + units[unit];
}

std::wstring bytes(int64_t value) {
    if (value < 0) return unavailable;
    return bytes(static_cast<double>(value));
}

std::wstring endpoint(std::wstring_view address, int port) {
    const std::wstring a(address);
    return (a.find(L':') != std::wstring::npos ? L"[" + a + L"]" : a) + L":" + std::to_wstring(port);
}

std::wstring rate(double value) {
    if (!std::isfinite(value) || value < 0) return unavailable;
    if (value < 1024) return fixed(std::floor(value), 0) + L" B/s";
    return bytes(value) + L"/s";
}

std::wstring count(int64_t value) {
    if (value < 0) return unavailable;
    std::wstring digits = std::to_wstring(value);
    std::wstring out;
    int n = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (n && n % 3 == 0) out.insert(out.begin(), L',');
        out.insert(out.begin(), *it);
        ++n;
    }
    return out;
}

std::wstring duration(double seconds) {
    const int64_t total = static_cast<int64_t>(seconds < 0 ? 0 : seconds);
    const int64_t days = total / 86400, hours = total % 86400 / 3600, minutes = total % 3600 / 60;
    if (days > 0)
        return std::to_wstring(days) + L"d " + std::to_wstring(hours) + L"h " + std::to_wstring(minutes) + L"m";
    if (hours > 0) return std::to_wstring(hours) + L"h " + std::to_wstring(minutes) + L"m";
    return std::to_wstring(minutes) + L"m " + std::to_wstring(total % 60) + L"s";
}

std::wstring temperature(double celsius, bool fahrenheit) {
    if (!std::isfinite(celsius) || celsius < 0) return unavailable;
    if (fahrenheit) return fixed(celsius * 9 / 5 + 32, 0) + L"°F";
    return fixed(celsius, 0) + L"°C";
}

std::wstring watts(double value) {
    if (!std::isfinite(value) || value < 0) return unavailable;
    if (value < 0.005) return L"0 W";
    const int digits = value < 1 ? 2 : value < 10 ? 1 : 0;
    return fixed(value, digits) + L" W";
}

std::wstring interval(double seconds) { return fixed(seconds, seconds < 1 ? 1 : 0) + L"s"; }

std::wstring time_of_day(int64_t unix_seconds) {
    if (unix_seconds <= 0) return unavailable;
    tm local{};
    if (!local_time(unix_seconds, local)) return unavailable;
    wchar_t buffer[32];
    (void)wcsftime(buffer, 32, L"%H:%M", &local);
    return buffer;
}

std::wstring date_time(int64_t unix_seconds) {
    if (unix_seconds <= 0) return unavailable;
    tm local{};
    if (!local_time(unix_seconds, local)) return unavailable;
    wchar_t buffer[64];
    (void)wcsftime(buffer, 64, L"%b %d, %Y %H:%M", &local);
    return buffer;
}

std::pair<std::wstring, std::wstring> split_unit(const std::wstring &text) {
    const size_t space = text.find_last_of(L' ');
    if (space == std::wstring::npos) return {text, L""};
    return {text.substr(0, space), text.substr(space + 1)};
}

// UTF-8 <-> wide strings without the platform's converter: wchar_t is UTF-16 on Windows and
// UTF-32 on Linux, so both encodings are handled. Malformed input decodes as U+FFFD.
std::wstring from_utf8(std::string_view text) {
    std::wstring out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        uint32_t code = 0;
        size_t length = 0;
        if (lead < 0x80) {
            code = lead;
            length = 1;
        } else if ((lead & 0xE0) == 0xC0) {
            code = lead & 0x1F;
            length = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            code = lead & 0x0F;
            length = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            code = lead & 0x07;
            length = 4;
        } else {
            code = 0xFFFD;
            length = 1;
        }
        if (i + length > text.size()) {
            code = 0xFFFD;
            length = text.size() - i;
        } else {
            for (size_t k = 1; k < length; ++k) {
                const auto byte = static_cast<unsigned char>(text[i + k]);
                if ((byte & 0xC0) != 0x80) {
                    code = 0xFFFD;
                    length = k;
                    break;
                }
                code = (code << 6) | (byte & 0x3F);
            }
        }
        i += std::max<size_t>(length, 1);
        if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) code = 0xFFFD;
        if constexpr (sizeof(wchar_t) == 2) {
            if (code >= 0x10000) {
                code -= 0x10000;
                out.push_back(static_cast<wchar_t>(0xD800 + (code >> 10)));
                out.push_back(static_cast<wchar_t>(0xDC00 + (code & 0x3FF)));
            } else {
                out.push_back(static_cast<wchar_t>(code));
            }
        } else {
            out.push_back(static_cast<wchar_t>(code));
        }
    }
    return out;
}

std::string to_utf8(std::wstring_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t code = static_cast<uint32_t>(text[i]) & (sizeof(wchar_t) == 2 ? 0xFFFF : 0xFFFFFFFF);
        if (sizeof(wchar_t) == 2 && code >= 0xD800 && code <= 0xDBFF && i + 1 < text.size()) {
            const uint32_t low = static_cast<uint32_t>(text[i + 1]) & 0xFFFF;
            if (low >= 0xDC00 && low <= 0xDFFF) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        if (code >= 0xD800 && code <= 0xDFFF) code = 0xFFFD;
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return out;
}

}  // namespace procyon::ui::fmt
