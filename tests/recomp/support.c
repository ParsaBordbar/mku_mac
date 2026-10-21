/* Freestanding helpers the compiler may call; linked into every MIPS test
 * (the host build uses the system libc instead). */
typedef unsigned long size_t;

void _start(void) {}   /* the linker wants an entry point; tests never use it */

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int v, size_t n) {
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)v;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) while (n--) *d++ = *s++;
    else while (n--) d[n] = s[n];
    return dst;
}
