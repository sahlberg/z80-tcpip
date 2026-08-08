/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/*
 * Copyright 2026 Ronnie Sahlberg <ronniesahlberg@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the “Software”), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * A tiny chat client, meant to be used against a
 *      ncat --chat -l <port>
 * (or any other line oriented tcp service) on the other side of slip-tun.
 *
 * The bottom line of the screen is the line we type on, the 23 lines
 * above it are the chat log.
 *
 * There is no OS here, no threads and no usable interrupts, so typing and
 * receiving are interleaved by hand in one single polling loop:
 *
 * - The keyboard is read with in_GetKey() which scans the keyboard ports
 *   directly. We can not use the ROMs LAST-K system variable (which is
 *   what scanf()/fgets_cons() use) because that one is only updated from
 *   the 50Hz interrupt routine, and rs232_get() runs with interrupts
 *   disabled for almost the entire time we are connected.
 *   in_GetKey() implements debounce and auto repeat in terms of "number
 *   of calls", so the values below are tuned for the ~40 calls/second
 *   that the loop ends up doing.
 *
 * - The connection is polled with tcp_recv_timeout() using a very short
 *   timeout (a couple of rs232_get() timeouts, ~25ms) instead of the ~1s
 *   that plain tcp_recv() waits, so that we get back to the keyboard
 *   often enough to not miss keypresses.
 *
 * The Interface 1 serial port has no receive buffer at all: every byte
 * that arrives while we are not sitting inside rs232_get() is lost.
 * That is why the loop spends nearly all of its time in the network poll
 * and does as little work as possible in between. A packet that is lost
 * this way is simply retransmitted by the peer.
 *
 * Unlike the rest of the stack, which keeps the window closed and only
 * opens it for the duration of one tcp_recv(), a chat client has to be
 * ready to receive at any time, so we keep a window open while we are
 * idle and re-open it every time the stack has closed it again (which it
 * does whenever it ACKs data or when we send something).
 */

#include <arch/zx.h>
#include <errno.h>
#include <input.h>
#include <net/hton.h>
#include <stdio.h>
#include <rs232.h>

#include "slip.h"
#include "ip.h"
#include "tcp.h"

#define BUF_SIZE (20 + 20 + 512)

/*
 * z88dk defaults the +zx console to the 64 column driver (the 4x8 font).
 * Building with "#pragma output CLIB_ZX_CONIO32 = 1" gives the normal
 * 32 column screen instead, in which case this has to say 32.
 */
#define SCREEN_COLS     64
#define SCREEN_ROWS     24
#define INPUT_ROW       (SCREEN_ROWS - 1)
/* '>' prompt + the text we are typing + the cursor == SCREEN_COLS */
#define INPUT_VIEW      (SCREEN_COLS - 2)
#define MAX_INPUT       80

/*
 * How long one poll of the connection waits for a packet, counted in
 * rs232_get() timeouts (~12ms each on Interface 1). recv_packet() does
 * to-1 reads so this is roughly 25ms.
 */
#define POLL_TICKS      3

/* The window we advertise while we sit idle waiting for someone to talk */
#define RX_WINDOW       256

/* Flag in the tcp header, we look at it to notice the server going away */
#define TCP_FIN         0x01

/* Keys, as returned by in_GetKey() */
#define KEY_LEFT        8       /* CAPS SHIFT + 5 */
#define KEY_ENTER       13
#define KEY_DELETE      12      /* CAPS SHIFT + 0 */
#define KEY_CTRL_DEL    127     /* CAPS SHIFT + SYM SHIFT + 0 */
#define KEY_QUIT        3       /* CAPS SHIFT + SYM SHIFT + C */

uint8_t tcp_buf[BUF_SIZE];
tcp_context_t tcp = {
        .ip = {
                .pkt_size = BUF_SIZE,
                .pkt = tcp_buf,
        },
};

/* The line we are currently typing */
static char input[MAX_INPUT + 2];
static int input_len;

/* The chat log line we are currently assembling from received data */
static char logline[SCREEN_COLS + 1];
static int logline_len;
/* Have we already written out part of the line we are assembling? */
static int logline_emitted;

/* Do we currently have a window open towards the peer? */
static int window_open;


static void puts_raw(char *s)
{
        while (*s) {
                putchar(*s++);
        }
}

/*
 * Move the print position. The console driver wants row and column
 * offset by 32, i.e. the same encoding as PRINT AT in basic.
 */
static void at(int row, int col)
{
        putchar(22);
        putchar(row + 32);
        putchar(col + 32);
}

/*
 * Redraw the line we are typing on.
 * If we have typed more than fits we show the tail of it, since that is
 * where the cursor is. The line is always padded out to the full width
 * so that this also erases whatever was there before.
 */
