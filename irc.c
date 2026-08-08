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
 * A very small IRC client, built the same way as tcp_chat.c: the bottom
 * line of the screen is what we type on, the 23 lines above it are the
 * log, and one single polling loop interleaves in_GetKey() with a short
 * tcp_recv_timeout() because there is no OS, no threads and no usable
 * interrupts here. See the comment at the top of tcp_chat.c for why the
 * keyboard and the network have to be handled that way.
 *
 * Only a tiny subset of RFC1459 is implemented, see README.irc:
 *
 * - Registration is NICK + USER, nothing else. No PASS, no SASL, no
 *   capability negotiation, no TLS (so plaintext port 6667 only).
 * - We answer PING, which is the one thing a client really must do or
 *   the server will drop it after a minute or two.
 * - PRIVMSG/NOTICE/JOIN/PART/QUIT/NICK/KICK/TOPIC/MODE are formatted
 *   into something readable, numeric replies are printed as plain text,
 *   and anything else is dumped raw.
 * - There is no name resolution anywhere in this stack, so the server is
 *   given as a naked IP address.
 *
 * One thing to be careful about: the stack has a single packet buffer
 * that is used both for receiving and for sending. tcp_rx_buffer() points
 * straight into it, so we must not transmit anything while we are still
 * walking over received data. Everything the *server* makes us send (the
 * PONG, and the JOIN we do once we are registered) is therefore only
 * recorded as pending while we parse, and actually sent afterwards from
 * flush_pending().
 */

#include <arch/zx.h>
#include <errno.h>
#include <input.h>
#include <net/hton.h>
#include <stdio.h>
#include <string.h>
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
#define MAX_INPUT       100

/* Longest line we accept from the server. Real IRC lines are at most
 * 512 bytes but that is eight screen rows of text, so we cut them short.
 */
#define RX_LINE         320
/* The line we are building to send. IRC limit is 512 including CRLF. */
#define OUT_LINE        256

#define MAX_NICK        16
#define MAX_CHAN        32

/*
 * How long one poll of the connection waits for a packet, counted in
 * rs232_get() timeouts (~12ms each on Interface 1). recv_packet() does
 * to-1 reads so this is roughly 25ms.
 */
#define POLL_TICKS      3

/* The window we advertise while we sit idle waiting for the server */
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

/* The line we are currently assembling from what the server sent */
static char rxline[RX_LINE];
static int rxlen;

/* The line we are building to send to the server */
static char out[OUT_LINE];
static int out_len;

/* Who we are and where we are talking */
static char mynick[MAX_NICK + 1];
static char cur_chan[MAX_CHAN + 1];

/* Sends that the server asked for while we were parsing its data */
static char pong_arg[64];
static int pong_pending;
static int join_pending;

/* Do we currently have a window open towards the peer? */
static int window_open;


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
        for (i = 0; i < SCREEN_COLS; i++) {
                putchar(row[i]);
        }
        /*
         * Printing the last column left the print position on the
         * (non existing) row below, so put it back to a known place.
         * Everything else prints an explicit at() first anyway.
         */
        at(INPUT_ROW, 0);
}

/*
 * Adding a line to the log.
 *
 * The console driver scrolls the whole screen when a newline is printed
 * while on the last row, and it has no notion of a scrolling region, so
 * the trick is to print the line on the input row and immediately follow
 * it with a newline. That lifts the text we just printed up into the log
 * and leaves the bottom row empty for the input line again. Lines longer
 * than the screen width wrap and scroll on their own.
 *
 * Since a log line is usually built out of several pieces (a nick and
 * what they said, ...) it is emitted piece by piece instead of being
 * assembled in yet another buffer.
 */
static int emit_n;

static void emit_start(void)
{
        at(INPUT_ROW, 0);
        emit_n = 0;
}

/*
 * Only plain printable characters are ever printed. Everything else is
 * dropped, both because there is nothing sensible to draw for it and
 * because the console driver would eat the following character(s) as
 * arguments to a control code. This is also what removes the \001 that
 * wraps a CTCP message.
 */
static void emit(char *s)
{
        char c;

        while ((c = *s++) != 0) {
                if (c == '\t') {
                        c = ' ';
                }
                if (c < 32 || c > 126) {
                        continue;
                }
                putchar(c);
                emit_n++;
        }
}

