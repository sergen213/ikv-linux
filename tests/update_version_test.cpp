// Strict SemVer ordering used by launcher self-update (handoff and update offer).
#include "ikv/updates.hpp"

#include <cstdio>
#include <stdexcept>

int main() {
    using ikv::updates::compare_versions;
    int failures = 0;
    const auto check = [&](bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; } };
    // SemVer 2.0 section 11 example chain, plus multi-digit numeric fields.
    const char* ordered[] = {"0.3.0", "0.4.0-alpha", "0.4.0-alpha.1", "0.4.0-alpha.beta", "0.4.0-beta", "0.4.0-beta.2",
                             "0.4.0-beta.11", "0.4.0-rc.1", "0.4.0", "0.4.1", "0.10.0", "1.0.0", "10.0.0"};
    for (std::size_t i = 0; i + 1 < std::size(ordered); ++i) {
        check(compare_versions(ordered[i], ordered[i + 1]) < 0, ordered[i]);
        check(compare_versions(ordered[i + 1], ordered[i]) > 0, ordered[i + 1]);
        check(compare_versions(ordered[i], ordered[i]) == 0, ordered[i]);
    }
    for (const char* garbage : {"", "v0.4.0", "0.4", "0.4.0.1", "00.4.0", "0.04.0", "0.4.0-", "0.4.0-a..b", "0.4.0-01",
                                "0.4.0+build", "0.4.0-a_b", " 0.4.0", "0.4.x", "18446744073709551616.0.0"}) {
        bool threw = false;
        try { (void)compare_versions(garbage, "0.4.0"); } catch (const std::exception&) { threw = true; }
        check(threw, garbage);
    }
    if (!failures) std::puts("update version ordering passed");
    return failures ? 1 : 0;
}
