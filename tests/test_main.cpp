#include "test_framework.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace lab_scheduler {
namespace test {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

int& failure_count() {
    static int count = 0;
    return count;
}

int& check_count() {
    static int count = 0;
    return count;
}

std::string& current_test() {
    static std::string name;
    return name;
}

std::vector<std::string>& failure_messages() {
    static std::vector<std::string> messages;
    return messages;
}

void register_test(const char* name, void (*function)()) { registry().push_back(TestCase{name, function}); }

void report_failure(const char* file, int line, const std::string& message) {
    ++failure_count();
    std::string text = current_test();
    text += " (";
    text += file;
    text += ":";
    text += std::to_string(line);
    text += "): ";
    text += message;
    failure_messages().push_back(text);
    std::printf("FAIL %s\n", text.c_str());
    std::fflush(stdout);
}

int run_all(int argc, char** argv) {
    std::string filter;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
            filter = argv[i + 1];
        }
    }
    int executed = 0;
    for (const TestCase& test : registry()) {
        if (!filter.empty() && std::string(test.name).find(filter) == std::string::npos) {
            continue;
        }
        current_test() = test.name;
        const int before = failure_count();
        test.function();
        ++executed;
        const int after = failure_count();
        std::printf("%s %s\n", after == before ? "ok  " : "FAIL", test.name);
        std::fflush(stdout);
    }
    std::printf("tests executed=%d checks=%d failures=%d\n", executed, check_count(), failure_count());
    std::fflush(stdout);
    return failure_count() == 0 ? 0 : 1;
}

}  // namespace test
}  // namespace lab_scheduler

int main(int argc, char** argv) {
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    return lab_scheduler::test::run_all(argc, argv);
}
