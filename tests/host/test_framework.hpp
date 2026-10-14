/* ============================================================================
 * test_framework.hpp -- a ~100-line test runner for the host test build.
 *
 * Deliberately not a dependency. This repo is a reverse-engineering rebuild for
 * a car part; it should stay buildable from a bare checkout with nothing but a
 * compiler and Python, and the tests here assert plain values -- nothing a
 * framework would do better.
 *
 * Usage:
 *     TEST(soft_timer_ages_every_slot)
 *     {
 *         ram::app().softTimers[3] = 2;
 *         svc::timers::tickAll();
 *         CHECK_EQ(ram::app().softTimers[3], 1);
 *     }
 *
 * Tests self-register at static-init time. That is fine HERE and only here:
 * this is the host harness, not the firmware image, which never runs
 * .init_array (see docs/misra-cpp.md, deviation D-2).
 * ==========================================================================*/

#ifndef TEST_FRAMEWORK_HPP
#define TEST_FRAMEWORK_HPP

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace testing {

using TestFn = void (*)();

struct TestCase {
    const char* name;
    TestFn      fn;
    TestCase*   next;
};

/* Intrusive singly-linked registry; no allocation, no ordering guarantees. */
inline TestCase*& registry() noexcept
{
    static TestCase* head = nullptr;
    return head;
}

struct Registrar {
    Registrar(TestCase* tc) noexcept
    {
        tc->next = registry();
        registry() = tc;
    }
};

/* Failure bookkeeping for the currently running case. */
inline int& failureCount() noexcept
{
    static int n = 0;
    return n;
}

inline void reportFailure(const char* file, int line, const char* expr,
                          long long got, long long want) noexcept
{
    std::printf("    FAIL %s:%d\n      %s\n      got      %lld (0x%llX)\n"
                "      expected %lld (0x%llX)\n",
                file, line, expr, got, static_cast<unsigned long long>(got),
                want, static_cast<unsigned long long>(want));
    ++failureCount();
}

inline void reportFailure(const char* file, int line, const char* expr) noexcept
{
    std::printf("    FAIL %s:%d\n      %s\n", file, line, expr);
    ++failureCount();
}

int run(const char* filter) noexcept;

}  // namespace testing

/* ---- the macros --------------------------------------------------------- */

#define TEST(name)                                                            \
    static void test_##name();                                                \
    static ::testing::TestCase tc_##name{#name, &test_##name, nullptr};       \
    static ::testing::Registrar reg_##name{&tc_##name};                       \
    static void test_##name()

/* Compares as signed 64-bit, which is wide enough for every fixed-width type
 * this firmware uses and keeps the macro to one overload. */
#define CHECK_EQ(got, want)                                                   \
    do {                                                                      \
        const long long g_ = static_cast<long long>(got);                     \
        const long long w_ = static_cast<long long>(want);                    \
        if (g_ != w_) {                                                       \
            ::testing::reportFailure(__FILE__, __LINE__,                      \
                                     #got " == " #want, g_, w_);              \
        }                                                                     \
    } while (false)

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            ::testing::reportFailure(__FILE__, __LINE__, #cond);              \
        }                                                                     \
    } while (false)

#define CHECK_FALSE(cond) CHECK(!(cond))

/* Byte-range compare, for the frame buffers this firmware is mostly made of. */
#define CHECK_BYTES(got_ptr, want_ptr, len)                                   \
    do {                                                                      \
        for (std::size_t i_ = 0; i_ < static_cast<std::size_t>(len); ++i_) {  \
            const long long g_ =                                              \
                static_cast<long long>((got_ptr)[i_]);                        \
            const long long w_ =                                              \
                static_cast<long long>((want_ptr)[i_]);                       \
            if (g_ != w_) {                                                   \
                ::testing::reportFailure(__FILE__, __LINE__,                  \
                                         #got_ptr "[i] == " #want_ptr "[i]",  \
                                         g_, w_);                             \
                break;                                                        \
            }                                                                 \
        }                                                                     \
    } while (false)

#endif /* TEST_FRAMEWORK_HPP */
