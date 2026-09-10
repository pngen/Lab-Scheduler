#pragma once

// Minimal deterministic test framework. It has no timeouts, no watchdogs and no
// way to skip work silently: a test either runs and reports, or it fails loudly.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace lab_scheduler {
namespace test {

struct TestCase {
    const char* name;
    void (*function)();
};

std::vector<TestCase>& registry();
int& failure_count();
int& check_count();
std::string& current_test();
std::vector<std::string>& failure_messages();

void register_test(const char* name, void (*function)());
void report_failure(const char* file, int line, const std::string& message);

// Deterministic pseudo random generator: property tests print their seed and
// reproduce exactly from it.
class Rng {
public:
    explicit Rng(std::uint64_t seed) noexcept : state_(seed == 0 ? 0x9e3779b97f4a7c15ull : seed) {}

    std::uint64_t next() noexcept {
        state_ += 0x9e3779b97f4a7c15ull;
        std::uint64_t value = state_;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
        return value ^ (value >> 31);
    }

    std::uint64_t range(std::uint64_t bound) noexcept { return bound == 0 ? 0 : next() % bound; }

    std::uint32_t u32(std::uint32_t bound) noexcept {
        return bound == 0 ? 0 : static_cast<std::uint32_t>(range(bound));
    }

    bool chance(unsigned numerator, unsigned denominator) noexcept {
        return denominator != 0 && u32(denominator) < numerator;
    }

    [[nodiscard]] std::uint64_t seed() const noexcept { return state_; }

private:
    std::uint64_t state_ = 0;
};

int run_all(int argc, char** argv);

}  // namespace test
}  // namespace lab_scheduler

#define LS_TEST(NAME)                                                                        \
    static void NAME();                                                                      \
    static const bool NAME##_registered = []() {                                             \
        ::lab_scheduler::test::register_test(#NAME, &NAME);                                  \
        return true;                                                                         \
    }();                                                                                     \
    static void NAME()

#define LS_CHECK(CONDITION)                                                                  \
    do {                                                                                     \
        ::lab_scheduler::test::check_count() += 1;                                           \
        if (!(CONDITION)) {                                                                  \
            ::lab_scheduler::test::report_failure(__FILE__, __LINE__, #CONDITION);           \
        }                                                                                    \
    } while (false)

#define LS_CHECK_EQ(ACTUAL, EXPECTED)                                                        \
    do {                                                                                     \
        ::lab_scheduler::test::check_count() += 1;                                           \
        const auto ls_actual_value = (ACTUAL);                                               \
        const auto ls_expected_value = (EXPECTED);                                           \
        if (!(ls_actual_value == ls_expected_value)) {                                       \
            ::lab_scheduler::test::report_failure(__FILE__, __LINE__,                        \
                                                  std::string(#ACTUAL " != " #EXPECTED));    \
        }                                                                                    \
    } while (false)

#define LS_CHECK_MESSAGE(CONDITION, MESSAGE)                                                 \
    do {                                                                                     \
        ::lab_scheduler::test::check_count() += 1;                                           \
        if (!(CONDITION)) {                                                                  \
            ::lab_scheduler::test::report_failure(__FILE__, __LINE__,                        \
                                                  std::string(#CONDITION " :: ") + (MESSAGE));\
        }                                                                                    \
    } while (false)