static void emit_end(void)
{
        /*
         * Pad out to a whole number of rows. The input line is sitting
         * on this row so we have to erase the tail of it, and by always
         * printing a multiple of the screen width we end up at column 0
         * of the row below, which is where the newline below wants us
         * to be for it to scroll exactly once.
         */
        while (emit_n == 0 || (emit_n % SCREEN_COLS)) {
                putchar(' ');
                emit_n++;
        }
        putchar('\n');
        draw_input();
}

static void log_line(char *s)
{
        emit_start();
        emit(s);
        emit_end();
}

static void log_line2(char *a, char *b)
{
        emit_start();
        emit(a);
        emit(b);
        emit_end();
}

static void log_line4(char *a, char *b, char *c, char *d)
{
        emit_start();
        emit(a);
        emit(b);
        emit(c);
        emit(d);
        emit_end();
}

/* --- little string helpers ------------------------------------------- */

static void str_copy(char *dst, char *src, int max)
{
        int i = 0;

        while (src[i] && i < max - 1) {
                dst[i] = src[i];
                i++;
        }
        dst[i] = 0;
}

/* Case insensitive compare, used for the commands the user types */
static int same(char *a, char *b)
{
        char ca, cb;

        for (;;) {
                ca = *a++;
                cb = *b++;
                if (ca >= 'A' && ca <= 'Z') {
                        ca += 32;
                }
                if (cb >= 'A' && cb <= 'Z') {
                        cb += 32;
                }
                if (ca != cb) {
                        return 0;
                }
                if (ca == 0) {
                        return 1;
                }
        }
}

/*
 * Next space separated word. The word is NUL terminated in place and
 * *pp is left pointing at whatever follows it.
 */
static char *next_word(char **pp)
{
        char *p = *pp;
        char *start;

        while (*p == ' ') {
                p++;
        }
        start = p;
        while (*p && *p != ' ') {
                p++;
        }
        if (*p) {
                *p++ = 0;
        }
        *pp = p;

        return start;
}

/*
 * Next IRC parameter. Same as next_word() except that a parameter that
 * starts with ':' is the last one and runs to the end of the line.
 */
static char *next_param(char **pp)
{
        char *p = *pp;

        while (*p == ' ') {
                p++;
        }
        if (*p == ':') {
                p++;
                *pp = p + strlen(p);
                return p;
        }
        *pp = p;

        return next_word(pp);
}

/* "nick!user@host" -> "nick", in place */
static char *nick_of(char *prefix)
{
        char *p = prefix;

        if (prefix == NULL) {
                return "?";
        }
        while (*p) {
                if (*p == '!' || *p == '@') {
                        *p = 0;
                        break;
                }
                p++;
        }

        return prefix;
}

static int is_channel(char *s)
{
        return s[0] == '#' || s[0] == '&';
}

/* --- sending ---------------------------------------------------------- */

static void show_data(uint8_t *data, int len);

static void o_str(char *s)
{
        while (*s && out_len < OUT_LINE - 2) {
                out[out_len++] = *s++;
        }
}

/*
 * Send whatever has been built up in out[], terminated with CRLF.
 *
 * tcp_send() waits for the ACK, and while it does that it will happily
 * swallow a data segment that the server sent us at the same time. To
 * make that less likely we first close the window and drain whatever was
 * already on its way to us.
 */
static void o_send(void)
{
        int i, rc;

        tcp_send(&tcp, 0, NULL, 0);
        window_open = 0;
        for (i = 0; i < 3; i++) {
                rc = tcp_recv_timeout(&tcp, 0, POLL_TICKS);
                if (rc > 0) {
                        show_data(tcp_rx_buffer(&tcp), rc);
                }
        }

        out[out_len++] = '\r';
        out[out_len++] = '\n';
        rc = tcp_send(&tcp, out_len, (uint8_t *)out, 0);
        window_open = 0;
        out_len = 0;
        if (rc < 0) {
                log_line("*** send failed");
        }
}

/*
 * Send the PONG and the JOIN that the server asked for while we were
 * busy parsing what it sent us. The flags are cleared before we send
 * anything since o_send() drains the connection and may well set them
 * again, and that new request has to survive.
 */
static void flush_pending(void)
{
        if (pong_pending) {
                pong_pending = 0;
                o_str("PONG :");
                o_str(pong_arg);
                o_send();
        }
        if (join_pending) {
                join_pending = 0;
                if (cur_chan[0]) {
                        o_str("JOIN ");
                        o_str(cur_chan);
                        o_send();
                }
        }
}

