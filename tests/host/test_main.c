#include "test.h"

int t_checks, t_failed;

void test_rss(void);
void test_ics(void);
void test_auth(void);
void test_urldecode(void);
void test_jwt(void);
void test_urlenc(void);
void test_timefmt(void);
void test_eventtext(void);
void test_setcrypt(void);
void test_readlater(void);

int main(void)
{
    test_rss();
    test_ics();
    test_auth();
    test_urldecode();
    test_jwt();
    test_urlenc();
    test_timefmt();
    test_eventtext();
    test_setcrypt();
    test_readlater();
    printf("\n%d checks, %d failed\n", t_checks, t_failed);
    return t_failed ? 1 : 0;
}
