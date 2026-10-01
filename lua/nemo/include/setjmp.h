#ifndef NEMO_SETJMP_H
#define NEMO_SETJMP_H
// 22 palabras: x19-x30, sp, d8-d15 en ARM64 (mas que sobra en x86-64 host)
typedef unsigned long long jmp_buf[22];
int  nemo_setjmp(jmp_buf env);
void nemo_longjmp(jmp_buf env, int val) __attribute__((noreturn));
#define setjmp(e)      nemo_setjmp(e)
#define longjmp(e, v)  nemo_longjmp(e, v)
#endif