/* --- what the server sends us ----------------------------------------- */

static int is_numeric(char *cmd)
{
        int i;

        for (i = 0; i < 3; i++) {
                if (cmd[i] < '0' || cmd[i] > '9') {
                        return 0;
                }
        }

        return cmd[3] == 0;
}

/* Print the parameters of a message, one space between each of them */
static void emit_params(char *p)
{
        while (*p) {
                emit(next_param(&p));
                emit(" ");
        }
}

/*
 * A numeric reply. The first parameter is always our own nick, which is
 * of no interest, so it is skipped and the rest is printed as text. That
 * is good enough for the MOTD, the name list, the topic and for all the
 * error replies.
 */
static void show_numeric(char *cmd, char *p)
{
        next_param(&p);         /* our nick */

        emit_start();
        emit_params(p);
        emit_end();

        /* The one error that is worth a hint of its own */
        if (!strcmp(cmd, "433")) {
                log_line("*** nick already in use, pick another with /nick");
        }
}

static void handle_server_line(char *line)
{
        char *prefix = NULL;
        char *cmd, *p, *nick, *target, *text;

        p = line;
        if (*p == ':') {
                p++;
                prefix = next_word(&p);
        }
        cmd = next_word(&p);
        if (*cmd == 0) {
                return;
        }

        if (!strcmp(cmd, "PING")) {
                str_copy(pong_arg, next_param(&p), sizeof(pong_arg));
                pong_pending = 1;
                return;
        }

        nick = nick_of(prefix);

        if (!strcmp(cmd, "PRIVMSG") || !strcmp(cmd, "NOTICE")) {
                target = next_param(&p);
                text = next_param(&p);

                /* CTCP ACTION, i.e. what /me produces */
                if (text[0] == 1 && !strncmp(text + 1, "ACTION ", 7)) {
                        log_line4("* ", nick, " ", text + 8);
                        return;
                }
                if (!strcmp(cmd, "NOTICE")) {
                        log_line4("-", nick, "- ", text);
                        return;
                }
                if (is_channel(target)) {
                        log_line4("<", nick, "> ", text);
                } else {
                        /* Someone talking to us and not to a channel */
                        log_line4("*", nick, "* ", text);
                }
                return;
        }
        if (!strcmp(cmd, "JOIN")) {
                log_line4("* ", nick, " joined ", next_param(&p));
                return;
        }
        if (!strcmp(cmd, "PART")) {
                log_line4("* ", nick, " left ", next_param(&p));
                return;
        }
        if (!strcmp(cmd, "QUIT")) {
                log_line4("* ", nick, " quit: ", next_param(&p));
                return;
        }
        if (!strcmp(cmd, "NICK")) {
                text = next_param(&p);
                if (same(nick, mynick)) {
                        str_copy(mynick, text, sizeof(mynick));
                }
                log_line4("* ", nick, " is now known as ", text);
                return;
        }
        if (!strcmp(cmd, "KICK")) {
                target = next_param(&p);
                log_line4("* ", nick, " kicked out of ", target);
                return;
        }
        if (!strcmp(cmd, "TOPIC")) {
                next_param(&p);         /* the channel */
                log_line4("* ", nick, " set the topic: ", next_param(&p));
                return;
        }
        if (!strcmp(cmd, "MODE")) {
                log_line4("* ", nick, " sets mode ", p);
                return;
        }
        if (!strcmp(cmd, "ERROR")) {
                log_line2("*** ", p);
                return;
        }
        if (is_numeric(cmd)) {
                /* 001 is the welcome, we are registered and may join */
                if (!strcmp(cmd, "001") && cur_chan[0]) {
                        join_pending = 1;
                }
                show_numeric(cmd, p);
                return;
        }

        /* Something we do not know about. Show it as it came in. */
        emit_start();
        emit(cmd);
        emit(" ");
        emit_params(p);
        emit_end();
}

/*
 * Feed one received character to the line we are assembling. IRC is line
 * based and a line can be split over several segments, so nothing is
 * looked at until the terminating newline has arrived. Lines longer than
 * our buffer simply lose their tail.
 */
