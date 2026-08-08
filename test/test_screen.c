/*
 * Host side test harness for the screen handling in tcp_chat.c.
 *
 * It emulates the z88dk "generic console" driver
 * (libsrc/classic/stdio/fputc_cons_generic.inc) that +zx links in:
 * 64x24, code 22/y+32/x+32 = goto, 10 = lf (scrolls on the last row),
 * 13 = cr, 8 = left, 12 = cls, wrap at the right edge and a lazy scroll
 * when a character is printed below the last row.
 *
 * tcp_chat.c is #included so we test the real code.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>

#include "ip.h"
#include "tcp.h"

#define W 64
#define H 24

static char screen[H][W];
static int cx, cy;
static int params_left, param0;

static void con_scroll(void)
{
        int y, x;

        for (y = 0; y < H - 1; y++) {
                memcpy(screen[y], screen[y + 1], W);
        }
        for (x = 0; x < W; x++) {
                screen[H - 1][x] = ' ';
        }
}

static void con_cls(void)
{
        int y, x;

        for (y = 0; y < H; y++) {
                for (x = 0; x < W; x++) {
                        screen[y][x] = ' ';
                }
        }
        cx = cy = 0;
}

static void con_printable(int c)
{
        /* lazy scroll, exactly like handle_character */
        while (cy > H - 1) {
                con_scroll();
                cy--;
        }
        if (cy > H - 1) {
                cy = H - 1;
                cx = 0;
        }
        screen[cy][cx] = (char)c;
        cx++;
        if (cx == W) {
                cx = 0;
                cy++;
        }
}

static int mock_putchar(int c)
{
        c &= 0xff;

        if (params_left) {
                params_left--;
                if (params_left) {
                        param0 = c;
                } else {
                        int y = param0 - 32;
                        int x = c - 32;

                        if (x >= 0 && x < W && y >= 0 && y < H) {
                                cx = x;
                                cy = y;
                        }
                }
                return c;
        }

        switch (c) {
        case 22:
                params_left = 2;
                return c;
        case 12:
                con_cls();
                return c;
        case 13:
                cx = 0;
                return c;
        case 8:
                if (cx) {
                        cx--;
                } else {
                        cx = W - 1;
                        if (cy) {
                                cy--;
                        }
                }
                return c;
        case 10:
                if (cy >= H - 1) {
                        con_scroll();
                        cy = H - 1;
                } else {
                        cy++;
                }
                cx = 0;
                return c;
        default:
                con_printable(c);
                return c;
        }
}

static int mock_printf(const char *fmt, ...)
{
        char buf[256];
        va_list ap;
        int i;

        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        for (i = 0; buf[i]; i++) {
                mock_putchar((unsigned char)buf[i]);
        }

        return i;
}

/* --- the bits of the environment tcp_chat.c expects ------------------ */

char in_KeyDebounce, in_KeyStartRepeat, in_KeyRepeatPeriod;
unsigned int in_GetKey(void) { return 0; }
void in_GetKeyReset(void) {}
void in_Wait(unsigned int msec) { (void)msec; }
void in_WaitForNoKey(void) {}
void zx_cls(void) {}
uint16_t htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
uint32_t htonl(uint32_t v)
{
        return ((v >> 24) & 0xff) | ((v >> 8) & 0xff00) |
               ((v << 8) & 0xff0000) | (v << 24);
}

static int sent_len;
static char sent[600];

int tcp_send(tcp_context_t *tcp, int len, uint8_t *data, int window)
{
        (void)tcp;
        (void)window;
        if (len && data) {
                memcpy(sent, data, len);
                sent_len = len;
        }
        return 0;
}
int tcp_recv_timeout(tcp_context_t *tcp, int len, int to)
{
        (void)tcp;
        (void)len;
        (void)to;
        return -EAGAIN;
}
int tcp_recv(tcp_context_t *tcp, int len) { return tcp_recv_timeout(tcp, len, 0); }
int tcp_connect(tcp_context_t *tcp) { (void)tcp; return 0; }
uint8_t *tcp_rx_buffer(tcp_context_t *tcp) { return &tcp->ip.pkt[40]; }
int slip_init(int baud, int parity) { (void)baud; (void)parity; return 0; }

#undef putchar
#undef printf
#define putchar mock_putchar
#define printf mock_printf
#define main chat_main
#include "tcp_chat.c"
#undef putchar
#undef printf
#undef main

/* --- tests ----------------------------------------------------------- */

static int failures;

