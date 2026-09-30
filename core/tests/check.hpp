#pragma once

#include <iostream>
#include <string>

// Minimal test helper: prints every check, counts failures, exit code is the failure count.
inline int& failures() {
    static int n = 0;
    return n;
}

inline void expect(bool ok, const std::string& what) {
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << "\n";
    failures() += !ok;
}

inline void section(const std::string& name) {
    std::cout << name << "\n";
}

inline int finish() {
    std::cout << (failures() ? std::to_string(failures()) + " FAILED\n" : "all passed\n");
    return failures() ? 1 : 0;
}
