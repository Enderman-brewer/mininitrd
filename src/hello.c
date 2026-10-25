/*
 * hello: trivial test program, exec'd repeatedly by test_ramify.c as /hello.
 * Built statically so it runs inside the initramfs, which has no C library.
 */
#include <unistd.h>

int main(void)
{
    static const char msg[] = "hello world\n";
    (void)!write(1, msg, sizeof msg - 1);
    return 0;
}
