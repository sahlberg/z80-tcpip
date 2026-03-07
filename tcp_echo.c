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
#include <arch/zx.h>
#include <net/hton.h>
#include <stdio.h>
#include <rs232.h>

#include "slip.h"
#include "ip.h"
#include "tcp.h"

#define BUF_SIZE (20 + 20 + 512)

uint8_t tcp_buf[BUF_SIZE];
tcp_context_t tcp = {
        .ip = {
                .pkt_size = BUF_SIZE,
                .pkt = tcp_buf,
        },
};


int main(void)
{
        int rc, i;
        int ipi[4];
        uint8_t *ip;

        zx_cls();
        tcp.ip.saddr = 0x020200c0; /* 192.0.2.2 */
        
        ip = (uint8_t *)&tcp.ip.saddr;
        printf("My IP ADDRESS: %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);
        printf("IP ADDRESS for echo server: ");fflush(stdout);
        scanf("%d.%d.%d.%d\n", &ipi[0], &ipi[1], &ipi[2], &ipi[3]);
        ip = (uint8_t *)&tcp.ip.daddr;
        ip[0] = ipi[0];
        ip[1] = ipi[1];
        ip[2] = ipi[2];
        ip[3] = ipi[3];
        printf("\n");

        tcp.src_port = htons(32768);
        tcp.dst_port = htons(31337);

        slip_init(RS_BAUD_19200, RS_PAR_NONE);
        rc = tcp_connect(&tcp);
        printf("connect rc:%d\n", rc);
        while(1) {
                rc = tcp_recv(tcp, 64);
                printf("recv rc:%d\n", rc);
                if (rc > 0) {
                        for(i = 0; i < rc; i++) {
                                putchar(tcp_rx_buffer(tcp)[i]);
                        }
                }
        }
        printf("done\n");
        return 0;
}

