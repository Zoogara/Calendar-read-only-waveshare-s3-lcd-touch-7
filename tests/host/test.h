#pragma once
/* A tiny test harness: CHECK* macros count failures and keep going. */
#include <stdio.h>
#include <string.h>

extern int t_checks, t_failed;

#define CHECK(cond) do { t_checks++; if (!(cond)) { t_failed++; \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_INT(got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); t_checks++; \
    if (g_ != w_) { t_failed++; printf("  FAIL %s:%d: %s = %lld, want %lld\n", __FILE__, __LINE__, #got, g_, w_); } } while (0)
#define CHECK_STR(got, want) do { const char *g_ = (got), *w_ = (want); t_checks++; \
    if (strcmp(g_, w_) != 0) { t_failed++; printf("  FAIL %s:%d: %s = \"%s\", want \"%s\"\n", __FILE__, __LINE__, #got, g_, w_); } } while (0)
#define CHECK_NEAR(got, want, tol) do { double g_ = (got), w_ = (want); t_checks++; \
    if (g_ < w_ - (tol) || g_ > w_ + (tol)) { t_failed++; printf("  FAIL %s:%d: %s = %g, want %g +/- %g\n", __FILE__, __LINE__, #got, g_, w_, (double)(tol)); } } while (0)

#define RUN(fn) do { int before_ = t_failed; fn(); printf("%s %s\n", t_failed == before_ ? "ok  " : "FAIL", #fn); } while (0)