static void rx_char(char c)
{
        if (c == '\r') {
                return;
        }
        if (c == '\n') {
                rxline[rxlen] = 0;
                rxlen = 0;
                handle_server_line(rxline);
                return;
        }
        if (rxlen < RX_LINE - 1) {
                rxline[rxlen++] = c;
        }
}

static void show_data(uint8_t *data, int len)
{
        while (len--) {
                rx_char((char)*data++);
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
                 * have to open the window again before the server will
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

/* --- what we type ------------------------------------------------------ */

static void clear_input(void)
{
        input_len = 0;
        draw_input();
}

static void help(void)
{
        log_line("*** /join #chan       /part [reason]     /nick name");
        log_line("*** /msg nick text    /me action         /names");
        log_line("*** /topic [text]     /raw irc command   /quit [reason]");
        log_line("*** anything else is said on the current channel");
}

/*
 * Act on the line that was just typed.
 * Returns 0 to keep going, <0 when we are done with the server.
 *
 * Note the order in the sending branches: the input line is cleared
 * before o_send() because o_send() takes a while and redraws the screen
 * while it drains the connection, and the text is echoed afterwards so
 * that it ends up below anything that arrived in the meantime. The text
 * is still there to echo since only input_len was reset.
 */
static int handle_input_line(void)
{
        char *p, *cmd, *arg;

        if (input_len == 0) {
                return 0;
        }
        input[input_len] = 0;

        /* Not a command, so it is something we say on the channel */
        if (input[0] != '/') {
                if (!cur_chan[0]) {
                        log_line("*** you are not on a channel, try /join #chan");
                        clear_input();
                        return 0;
                }
                o_str("PRIVMSG ");
                o_str(cur_chan);
                o_str(" :");
                o_str(input);
                p = input;
                clear_input();
                o_send();
                log_line4("<", mynick, "> ", p);
                return 0;
        }

        p = input + 1;
        cmd = next_word(&p);
        arg = p;
        while (*arg == ' ') {
                arg++;
        }

        if (same(cmd, "help") || same(cmd, "h")) {
                clear_input();
                help();
                return 0;
        }
        if (same(cmd, "join") || same(cmd, "j")) {
                if (!is_channel(arg)) {
                        clear_input();
                        log_line("*** usage: /join #channel");
                        return 0;
                }
                str_copy(cur_chan, next_word(&arg), sizeof(cur_chan));
                o_str("JOIN ");
                o_str(cur_chan);
                clear_input();
                o_send();
                return 0;
        }
        if (same(cmd, "part")) {
                if (!cur_chan[0]) {
                        clear_input();
                        log_line("*** you are not on a channel");
                        return 0;
                }
                o_str("PART ");
                o_str(cur_chan);
                if (*arg) {
                        o_str(" :");
                        o_str(arg);
                }
                cur_chan[0] = 0;
                clear_input();
                o_send();
                return 0;
        }
        if (same(cmd, "nick")) {
                if (*arg == 0) {
                        clear_input();
                        log_line("*** usage: /nick newname");
                        return 0;
                }
                o_str("NICK ");
                o_str(arg);
                clear_input();
                o_send();
                return 0;
        }
        if (same(cmd, "msg") || same(cmd, "m")) {
                char *to = next_word(&arg);

                if (*to == 0 || *arg == 0) {
                        clear_input();
                        log_line("*** usage: /msg nick text");
                        return 0;
                }
                o_str("PRIVMSG ");
                o_str(to);
                o_str(" :");
                o_str(arg);
                clear_input();
                o_send();
                log_line4(">", to, "< ", arg);
                return 0;
        }
        if (same(cmd, "me")) {
                if (!cur_chan[0] || *arg == 0) {
                        clear_input();
                        log_line("*** usage: /me does something, on a channel");
                        return 0;
                }
                o_str("PRIVMSG ");
                o_str(cur_chan);
                o_str(" :\001ACTION ");
                o_str(arg);
                o_str("\001");
                clear_input();
                o_send();
                log_line4("* ", mynick, " ", arg);
                return 0;
        }
        if (same(cmd, "names")) {
                /* A plain NAMES lists every channel on the server, which
                 * is not something you want to do to a spectrum.
                 */
                if (*arg == 0 && cur_chan[0] == 0) {
                        clear_input();
                        log_line("*** usage: /names #channel");
                        return 0;
                }
                o_str("NAMES ");
                o_str(*arg ? arg : cur_chan);
                clear_input();
                o_send();
                return 0;
        }
        if (same(cmd, "topic")) {
                if (!cur_chan[0]) {
                        clear_input();
                        log_line("*** you are not on a channel");
                        return 0;
                }
                o_str("TOPIC ");
                o_str(cur_chan);
                if (*arg) {
                        o_str(" :");
                        o_str(arg);
                }
                clear_input();
                o_send();
                return 0;
        }
        if (same(cmd, "raw") || same(cmd, "quote")) {
                if (*arg == 0) {
                        clear_input();
                        log_line("*** usage: /raw WHOIS someone");
                        return 0;
                }
                o_str(arg);
                clear_input();
                o_send();
                return 0;
        }
        if (same(cmd, "quit")) {
                o_str("QUIT :");
                o_str(*arg ? arg : "spectrum signing off");
                clear_input();
                o_send();
                return -1;
        }

        clear_input();
        log_line2("*** no such command, try /help: /", cmd);

        return 0;
}

/* Returns 0 to keep going, <0 when we are done */
static int handle_key(unsigned int k)
{
        switch (k) {
        case KEY_ENTER:
                return handle_input_line();
        case KEY_LEFT:
        case KEY_DELETE:
        case KEY_CTRL_DEL:
                if (input_len) {
                        input_len--;
                        draw_input();
                }
                return 0;
        case KEY_QUIT:
                o_str("QUIT :spectrum signing off");
                o_send();
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
 * based just like the main loop, so that the keyboard behaves the same
 * way in both places.
 */
static void read_line(char *buf, int max)
{
        unsigned int k;
        int len = 0;

        for (;;) {
                /*
                 * in_GetKey() counts debounce and repeat in calls, so the
                 * loop has to be paced. In the main loop the network poll
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
        char *p;

        /*
         * in_GetKey() counts debounce and repeat in number of calls, and
         * both loops below call it roughly 40 times per second.
         */
        in_KeyDebounce = 1;
        in_KeyStartRepeat = 20;
        in_KeyRepeatPeriod = 5;
        in_GetKeyReset();

        putchar(12);            /* clear screen, cursor home */
        printf("Z80 IRC\n\n");

        tcp.ip.saddr = 0x020200c0; /* 192.0.2.2 */
        ip = (uint8_t *)&tcp.ip.saddr;
        printf("My IP address: %d.%d.%d.%d\n\n", ip[0], ip[1], ip[2], ip[3]);

        ip = (uint8_t *)&tcp.ip.daddr;
        for (;;) {
                /* There is no name resolution in this stack at all */
                printf("IRC server IP: ");
                fflush(stdout);
                read_line(line, sizeof(line));
                if (parse_ip(line, ip) == 0) {
                        break;
                }
                printf("Not an IP address.\n");
        }

        printf("Port [6667]: ");
        fflush(stdout);
        read_line(line, sizeof(line));
        p = line;
        port = parse_number(&p);
        if (port == 0) {
                port = 6667;
        }

        while (mynick[0] == 0) {
                printf("Nick: ");
                fflush(stdout);
                read_line(line, sizeof(line));
                p = line;
                str_copy(mynick, next_word(&p), sizeof(mynick));
        }

        printf("Channel (empty for none): ");
        fflush(stdout);
        read_line(line, sizeof(line));
        p = line;
        str_copy(cur_chan, next_word(&p), sizeof(cur_chan));
        if (cur_chan[0] && !is_channel(cur_chan)) {
                printf("Channels start with #, ignoring it.\n");
                cur_chan[0] = 0;
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
        rxlen = 0;
        out_len = 0;
        pong_pending = 0;
        join_pending = 0;
        window_open = 0;
        log_line("*** connected. /help for commands, CAPS+SYM+C quits.");

        /*
         * Register. Both lines go in one segment since every segment we
         * send costs us a round trip. The USER parameters are the user
         * name, two fields that nobody uses any more and the real name.
         */
        o_str("NICK ");
        o_str(mynick);
        o_str("\r\nUSER ");
        o_str(mynick);
        o_str(" 0 * :");
        o_str(mynick);
        o_send();

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

                /* Anything the server asked us to send while we parsed */
                flush_pending();
        }

        /* Leave the screen in a sane state for whatever comes next */
        at(INPUT_ROW, 0);
        putchar('\n');

        return 0;
}
