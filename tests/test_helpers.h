#pragma once
// Minimal test framework — no external dependencies.
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

static int g_pass = 0, g_fail = 0;

#define CHECK(expr) \
    do { \
        if (expr) { g_pass++; } \
        else { \
            std::cerr << "FAIL  " << __FILE__ << ":" << __LINE__ << "  " << #expr << "\n"; \
            g_fail++; \
        } \
    } while (0)

#define CHECK_NEAR(a, b, tol) \
    do { \
        double _a=(a), _b=(b), _t=(tol); \
        if (std::abs(_a - _b) <= _t) { g_pass++; } \
        else { \
            std::cerr << "FAIL  " << __FILE__ << ":" << __LINE__ \
                      << "  |" << #a << " - " << #b << "| = " \
                      << std::abs(_a-_b) << " > " << _t << "\n"; \
            g_fail++; \
        } \
    } while (0)

#define CHECK_LT(a, b) CHECK((a) < (b))
#define CHECK_LE(a, b) CHECK((a) <= (b))
#define CHECK_GT(a, b) CHECK((a) > (b))
#define CHECK_GE(a, b) CHECK((a) >= (b))
#define CHECK_EQ(a, b) CHECK((a) == (b))

#define SECTION(name) std::cout << "\n--- " << name << " ---\n"

static int TEST_SUMMARY() {
    std::cout << "\nResults: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail ? 1 : 0;
}
