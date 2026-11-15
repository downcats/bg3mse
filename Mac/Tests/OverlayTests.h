#pragma once

#include <cstdio>

// What the two files of the overlay tests share (OverlayTests.cpp and
// OverlayTestsObjc.mm, which is Objective-C++).

extern int gFailures;
extern char const* gTest;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", gTest, #cond, __LINE__); gFailures++; } } while (0)
#define TEST(name) static void name(); static void name##_run() { gTest = #name; name(); } static void name()

// The tests that need Objective-C objects; on the main thread.
void RunObjcTests();
