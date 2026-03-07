/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
#include <stdio.h>
#include <rs232.h>

int main(int argc, char *argv[])
{
        int i;
        
        if (rs232_init() != RS_ERR_OK) {
                return -1;
        }
        if (rs232_params(RS_BAUD_9600, RS_PAR_NONE) != RS_ERR_OK) {
                return -1;
        }

        rs232_put('H');
        rs232_put('e');
        rs232_put('l');
        rs232_put('l');
        rs232_put('o');
        for (i = 0; i < 32; i++) {
                rs232_put(i);
        }
        printf("foo\n");
        return 0;
}
