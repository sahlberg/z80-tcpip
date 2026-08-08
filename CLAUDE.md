# z80-tcpip

A tiny TCP/IP stack for Z80 (ZX Spectrum), running over SLIP on the Interface 1
RS-232 port. See `README.tcpip` for the design and protocol details.

## Building

The Spectrum programs are cross-compiled with **z88dk / zcc**, which is NOT on
the default PATH. Before running `make` (or any `zcc` command), source the SDK
setup script:

```sh
. /data/sahlberg/z88dk/zcc-setup.sh
make
```

That script sets `PATH` (adds `/data/sahlberg/z88dk/bin`), `ZCCCFG`
(`/data/sahlberg/z88dk/lib/config`) and the local::lib perl environment that
z88dk's tools need. Without it the build fails with `zcc: command not found`.

Note: `sh` cannot source and run make in one shell invocation unless chained,
e.g. `. /data/sahlberg/z88dk/zcc-setup.sh && make`.

Targets in the `Makefile`:

* `ping.bin`, `tcp_chat.bin` — Spectrum programs, built with
  `zcc +zx ... -lndos -lrs232if1 -create-app` (also produces `.tap` files).
* `slip-tun` — Linux host bridge, built with the native `gcc` (not zcc).

Objects are built with `-DZ80 -DSLIP_ESC_00 -DRS232_TPS=80`. `SLIP_ESC_00` is
the FUSE Interface 1 workaround (FUSE treats 0x00 as an escape), and must match
between the Z80 side and `slip-tun`.

## Running under FUSE

```sh
./NET.sh                                    # builds and runs slip-tun (needs sudo, creates tun0)
fuse --rs232-tx=/tmp/tx --rs232-rx=/tmp/rx  # in a second console
```

Always go through `NET.sh` rather than running `slip-tun` by hand. Besides
building and starting the bridge it enables `ip_forward` and installs the
MASQUERADE / FORWARD rules without which the Spectrum can only reach 192.0.2.1
(192.0.2.0/24 is TEST-NET-1, so nothing routes a reply back to it). The rules
are removed again on exit, so stop it with ^C. Note that outbound packets are
re-injected by `slip-tun` on a raw `IP_HDRINCL` socket and therefore never
traverse FORWARD — only POSTROUTING — while the replies *are* forwarded out
`tun0`; see "Reaching the outside world" in `README.tcpip`.

The Spectrum side is hardcoded to 192.0.2.2; `slip-tun` owns the other end of
the tun device. Pass `-v` to `slip-tun` to log every packet.

## Platform constraints — no OS, no clock

This is a bare Z80 (Spectrum 48k + Interface 1). There is no operating system,
no scheduler, no `time()`, no timer interrupt to hook. Everything is a single
blocking loop, and there is no way to ask "how long has it been?". The design
works around this in a few deliberately inventive ways — don't "fix" them by
reaching for facilities that do not exist here:

* **Time is counted in `rs232_get()` timeouts, not in seconds.** On an idle
  serial port a failed `rs232_get()` takes a roughly fixed amount of time
  (~12ms on FUSE Interface 1). That *is* the clock. `-DRS232_TPS=n` tells the
  stack how many such timed-out calls make up one second (80 for Spectrum
  Interface 1); it must be re-measured for any other serial interface, though
  it does not need to be accurate. `recv_packet()` (`slip.c:105`) just
  decrements the `to` counter once per idle `rs232_get()` and returns `-EAGAIN`
  when it hits zero — that is the entire timeout implementation.
* **~1s timeouts** in `tcp_recv()` / retransmission therefore mean "RS232_TPS
  idle reads", not wall-clock time (`tcp.c:167`).
* **Retransmission is retry-count based**, not RTO based: `tcp_send()` resends
  up to 5 times whenever `tcp_recv()` returns `-EAGAIN`; `tcp_connect()` makes
  5 attempts.
* **The machine is very slow relative to the peer.** We frequently fail to ACK
  in time and the server retransmits, so the stack must detect and ignore
  duplicate incoming segments (sequence-number check in `tcp.c` around line
  225) — this is normal operation, not an error path.
