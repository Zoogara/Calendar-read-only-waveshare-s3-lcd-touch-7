#include "test.h"

int t_checks, t_failed;

void test_rss(void);
void test_ics(void);
void test_wind(void);

int main(void)
{
    test_rss();
    test_ics();
    test_wind();
    printf("\n%d checks, %d failed\n", t_checks, t_failed);
    return t_failed ? 1 : 0;
}
