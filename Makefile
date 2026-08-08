CFILESNET = icmp.c ip.c slip.c tcp.c
OFILESNET = $(CFILESNET:.c=.o)

# Programs for the spectrum, built with z88dk
PROGRAMS = ping.bin tcp_chat.bin

# Programs for the linux host, built with the native compiler
HOSTPROGRAMS = slip-tun

CC = gcc
CFLAGS = -Wall -O2

all: $(PROGRAMS) $(HOSTPROGRAMS)


ping.bin: ping.o $(OFILESNET)
	zcc +zx -o $@ ping.c $(OFILESNET) -lndos -lrs232if1 -create-app

tcp_chat.bin: tcp_chat.o $(OFILESNET)
	zcc +zx -o $@ tcp_chat.c $(OFILESNET) -lndos -lrs232if1 -create-app

%.o: %.c
	zcc +zx -o $@ -c $^ -I.. -DZ80 -DSLIP_ESC_00 -DRS232_TPS=80

# Built straight from the .c file so it does not pick up the zcc rule above.
slip-tun: slip-tun.c
	$(CC) $(CFLAGS) -DSLIP_ESC_00 -o $@ $<

# Host side tests for the screen/input handling in tcp_chat.c.
# test/stub/ has just enough of the z88dk headers to build it here.
test/test_screen: test/test_screen.c tcp_chat.c tcp.h ip.h
	$(CC) $(CFLAGS) -I test/stub -I . -o $@ $<

test: test/test_screen
	./test/test_screen

clean:
	$(RM) *.o ../*.o *.tap *.bin $(HOSTPROGRAMS) test/test_screen

.PHONY: all clean test
