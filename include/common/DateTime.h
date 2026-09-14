#pragma once

// =============================================================================
//  DateTime 模块 —— 45_datetime 的核心支持。
//
// 语义说明：
//  - 当前实现不引入时区概念；所有 DATE / TIMESTAMP 视为"无时区本地时间"。
//    FormatDouble / FormatDate 等只在进程内解释，不携带时区偏移信息。
//  - 比较与算术均以 int64 epoch 秒（在 UTC 时间轴上）作为唯一权威表示；
//    文本形式仅作输入/输出。
//  - DATE 字面量取 00:00:00 作为一天内的时刻；TIMESTAMP 字面量精确到秒。
//  - 日期加减语义：
//      DAY      : 日历日（24*3600 秒）。
//      WEEK     : 7 个日历日（语义等价于 DAY*7，本任务未公开该单位）。
//      HOUR     : 3600 秒。
//      MINUTE   : 60 秒。
//      SECOND   : 1 秒。
//      MONTH    : 日历月，月末按"目标月最大天数"做夹取（end-of-month clamping）。
//      YEAR     : 日历年，闰年 2-29 加上非闰年时按目标年最大天数夹取。
//    这与主流 SQL 方言的 INTERVAL 行为一致；超出 int64 表示范围返回 false。
// =============================================================================

#include <cstdint>
#include <string>

namespace sqlcompiler {

// 把 "YYYY-MM-DD" / "YYYY-MM-DD HH:MM:SS" / "YYYY-MM-DDTHH:MM:SS" 解析为
// epoch 秒（UTC 时间轴上的绝对秒数）。返回是否成功。
//
// 仅识别 '-' 作为日期分隔符、':' 作为时间分隔符、' ' / 'T' 作为日期-时间分隔符。
// 解析失败返回 false，epoch_out 不被修改。
bool ParseDateTime(const std::string& s, int64_t* epoch_out);

// 把 epoch 秒格式化为 "YYYY-MM-DD HH:MM:SS"（始终带时间分量，便于与 TIMESTAMP
// 类型保持一致）。
std::string FormatDateTime(int64_t epoch);

// 把 epoch 秒格式化为 "YYYY-MM-DD"（不带时间）。
std::string FormatDate(int64_t epoch);

// 提取 epoch 秒对应的年/月/日/时/分/秒（按本地时间口径，便于语义直观）。
void ExtractDateTimeParts(int64_t epoch,
                          int* year, int* month, int* day,
                          int* hour, int* minute, int* second);

// 仅支持 YEAR / MONTH / DAY / HOUR / MINUTE / SECOND。
enum class IntervalUnit { YEAR, MONTH, DAY, HOUR, MINUTE, SECOND };

// 单位字符串解析（大小写不敏感）。成功时返回 true，并写出 unit。
bool ParseIntervalUnit(const std::string& s, IntervalUnit* unit);

// 单位转字符串（用于报错与 ToString）。
const char* IntervalUnitName(IntervalUnit unit);

// 给定 epoch 与 (count, unit)，计算 (epoch ± count*unit_seconds) 后的新 epoch。
// MONTH / YEAR 使用日历加减（依赖 ExtractDateTimeParts / DaysInMonth / IsLeapYear），
// 会做月末夹取。返回是否成功（仅在溢出 int64 时返回 false）。
//
// sign 为 +1 时为加法（INTERVAL_ADD），-1 时为减法（INTERVAL_SUB）。
bool ApplyInterval(int64_t epoch, int64_t count, IntervalUnit unit, int sign,
                   int64_t* out_epoch);

// 计算 yyyy-mm-dd 的"下个月"或"上个月"，做日历日 clamp。
// delta_months 可为正（向后）或负（向前）。
// 仅用于 ApplyInterval 的实现；调用方应通过 ApplyInterval 间接使用。
bool AddCalendarMonths(int y, int m, int d, int delta_months,
                       int* out_y, int* out_m, int* out_d);

}  // namespace sqlcompiler
