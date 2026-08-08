/*
 * Host side test harness for irc.c: the screen handling, the parsing of
 * what the server sends and the commands we type.
 *
 * It emulates the z88dk "generic console" driver
 * (libsrc/classic/stdio/fputc_cons_generic.inc) that +zx links in:
 * 64x24, code 22/y+32/x+32 = goto, 10 = lf (scrolls on the last row),
 * 13 = cr, 8 = left, 12 = cls, wrap at the right edge and a lazy scroll
 * when a character is printed below the last row.
 *
 * irc.c is #included so we test the real code. Nothing here talks to the
 * network, tcp_send() just records what we would have put on the wire.
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

/* --- the bits of the environment irc.c expects ----------------------- */

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

/* Everything we would have put on the wire, concatenated */
static char sent[2048];
static int sent_len;

int tcp_send(tcp_context_t *tcp, int len, uint8_t *data, int window)
{
        (void)tcp;
        (void)window;
        if (len && data && sent_len + len < (int)sizeof(sent)) {
                memcpy(sent + sent_len, data, len);
                sent_len += len;
                sent[sent_len] = 0;
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
#define main irc_main
#include "irc.c"
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

static void sent_is(const char *expect)
{
        if (strcmp(sent, expect)) {
                printf("FAIL sent\n  want |%s|\n  got  |%s|\n", expect, sent);
                failures++;
        }
}

/* Hand the client some bytes as if they had arrived in one segment */
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
        rxlen = 0;
        out_len = 0;
        pong_pending = 0;
        join_pending = 0;
        sent_len = 0;
        sent[0] = 0;
        strcpy(mynick, "spec");
        cur_chan[0] = 0;
        draw_input();
}

int main(void)
{
        /* 1: a channel message and a private one */
        reset();
        feed(":bob!bob@example.com PRIVMSG #zx :hello there\r\n");
        feed(":ann!ann@example.com PRIVMSG spec :psst\r\n");
        row_is(21, "<bob> hello there");
        row_is(22, "*ann* psst");
        row_is(23, ">_");

        /* 2: a line split over two segments is still parsed */
        reset();
        feed(":bob!b@h PRIVMSG #zx :split ");
        feed("over two packets\r\n");
        row_is(22, "<bob> split over two packets");

        /* 3: PING is answered, but only once we are out of the parser */
        reset();
        feed("PING :irc.example.com\r\n");
        if (!pong_pending || sent_len != 0) {
                printf("FAIL ping was answered from inside the parser\n");
                failures++;
        }
        flush_pending();
        sent_is("PONG :irc.example.com\r\n");

        /* 4: CTCP ACTION, i.e. someone elses /me */
        reset();
        feed(":bob!b@h PRIVMSG #zx :\001ACTION waves\001\r\n");
        row_is(22, "* bob waves");

        /* 5: notices, joins, parts, quits and nick changes */
        reset();
        feed(":irc.example.com NOTICE * :*** Looking up your hostname\r\n");
        row_is(22, "-irc.example.com- *** Looking up your hostname");
        reset();
        feed(":bob!b@h JOIN #zx\r\n");
        row_is(22, "* bob joined #zx");
        reset();
        feed(":bob!b@h PART #zx :bye\r\n");
        row_is(22, "* bob left #zx");
        reset();
        feed(":bob!b@h QUIT :Ping timeout\r\n");
        row_is(22, "* bob quit: Ping timeout");
        reset();
        feed(":bob!b@h NICK bobby\r\n");
        row_is(22, "* bob is now known as bobby");

        /* 6: a numeric prints its text without our own nick in front */
        reset();
        feed(":irc.example.com 372 spec :- this is the motd\r\n");
        row_is(22, "- this is the motd");
        reset();
        feed(":irc.example.com 353 spec = #zx :spec bob ann\r\n");
        row_is(22, "= #zx spec bob ann");

        /* 7: 001 makes us join the channel we asked for at startup */
        reset();
        strcpy(cur_chan, "#zx");
        feed(":irc.example.com 001 spec :Welcome\r\n");
        flush_pending();
        sent_is("JOIN #zx\r\n");

        /* 8: typing something says it on the channel and echoes it */
        reset();
        strcpy(cur_chan, "#zx");
        type("hello everyone");
        handle_key(13);
        sent_is("PRIVMSG #zx :hello everyone\r\n");
        row_is(22, "<spec> hello everyone");
        row_is(23, ">_");

        /* 9: ... but not when we are not on a channel */
        reset();
        type("hello");
        handle_key(13);
        sent_is("");
        row_is(22, "*** you are not on a channel, try /join #chan");

        /* 10: the commands */
        reset();
        type("/join #zx");
        handle_key(13);
        sent_is("JOIN #zx\r\n");
        if (strcmp(cur_chan, "#zx")) {
                printf("FAIL /join did not set the channel: |%s|\n", cur_chan);
                failures++;
        }

        reset();
        strcpy(cur_chan, "#zx");
        type("/me waves back");
        handle_key(13);
        sent_is("PRIVMSG #zx :\001ACTION waves back\001\r\n");
        row_is(22, "* spec waves back");

        reset();
        type("/msg bob are you there");
        handle_key(13);
        sent_is("PRIVMSG bob :are you there\r\n");
        row_is(22, ">bob< are you there");

        reset();
        strcpy(cur_chan, "#zx");
        type("/part later");
        handle_key(13);
        sent_is("PART #zx :later\r\n");
        if (cur_chan[0]) {
                printf("FAIL /part did not leave the channel\n");
                failures++;
        }

        reset();
        type("/nick zx81");
        handle_key(13);
        sent_is("NICK zx81\r\n");

        reset();
        strcpy(cur_chan, "#zx");
        type("/topic a new topic");
        handle_key(13);
        sent_is("TOPIC #zx :a new topic\r\n");

        reset();
        type("/raw WHOIS bob");
        handle_key(13);
        sent_is("WHOIS bob\r\n");

        reset();
        type("/QUIT so long");
        if (handle_key(13) >= 0) {
                printf("FAIL /quit did not end the session\n");
                failures++;
        }
        sent_is("QUIT :so long\r\n");

        /* 11: a plain /names would list every channel on the server */
        reset();
        type("/names");
        handle_key(13);
        sent_is("");
        row_is(22, "*** usage: /names #channel");

        /* 12: something we do not implement is shown as it came in */
        reset();
        type("/dance");
        handle_key(13);
        sent_is("");
        row_is(22, "*** no such command, try /help: /dance");
        reset();
        feed(":irc.example.com WALLOPS :the server is going down\r\n");
        row_is(22, "WALLOPS the server is going down");

        /* 13: control characters from the network are never printed */
        reset();
        feed(":bob!b@h PRIVMSG #zx :a\026\101\102b\007\r\n");
        row_is(22, "<bob> aABb");

        /* 14: a message arriving while we are typing keeps our line */
        reset();
        strcpy(cur_chan, "#zx");
        type("i am typing a fairly long line here");
        feed(":bob!b@h PRIVMSG #zx :hi\r\n");
        row_is(22, "<bob> hi");
        row_is(23, ">i am typing a fairly long line here_");

        /* 15: a long line wraps over two log rows */
        reset();
        {
                char big[200];
                char head[W + 1];
                char tail[W + 1];
                int i;

                strcpy(big, ":bob!b@h PRIVMSG #zx :");
                for (i = 0; i < 100; i++) {
                        big[22 + i] = '0' + (i % 10);
                }
                strcpy(big + 122, "\r\n");
                feed(big);

                /* "<bob> " plus the first 58 digits fills one row, the
                   remaining 42 digits end up on the next one */
                memcpy(head, "<bob> ", 6);
                for (i = 0; i < W - 6; i++) {
                        head[6 + i] = '0' + (i % 10);
                }
                head[W] = 0;
                for (i = 0; i < 100 - (W - 6); i++) {
                        tail[i] = '0' + ((W - 6 + i) % 10);
                }
                tail[100 - (W - 6)] = 0;
                row_is(21, head);
                row_is(22, tail);
        }

        /* 16: address parsing */
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

        printf(failures ? "\n%d FAILURES\n" : "\nall tests passed\n", failures);

        return failures != 0;
}
