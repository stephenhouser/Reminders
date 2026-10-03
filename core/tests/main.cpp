#include <string_view>

#include "test.hpp"

// core_tests [NAME…]: every test, or only those whose name contains a NAME.
int main(int argc, char** argv) {
    int run = 0;
    for (auto& c : test::registry()) {
        bool wanted = argc < 2;
        for (int i = 1; i < argc; ++i) wanted = wanted || std::string_view(c.name).find(argv[i]) != std::string_view::npos;
        if (!wanted) continue;
        ++run;
        int before = test::failures();
        try {
            c.fn();
        } catch (const std::exception& e) {
            ++test::failures();
            std::cerr << c.name << ": threw " << e.what() << "\n";
        }
        if (test::failures() != before) std::cerr << "FAILED: " << c.name << "\n";
    }
    std::cout << run << " tests, " << test::failures() << " failed checks\n";
    return test::failures() == 0 ? 0 : 1;
}
