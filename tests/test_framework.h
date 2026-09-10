#pragma once

// 轻量级测试框架（无第三方依赖，C++17）
//
// 用法示例：
//   #include "test_framework.h"
//
//   static void TestFoo() {
//       CHECK(1 + 1 == 2);
//       CHECK_EQ(std::string("a"), "a");
//   }
//
//   int main() {
//       testfw::Run("foo", TestFoo);
//       return testfw::Summary("lexer_test");
//   }
//
// 说明：
//   - 断言失败只记录，不中断当前测试用例（一个用例可暴露全部问题）；
//   - 未捕获异常会终止当前用例并计为失败；
//   - Summary() 返回进程退出码：全部通过返回 0。

#include <cstdio>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

namespace testfw {

// ---- 全局统计（inline 变量，C++17） ----
inline std::string& CurrentName() { static std::string n; return n; }
inline int& TestsPassed() { static int n = 0; return n; }
inline int& TestsFailed() { static int n = 0; return n; }
inline int& AssertFailed() { static int n = 0; return n; }
inline std::vector<std::string>& FailureLog() {
    static std::vector<std::string> log;
    return log;
}

// 记录一次断言失败（打印并累计）
inline void ReportFailure(const char* file, int line, const std::string& what) {
    ++AssertFailed();
    std::ostringstream oss;
    oss << "    FAIL  " << file << ":" << line << "  " << what;
    FailureLog().push_back(oss.str());
    std::printf("%s\n", oss.str().c_str());
}

// 任意可流式输出的值转字符串（用于失败信息展示）
template <typename T>
std::string Str(const T& v) {
    std::ostringstream oss;
    oss << v;
    return oss.str();
}

// 注册并执行一个测试用例
inline void Run(const std::string& name, void (*fn)()) {
    CurrentName() = name;
    const int baseline = AssertFailed();
    std::printf("[ RUN  ] %s\n", name.c_str());
    try {
        fn();
    } catch (const std::exception& e) {
        ReportFailure("<framework>", 0, std::string("未捕获异常: ") + e.what());
    } catch (...) {
        ReportFailure("<framework>", 0, "未捕获的非标准异常");
    }
    if (AssertFailed() == baseline) {
        ++TestsPassed();
        std::printf("[  OK  ] %s\n", name.c_str());
    } else {
        ++TestsFailed();
    }
    CurrentName().clear();
}

// 打印套件总结，返回退出码
inline int Summary(const char* suite_name) {
    std::printf("\n==== %s: %d 个用例通过, %d 个用例失败 (断言失败共 %d 处) ====\n",
                suite_name, TestsPassed(), TestsFailed(), AssertFailed());
    return TestsFailed() == 0 ? 0 : 1;
}

}  // namespace testfw

// ---- 断言宏 ----

// 条件为真；失败时打印表达式原文
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            testfw::ReportFailure(__FILE__, __LINE__,                        \
                                  std::string("断言失败: ") + #cond);        \
        }                                                                    \
    } while (0)

// 相等断言；失败时打印实际值与期望值
#define CHECK_EQ(actual, expected)                                           \
    do {                                                                     \
        const auto& va_ = (actual);                                          \
        const auto& ve_ = (expected);                                        \
        if (!(va_ == ve_)) {                                                 \
            testfw::ReportFailure(__FILE__, __LINE__,                        \
                std::string("断言失败: ") + #actual + " == " + #expected +   \
                "  (实际: " + testfw::Str(va_) +                             \
                ", 期望: " + testfw::Str(ve_) + ")");                        \
        }                                                                    \
    } while (0)

// 期望语句抛出 std::exception 异常
#define CHECK_THROW(stmt)                                                    \
    do {                                                                     \
        bool thrown_ = false;                                                \
        try {                                                                \
            (void)(stmt);                                                    \
        } catch (const std::exception&) {                                    \
            thrown_ = true;                                                  \
        } catch (...) {                                                      \
            thrown_ = true;                                                  \
        }                                                                    \
        if (!thrown_) {                                                      \
            testfw::ReportFailure(__FILE__, __LINE__,                        \
                std::string("期望抛出异常但未抛出: ") + #stmt);              \
        }                                                                    \
    } while (0)