static void draw_input(void)
{
        char row[SCREEN_COLS + 1];
        int i, n, start;

        n = input_len;
        start = 0;
        if (n > INPUT_VIEW) {
                start = n - INPUT_VIEW;
                n = INPUT_VIEW;
        }

        row[0] = '>';
        for (i = 0; i < n; i++) {
                row[1 + i] = input[start + i];
        }
        row[1 + n] = '_';
        for (i = n + 2; i < SCREEN_COLS; i++) {
                row[i] = ' ';
        }
        row[SCREEN_COLS] = 0;

        at(INPUT_ROW, 0);
        puts_raw(row);
        /*
         * Printing the last column left the print position on the
         * (non existing) row below, so put it back to a known place.
         * Everything else prints an explicit at() first anyway.
         */
        at(INPUT_ROW, 0);
}

/*
 * Add one line to the chat log.
 *
 * The console driver scrolls the whole screen when a newline is printed
 * while on the last row, and it has no notion of a scrolling region, so
 * the trick is to print the line on the input row and immediately follow
 * it with a newline. That lifts the text we just printed up into the log
 * and leaves the bottom row empty for the input line again.
 *
 * Lines longer than the screen width wrap and scroll on their own.
 */
static void log_line(char *s)
{
        int n = 0;

        at(INPUT_ROW, 0);
        while (*s) {
                putchar(*s++);
                n++;
        }
        /*
         * Pad out to a whole number of rows. The input line is sitting
         * on this row so we have to erase the tail of it, and by always
         * printing a multiple of the screen width we end up at column 0
         * of the row below, which is where the newline below wants us
         * to be for it to scroll exactly once.
         */
        while (n == 0 || (n % SCREEN_COLS)) {
                putchar(' ');
                n++;
        }
        putchar('\n');
        draw_input();
}

static void logline_flush(void)
{
        logline[logline_len] = 0;
        logline_len = 0;
        logline_emitted = 1;
        log_line(logline);
}

/*
 * Feed one received character to the chat log.
 * We only ever print plain printable characters. Everything else is
 * dropped, both because there is nothing sensible to draw for it and
 * because the console driver would eat the following character(s) as
 * arguments to a control code.
 */
static void logline_char(char c)
{
        if (c == '\n') {
                /*
                 * Only start a new row if there is something left to
                 * show, or if nothing at all has been shown for this
                 * line yet (an empty line in the chat). A line that
                 * filled the screen width, or that we flushed at the
                 * end of a segment, has already been written out.
                 */
                if (logline_len || !logline_emitted) {
                        logline_flush();
                }
                logline_emitted = 0;
                return;
        }
        if (c == '\t') {
                c = ' ';
        }
        if (c < 32 || c > 126) {
                return;
        }

        logline[logline_len++] = c;
        if (logline_len == SCREEN_COLS) {
                logline_flush();
        }
}

static void show_data(uint8_t *data, int len)
{
        while (len--) {
                logline_char((char)*data++);
        }
        /*
         * Chat is line based so anything left over here is a line that
         * did not end with a newline. Show it now instead of leaving it
         * invisible until the peer happens to send more.
         */
        if (logline_len) {
                logline_flush();
        }
}

/*
 * Poll the connection once.
 * Returns 0 if we are still connected, <0 if the session is gone.
 */
static int poll_net(void)
{
        int rc;

        if (!window_open) {
                /*
                 * A pure ACK with a window. tcp_send() does not wait for
                 * anything when there is no payload so this is cheap.
                 */
                tcp_send(&tcp, 0, NULL, RX_WINDOW);
                window_open = 1;
        }

        rc = tcp_recv_timeout(&tcp, 0, POLL_TICKS);
        if (rc == -EAGAIN) {
                return 0;
        }
        if (rc == -ERESET) {
                log_line("*** connection reset by peer");
                return -1;
        }
        if (rc < 0) {
                return 0;
        }

        if (rc > 0) {
                /*
                 * The stack ACKed this segment with a zero window, so we
                 * have to open the window again before the peer will
                 * send us anything more.
                 */
                window_open = 0;
                show_data(tcp_rx_buffer(&tcp), rc);
                return 0;
        }

        /*
         * A packet for our session that carried no data. The stack has
         * no concept of a FIN so peek in the buffer and see if the peer
         * just hung up on us.
         *
         * This is only valid for a segment without data since the stack
         * builds its ACK for a data segment on top of the very buffer we
         * are looking at. A FIN that came with data is picked up when the
         * peer retransmits it.
         */
        if (tcp_buf[20 + 13] & TCP_FIN) {
                log_line("*** server closed the connection");
                return -1;
        }

        return 0;
}

/*
 * Send the line we have typed.
 *
 * tcp_send() waits for the ACK, and while it does that it will happily
 * swallow a data segment that the peer sent us at the same time. To make
 * that less likely we first close the window and drain whatever was
 * already on its way to us.
 */
static void send_input(void)
{
        int i, rc;

        if (input_len == 0) {
                return;
        }

        tcp_send(&tcp, 0, NULL, 0);
        window_open = 0;
        for (i = 0; i < 3; i++) {
                rc = tcp_recv_timeout(&tcp, 0, POLL_TICKS);
                if (rc > 0) {
                        show_data(tcp_rx_buffer(&tcp), rc);
                }
        }

        input[input_len] = '\n';
        rc = tcp_send(&tcp, input_len + 1, (uint8_t *)input, 0);
        window_open = 0;

        /*
         * A chat server only relays what we say to the *other* clients,
         * so echo it locally.
         */
        input[input_len] = 0;
        input_len = 0;
        log_line(input);
        if (rc < 0) {
                log_line("*** send failed");
        }
}