static void row_is(int row, const char *expect)
{
        char got[W + 1];
        char want[W + 1];

        memcpy(got, screen[row], W);
        got[W] = 0;
        memset(want, ' ', W);
        want[W] = 0;
        memcpy(want, expect, strlen(expect));

        if (memcmp(got, want, W)) {
                printf("FAIL row %d\n  want |%s|\n  got  |%s|\n",
                       row, want, got);
                failures++;
        }
}

static void feed(const char *s)
{
        show_data((uint8_t *)s, (int)strlen(s));
}

static void type(const char *s)
{
        while (*s) {
                handle_key((unsigned char)*s++);
        }
}

static void reset(void)
{
        con_cls();
        input_len = 0;
        logline_len = 0;
        logline_emitted = 0;
        draw_input();
}

int main(void)
{
        char big[300];
        int i;

        /* 1: a couple of lines of chat and something typed */
        reset();
        feed("<user1> hello\n<user2> hi there\n");
        type("how are you");
        row_is(21, "<user1> hello");
        row_is(22, "<user2> hi there");
        row_is(23, ">how are you_");

        /* 2: log scrolls, oldest lines fall off the top */
        reset();
        for (i = 0; i < 30; i++) {
                char l[32];

                sprintf(l, "line %d\n", i);
                feed(l);
        }
        row_is(0, "line 7");
        row_is(22, "line 29");
        row_is(23, ">_");

        /* 3: a line exactly as wide as the screen must not leave a hole */
        reset();
        for (i = 0; i < W; i++) {
                big[i] = 'a' + (i % 26);
        }
        big[W] = 0;
        feed(big);
        feed("\n");
        feed("after\n");
        row_is(21, big);
        row_is(22, "after");
        row_is(23, ">_");

        /* 4: a long line wraps onto two log lines */
        reset();
        for (i = 0; i < W + 10; i++) {
                big[i] = '0' + (i % 10);
        }
        big[W + 10] = 0;
        feed(big);
        feed("\n");
        {
                char head[W + 1];

                memcpy(head, big, W);
                head[W] = 0;
                row_is(21, head);
                row_is(22, big + W);
        }
        row_is(23, ">_");

        /* 5: typing more than fits shows the tail of the line */
        reset();
        for (i = 0; i < 70; i++) {
                big[i] = 'A' + (i % 26);
        }
        big[70] = 0;
        type(big);
        {
                char want[W + 1];

                want[0] = '>';
                memcpy(want + 1, big + 70 - (W - 2), W - 2);
                want[W - 1] = '_';
                want[W] = 0;
                row_is(23, want);
        }

        /* 6: backspace */
        reset();
        type("abcd");
        handle_key(12);
        handle_key(12);
        row_is(23, ">ab_");
        for (i = 0; i < 10; i++) {
                handle_key(12);
        }
        row_is(23, ">_");

        /* 7: control characters in the stream are dropped, not printed */
        reset();
        feed("a\x16\x41\x42" "b\x07\n");
        row_is(22, "aABb");

        /* 8: sending echoes locally and puts a newline on the wire */
        reset();
        type("hello world");
        handle_key(13);
        if (sent_len != 12 || memcmp(sent, "hello world\n", 12)) {
                printf("FAIL sent |%.*s| (%d)\n", sent_len, sent, sent_len);
                failures++;
        }
        row_is(22, "hello world");
        row_is(23, ">_");

        /* 9: address parsing */
        {
                uint8_t ip[4];

                if (parse_ip("192.0.2.1", ip) || ip[0] != 192 || ip[1] != 0 ||
                    ip[2] != 2 || ip[3] != 1) {
                        printf("FAIL parse_ip valid\n");
                        failures++;
                }
                if (!parse_ip("192.0.2", ip) || !parse_ip("hello", ip) ||
                    !parse_ip("", ip)) {
                        printf("FAIL parse_ip invalid\n");
                        failures++;
                }
        }

        /* 10: a message arriving while we are typing keeps our line and
               does not leave any of it behind in the log */
        reset();
        type("i am typing a fairly long line here");
        feed("<user2> hi\n");
        row_is(22, "<user2> hi");
        row_is(23, ">i am typing a fairly long line here_");

        /* 11: an empty line from the server erases the input line too */
        reset();
        type("typing");
        feed("\n");
        row_is(22, "");
        row_is(23, ">typing_");

        printf(failures ? "\n%d FAILURES\n" : "\nall tests passed\n", failures);

        return failures != 0;
}
