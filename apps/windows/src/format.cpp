// Display formatting shared by every screen: a port of ProcyonDesign's Format (macOS).
#include <cmath>
#include <cstdio>
#include <ctime>

#include "ui.hpp"

namespace procyon::ui::fmt {

const wchar_t *const unavailable = L"—";

namespace {

std::wstring fixed(double value, int digits) {
    wchar_t buffer[64];
    (void)swprintf_s(buffer, L"%.*f", digits, value);
    return buffer;
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
    const time_t t = static_cast<time_t>(unix_seconds);
    tm local{};
    if (localtime_s(&local, &t) != 0) return unavailable;
    wchar_t buffer[32];
    (void)wcsftime(buffer, 32, L"%H:%M", &local);
    return buffer;
}

std::wstring date_time(int64_t unix_seconds) {
    if (unix_seconds <= 0) return unavailable;
    const time_t t = static_cast<time_t>(unix_seconds);
    tm local{};
    if (localtime_s(&local, &t) != 0) return unavailable;
    wchar_t buffer[64];
    (void)wcsftime(buffer, 64, L"%b %d, %Y %H:%M", &local);
    return buffer;
}

std::pair<std::wstring, std::wstring> split_unit(const std::wstring &text) {
    const size_t space = text.find_last_of(L' ');
    if (space == std::wstring::npos) return {text, L""};
    return {text.substr(0, space), text.substr(space + 1)};
}

std::wstring from_utf8(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string to_utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

}  // namespace procyon::ui::fmt
