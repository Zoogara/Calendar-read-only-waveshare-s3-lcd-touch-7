#include "test.h"

int t_checks, t_failed;

void test_rss(void);
void test_ics(void);
void test_auth(void);
void test_urldecode(void);
void test_jwt(void);
void test_urlenc(void);

int main(void)
{
    test_rss();
    test_ics();
    test_auth();
    test_urldecode();
    test_jwt();
    test_urlenc();
    printf("\n%d checks, %d failed\n", t_checks, t_failed);
    return t_failed ? 1 : 0;
}
