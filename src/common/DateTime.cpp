// =============================================================================
//  DateTime 实现 —— 见 include/common/DateTime.h 顶部语义说明。
// =============================================================================

#include "common/DateTime.h"

#include <cctype>
#include <ctime>
#include <string>

namespace sqlcompiler {

namespace {

bool IsLeapYear(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

int DaysInMonth(int y, int m) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30,
                                31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) return 0;
    if (m == 2 && IsLeapYear(y)) return 29;
    return kDays[m - 1];
}

// 把 UTC 的 (y,mo,d,h,mi,s) 折叠成 epoch 秒（timegm 等价物）。
// 失败时返回 false。
bool UtcToEpoch(int y, int mo, int d, int h, int mi, int s, int64_t* out) {
    if (y < 1970 || y > 9999) return false;
    if (mo < 1 || mo > 12) return false;
    if (d < 1 || d > DaysInMonth(y, mo)) return false;
    if (h < 0 || h > 23) return false;
    if (mi < 0 || mi > 59) return false;
    if (s < 0 || s > 60) return false;  // 容忍闰秒
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    // timegm 把 tm 解释为 UTC，返回 epoch 秒。Windows 没有 timegm，
    // 用 _mkgmtime（VC++ 扩展）作为等价物。
#if defined(_WIN32)
    int64_t secs = static_cast<int64_t>(_mkgmtime(&tm));
    if (secs == static_cast<int64_t>(-1)) return false;
#else
    time_t secs = timegm(&tm);
    if (secs == static_cast<time_t>(-1)) return false;
#endif
    *out = static_cast<int64_t>(secs);
    return true;
}

void EpochToUtcParts(int64_t epoch,
                     int* y, int* mo, int* d,
                     int* h, int* mi, int* s) {
    std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    if (y) *y = tm.tm_year + 1900;
    if (mo) *mo = tm.tm_mon + 1;
    if (d) *d = tm.tm_mday;
    if (h) *h = tm.tm_hour;
    if (mi) *mi = tm.tm_min;
    if (s) *s = tm.tm_sec;
}

// ---------- string <-> epoch ----------

bool ReadDigits(const std::string& s, size_t* pos, int min_digits, int max_digits,
                int* out) {
    size_t start = *pos;
    int v = 0;
    bool any = false;
    while (*pos < s.size() &&
           *pos - start < static_cast<size_t>(max_digits) &&
           std::isdigit(static_cast<unsigned char>(s[*pos]))) {
        v = v * 10 + (s[*pos] - '0');
        ++(*pos);
        any = true;
    }
    if (!any || *pos - start < static_cast<size_t>(min_digits)) return false;
    *out = v;
    return true;
}

}  // namespace

bool ParseDateTime(const std::string& s, int64_t* epoch_out) {
    if (s.empty()) return false;
    size_t pos = 0;
    int y = 0, mo = 0, d = 0;
    if (!ReadDigits(s, &pos, 4, 4, &y)) return false;
    if (pos >= s.size() || s[pos] != '-') return false;
    ++pos;
    if (!ReadDigits(s, &pos, 1, 2, &mo)) return false;
    if (pos >= s.size() || s[pos] != '-') return false;
    ++pos;
    if (!ReadDigits(s, &pos, 1, 2, &d)) return false;
    // 默认时分秒 = 0（纯 DATE 字面量）
    int h = 0, mi = 0, sec = 0;
    if (pos < s.size()) {
        char sep = s[pos];
        if (sep != ' ' && sep != 'T') return false;
        ++pos;
        if (!ReadDigits(s, &pos, 1, 2, &h)) return false;
        if (pos >= s.size() || s[pos] != ':') return false;
        ++pos;
        if (!ReadDigits(s, &pos, 1, 2, &mi)) return false;
        if (pos < s.size() && s[pos] == ':') {
            ++pos;
            if (!ReadDigits(s, &pos, 1, 2, &sec)) return false;
        }
    }
    if (pos != s.size()) return false;  // 尾部多余字符视为不合法
    return UtcToEpoch(y, mo, d, h, mi, sec, epoch_out);
}

std::string FormatDateTime(int64_t epoch) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    EpochToUtcParts(epoch, &y, &mo, &d, &h, &mi, &s);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  y, mo, d, h, mi, s);
    return std::string(buf);
}

std::string FormatDate(int64_t epoch) {
    int y = 0, mo = 0, d = 0;
    EpochToUtcParts(epoch, &y, &mo, &d, nullptr, nullptr, nullptr);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, mo, d);
    return std::string(buf);
}

void ExtractDateTimeParts(int64_t epoch,
                          int* year, int* month, int* day,
                          int* hour, int* minute, int* second) {
    EpochToUtcParts(epoch, year, month, day, hour, minute, second);
}