/* Returns 0 to keep going, <0 when the user wants out */
static int handle_key(unsigned int k)
{
        switch (k) {
        case KEY_ENTER:
                send_input();
                return 0;
        case KEY_LEFT:
        case KEY_DELETE:
        case KEY_CTRL_DEL:
                if (input_len) {
                        input_len--;
                        draw_input();
                }
                return 0;
        case KEY_QUIT:
                return -1;
        default:
                if (k >= 32 && k < 127 && input_len < MAX_INPUT) {
                        input[input_len++] = k;
                        draw_input();
                }
                return 0;
        }
}

/*
 * Read a line of text from the keyboard, echoing it as it is typed.
 * Used for the questions we ask before connecting. This is in_GetKey()
 * based just like the chat loop, so that the keyboard behaves the same
 * way in both places.
 */
static void read_line(char *buf, int max)
{
        unsigned int k;
        int len = 0;

        for (;;) {
                /*
                 * in_GetKey() counts debounce and repeat in calls, so the
                 * loop has to be paced. In the chat loop the network poll
                 * is what paces it, here there is nothing else to do.
                 */
                in_Wait(20);
                k = in_GetKey();
                if (k == 0) {
                        continue;
                }
                if (k == KEY_ENTER) {
                        break;
                }
                if (k == KEY_LEFT || k == KEY_DELETE || k == KEY_CTRL_DEL) {
                        if (len) {
                                len--;
                                putchar(8);
                                putchar(' ');
                                putchar(8);
                        }
                        continue;
                }
                if (k >= 32 && k < 127 && len < max - 1) {
                        buf[len++] = k;
                        putchar(k);
                }
        }
        buf[len] = 0;
        putchar('\n');

        /*
         * Do not let the ENTER that ended this line auto repeat into
         * whatever we ask next.
         */
        in_WaitForNoKey();
        in_GetKeyReset();
}

static unsigned int parse_number(char **pp)
{
        char *p = *pp;
        unsigned int val = 0;

        while (*p >= '0' && *p <= '9') {
                val = val * 10 + (*p - '0');
                p++;
        }
        *pp = p;

        return val;
}

/* Parse a.b.c.d into the four bytes at ip. Returns 0 on success. */
static int parse_ip(char *p, uint8_t *ip)
{
        int i;

        for (i = 0; i < 4; i++) {
                if (*p < '0' || *p > '9') {
                        return -1;
                }
                ip[i] = parse_number(&p);
                if (i < 3) {
                        if (*p != '.') {
                                return -1;
                        }
                        p++;
                }
        }

        return 0;
}

int main(void)
{
        char line[MAX_INPUT + 2];
        unsigned int k, port;
        int rc;
        uint8_t *ip;

        /*
         * in_GetKey() counts debounce and repeat in number of calls, and
         * both loops below call it roughly 40 times per second.
         */
        in_KeyDebounce = 1;
        in_KeyStartRepeat = 20;
        in_KeyRepeatPeriod = 5;
        in_GetKeyReset();

        putchar(12);            /* clear screen, cursor home */
        printf("Z80 CHAT\n\n");

        tcp.ip.saddr = 0x020200c0; /* 192.0.2.2 */
        ip = (uint8_t *)&tcp.ip.saddr;
        printf("My IP address: %d.%d.%d.%d\n\n", ip[0], ip[1], ip[2], ip[3]);

        ip = (uint8_t *)&tcp.ip.daddr;
        for (;;) {
                printf("Chat server IP: ");
                fflush(stdout);
                read_line(line, sizeof(line));
                if (parse_ip(line, ip) == 0) {
                        break;
                }
                printf("Not an IP address.\n");
        }

        port = 0;
        while (port == 0) {
                printf("Chat server port: ");
                fflush(stdout);
                read_line(line, sizeof(line));
                if (line[0] >= '0' && line[0] <= '9') {
                        char *p = line;

                        port = parse_number(&p);
                }
        }

        tcp.src_port = htons(32768);
        tcp.dst_port = htons(port);

        printf("\nConnecting ...\n");
        slip_init(RS_BAUD_19200, RS_PAR_NONE);
        rc = tcp_connect(&tcp);
        if (rc < 0) {
                printf("Failed to connect: %d\n", rc);
                return 1;
        }

        putchar(12);
        input_len = 0;
        logline_len = 0;
        logline_emitted = 0;
        window_open = 0;
        log_line("*** connected. CAPS+SYM+C quits.");

        for (;;) {
                k = in_GetKey();
                if (k) {
                        if (handle_key(k) < 0) {
                                break;
                        }
                }

                if (poll_net() < 0) {
                        break;
                }
        }

        /* Leave the screen in a sane state for whatever comes next */
        at(INPUT_ROW, 0);
        putchar('\n');

        return 0;
}
