#include "test.hpp"

int main() {
    for (auto& c : test::registry()) {
        int before = test::failures();
        try {
            c.fn();
        } catch (const std::exception& e) {
            ++test::failures();
            std::cerr << c.name << ": threw " << e.what() << "\n";
        }
        if (test::failures() != before) std::cerr << "FAILED: " << c.name << "\n";
    }
    std::cout << test::registry().size() << " tests, " << test::failures() << " failed checks\n";
    return test::failures() == 0 ? 0 : 1;
}
