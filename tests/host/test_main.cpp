/* ============================================================================
 * test_main.cpp -- the host test runner's entry point.
 *
 *     python tools/test.py              run everything
 *     python tools/test.py -k steering  run cases whose name contains "steering"
 * ==========================================================================*/

#include "test_framework.hpp"

#include <cstdio>
#include <cstring>

namespace testing {

int run(const char* filter) noexcept
{
    /* The registry is built by static initialisers in reverse source order;
     * walk it as-is -- these cases are independent by construction (every one
     * starts with fake::reset()). */
    std::size_t total = 0U;
    std::size_t failed = 0U;
    std::size_t skipped = 0U;

    for (const TestCase* tc = registry(); tc != nullptr; tc = tc->next) {
        if (filter != nullptr && std::strstr(tc->name, filter) == nullptr) {
            ++skipped;
            continue;
        }

        ++total;
        const int before = failureCount();
        tc->fn();
        const int after = failureCount();

        if (after != before) {
            ++failed;
            std::printf("  [FAIL] %s\n", tc->name);
        }
    }

    std::printf("\n%zu test%s run, %zu failed", total, total == 1U ? "" : "s", failed);
    if (skipped != 0U) { std::printf(", %zu filtered out", skipped); }
    std::printf("\n");

    return (failed == 0U) ? 0 : 1;
}

}  // namespace testing

int main(int argc, char** argv)
{
    const char* filter = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-k") == 0 && (i + 1) < argc) {
            filter = argv[i + 1];
            ++i;
        }
    }

    std::printf("canbox host tests\n");
    return ::testing::run(filter);
}
