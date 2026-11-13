#include "check.h"

#include <cstring>

int main(int argc, char **argv) {
    const char *filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0, failed_cases = 0;
    for (const auto &c : check::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        const int before = check::failures();
        c.fn();
        ++ran;
        const bool ok = check::failures() == before;
        if (!ok) ++failed_cases;
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", c.name);
    }
    std::printf("%d run, %d failed\n", ran, failed_cases);
    return failed_cases == 0 && ran > 0 ? 0 : 1;
}