bool ParseIntervalUnit(const std::string& s, IntervalUnit* unit) {
    std::string up;
    up.reserve(s.size());
    for (char c : s) up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    if (up == "YEAR" || up == "YEARS")       { *unit = IntervalUnit::YEAR;   return true; }
    if (up == "MONTH" || up == "MONTHS")     { *unit = IntervalUnit::MONTH;  return true; }
    if (up == "DAY" || up == "DAYS")         { *unit = IntervalUnit::DAY;    return true; }
    if (up == "HOUR" || up == "HOURS")       { *unit = IntervalUnit::HOUR;   return true; }
    if (up == "MINUTE" || up == "MINUTES")   { *unit = IntervalUnit::MINUTE; return true; }
    if (up == "SECOND" || up == "SECONDS")   { *unit = IntervalUnit::SECOND; return true; }
    return false;
}

const char* IntervalUnitName(IntervalUnit unit) {
    switch (unit) {
        case IntervalUnit::YEAR:   return "YEAR";
        case IntervalUnit::MONTH:  return "MONTH";
        case IntervalUnit::DAY:    return "DAY";
        case IntervalUnit::HOUR:   return "HOUR";
        case IntervalUnit::MINUTE: return "MINUTE";
        case IntervalUnit::SECOND: return "SECOND";
    }
    return "?";
}

bool AddCalendarMonths(int y, int m, int d, int delta_months,
                       int* out_y, int* out_m, int* out_d) {
    // 把月份折叠为从 1970-01 起的连续计数（避免溢出）。正负方向分别计算。
    long long total = static_cast<long long>(y) * 12 + (m - 1) + delta_months;
    long long new_total = total;
    long long ny = new_total / 12;
    long long nm = new_total % 12;
    if (nm < 0) { nm += 12; ny -= 1; }
    int new_y = static_cast<int>(ny);
    int new_m = static_cast<int>(nm) + 1;
    if (new_y < 1970 || new_y > 9999) return false;
    // 月末夹取：若 d 超过目标月最大天数，按目标月最大天数保留。
    int max_d = DaysInMonth(new_y, new_m);
    int new_d = (d > max_d) ? max_d : d;
    *out_y = new_y;
    *out_m = new_m;
    *out_d = new_d;
    return true;
}

namespace {
bool AddCalendarYears(int y, int m, int d, int delta_years,
                      int* out_y, int* out_m, int* out_d) {
    int new_y = y + static_cast<int>(delta_years);
    if (new_y < 1970 || new_y > 9999) return false;
    int max_d = DaysInMonth(new_y, m);
    int new_d = (d > max_d) ? max_d : d;
    *out_y = new_y;
    *out_m = m;
    *out_d = new_d;
    return true;
}
}  // namespace

bool ApplyInterval(int64_t epoch, int64_t count, IntervalUnit unit, int sign,
                   int64_t* out_epoch) {
    if (!out_epoch) return false;
    if (count == 0) { *out_epoch = epoch; return true; }
    int s = (sign >= 0) ? 1 : -1;
    int64_t delta = count * s;

    switch (unit) {
        case IntervalUnit::SECOND:
            if (delta > 0 && epoch > INT64_MAX - delta) return false;
            if (delta < 0 && epoch < INT64_MIN - delta) return false;
            *out_epoch = epoch + delta;
            return true;
        case IntervalUnit::MINUTE:
            delta *= 60;
            if (delta > 0 && epoch > INT64_MAX - delta) return false;
            if (delta < 0 && epoch < INT64_MIN - delta) return false;
            *out_epoch = epoch + delta;
            return true;
        case IntervalUnit::HOUR:
            delta *= 3600;
            if (delta > 0 && epoch > INT64_MAX - delta) return false;
            if (delta < 0 && epoch < INT64_MIN - delta) return false;
            *out_epoch = epoch + delta;
            return true;
        case IntervalUnit::DAY:
            delta *= 86400;
            if (delta > 0 && epoch > INT64_MAX - delta) return false;
            if (delta < 0 && epoch < INT64_MIN - delta) return false;
            *out_epoch = epoch + delta;
            return true;
        case IntervalUnit::MONTH: {
            int y = 0, mo = 0, d = 0;
            EpochToUtcParts(epoch, &y, &mo, &d, nullptr, nullptr, nullptr);
            int ny = 0, nmo = 0, nd = 0;
            if (!AddCalendarMonths(y, mo, d, static_cast<int>(delta),
                                   &ny, &nmo, &nd)) {
                return false;
            }
            int h = 0, mi = 0, s = 0;
            EpochToUtcParts(epoch, nullptr, nullptr, nullptr, &h, &mi, &s);
            return UtcToEpoch(ny, nmo, nd, h, mi, s, out_epoch);
        }
        case IntervalUnit::YEAR: {
            int y = 0, mo = 0, d = 0;
            EpochToUtcParts(epoch, &y, &mo, &d, nullptr, nullptr, nullptr);
            int ny = 0, nmo = 0, nd = 0;
            if (!AddCalendarYears(y, mo, d, static_cast<int>(delta),
                                  &ny, &nmo, &nd)) {
                return false;
            }
            int h = 0, mi = 0, s = 0;
            EpochToUtcParts(epoch, nullptr, nullptr, nullptr, &h, &mi, &s);
            return UtcToEpoch(ny, nmo, nd, h, mi, s, out_epoch);
        }
    }
    return false;
}

}  // namespace sqlcompiler