* **There is no entropy source.** For a fresh random source port on a
  connection retry, the code adds the last packet's checksum plus the Z80 `R`
  (DRAM refresh) register, read via inline `#asm` in `get_r_register()`
  (`tcp.c:247`).

* **There is no serial receive buffer.** Interface 1 RS-232 is bit-banged;
  bytes that arrive while the program is not inside `rs232_get()` are simply
  lost. That is why the stack keeps the TCP window closed except while it is
  actually sitting in `recv_packet()`, and why a polling app must do as little
  as possible between polls and let the peer retransmit.

## z88dk console and keyboard facts (verified against the SDK sources)

The `+zx` classic target links `fputc_cons_generic_full`
(`libsrc/classic/stdio/fputc_cons_generic.inc`) — this is the driver whose
behaviour the screen code depends on:

* The console is **64x24 by default** (`CONSOLE_COLUMNS=64`, the 4x8 font);
  `#pragma output CLIB_ZX_CONIO32 = 1` switches it to 32 columns.
* `22, y+32, x+32` = goto (`PRINT AT` encoding). `12` = cls+home, `8` = left,
  `13` = CR, `10` = LF. `27 'K'` = clear to end of line, `20,n` = inverse.
* Printing `\n` on the last row **scrolls the whole screen** and leaves the
  cursor at column 0 of the last row. There is no scrolling region, so
  "log area + fixed bottom line" is done by printing the log line *on* the
  bottom row and letting the newline scroll it up (see `log_line()` in
  `tcp_chat.c`).
* Wrapping past the last column defers the scroll: the cursor goes to row 24
  and the scroll happens when the *next* character is printed. An explicit
  goto cancels that pending scroll.
* Codes 1,2,3,4,16,17,18,19,20,22 consume the following byte(s) as arguments,
  so anything received off the network must be filtered before printing.

Keyboard:

* `scanf()`/`fgets_cons()`/`fgetc_cons()` read the ROM's LAST-K (23560), which
  is only updated by the **50Hz interrupt routine**. `rs232_get()` runs with
  interrupts disabled for ~9ms of every ~12ms call, so while a SLIP session is
  active those interrupts almost never fire and LAST-K is effectively dead.
  **Do not use stdio input while connected.**
* Use `in_GetKey()` from `<input.h>` instead: it scans the keyboard ports
  directly. Its debounce/repeat are counted in *number of calls*, so
  `in_KeyDebounce` / `in_KeyStartRepeat` / `in_KeyRepeatPeriod` have to be
  tuned to how often the program's loop calls it. Those variables are already
  defined by the library (`in_GetKeyVariables.asm`) — assign to them, do not
  define them.
* Key codes come from `in_keytranstbl.asm`: ENTER 13, CAPS+0 (DELETE) 12,
  CAPS+5..8 (arrows) 8/10/11/9, CAPS+SYM+C 3, CAPS+letter = uppercase,
  SYM+letter = punctuation.

## Source layout

* `slip.c/h` — send/receive IP packets over RS-232 (SLIP framing)
* `ip.c/h`   — IP header construction
* `icmp.c/h` — ICMP (only needed by `ping.c`)
* `tcp.c/h`  — minimal single-connection, single-buffer TCP
* `ping.c`, `tcp_chat.c` — the Spectrum applications
* `slip-tun.c` — Linux-side SLIP<->tun bridge
* `test/` — host-side tests, run with `make test` (plain gcc, no z88dk needed)

`tcp_chat.c` is a chat client for `ncat --chat -l <port>`. It asks for the
server IP and port, then runs one polling loop that interleaves `in_GetKey()`
with `tcp_recv_timeout(..., POLL_TICKS)` so that typing and receiving happen
at the same time. The bottom screen row is the input line, the rows above it
are the chat log.

## Testing without a Spectrum

`make test` builds `test/test_screen.c` with the native gcc. It mocks the
z88dk console driver (same control codes, wrapping and scroll semantics as
`fputc_cons_generic.inc`) and `#include`s `tcp_chat.c` itself — `putchar`,
`printf` and `main` are `#define`d away around the include, and `test/stub/`
supplies the z88dk headers (`arch/zx.h`, `input.h`, `net/hton.h`, `rs232.h`).
This is the cheap way to check screen/input logic; it does not exercise any
networking, which still needs FUSE + slip-tun.
