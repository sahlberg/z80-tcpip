#ifndef _STUB_INPUT_H
#define _STUB_INPUT_H
extern char in_KeyDebounce;
extern char in_KeyStartRepeat;
extern char in_KeyRepeatPeriod;
unsigned int in_GetKey(void);
void in_GetKeyReset(void);
void in_Wait(unsigned int msec);
#endif
void in_WaitForNoKey(void);
