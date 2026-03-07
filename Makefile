CFILESNET = icmp.c ip.c slip.c tcp.c
OFILESNET = $(CFILESNET:.c=.o)

PROGRAMS = ping.bin

all: $(PROGRAMS)


ping.bin: ping.o $(OFILESNET)
	zcc +zx -o $@ ping.c $(OFILESNET) -lndos -lrs232if1 -create-app 

%.o: %.c
	zcc +zx -o $@ -c $^ -I.. -DUSMB2_FEATURE_CLOSE -DUSMB2_FEATURE_NTLM -DZ80 -DSLIP_ESC_00 -DRS232_TPS=80

clean:
	$(RM) *.o ../*.o *.tap *.bin
