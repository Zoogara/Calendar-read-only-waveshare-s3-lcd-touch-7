#include "test.h"

int t_checks, t_failed;

void test_rss(void);
void test_ics(void);
void test_auth(void);
void test_urldecode(void);

int main(void)
{
    test_rss();
    test_ics();
    test_auth();
    test_urldecode();
    printf("\n%d checks, %d failed\n", t_checks, t_failed);
    return t_failed ? 1 : 0;
}
