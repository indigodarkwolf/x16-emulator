/*

						Extracted from original single fake6502.c file

*/

#ifndef SUPPORT_H
#define SUPPORT_H

//flag modifier macros
#define setflags(BITS) CPU.status |= (BITS)
#define clearflags(BITS) CPU.status &= ~(BITS)
#define setcarry() CPU.status |= (FLAG_CARRY)
#define clearcarry() CPU.status &= (~FLAG_CARRY)
#define setzero() CPU.status |= (FLAG_ZERO)
#define clearzero() CPU.status &= (~FLAG_ZERO)
#define setinterrupt() CPU.status |= (FLAG_INTERRUPT)
#define clearinterrupt() CPU.status &= (~FLAG_INTERRUPT)
#define setdecimal() CPU.status |= (FLAG_DECIMAL)
#define cleardecimal() CPU.status &= (~FLAG_DECIMAL)
#define setoverflow() CPU.status |= (FLAG_OVERFLOW)
#define clearoverflow() CPU.status &= (~FLAG_OVERFLOW)
#define setsign() CPU.status |= (FLAG_SIGN)
#define clearsign() CPU.status &= (~FLAG_SIGN)

#define select_carry(A) (((A) >> 8) & 1)
#define select_zero(A) (((A) == 0) << 1)
#define select_overflow(R, A, M) ((((R)^(A) & (R)^(M)) & 0x80) >> 1)
#define select_sign(A) ((A) & 0x80)

//a few general functions used by various other functions
inline static void push16(uint16_t pushval) {
    write6502(BASE_STACK + CPU.sp, (pushval >> 8) & 0xFF);
    write6502(BASE_STACK + ((CPU.sp - 1) & 0xFF), pushval & 0xFF);
    CPU.sp -= 2;
}

inline static void push8(uint8_t pushval) {
    write6502(BASE_STACK + CPU.sp--, pushval);
}

inline static uint16_t pull16(void) {
    const uint16_t temp16 = read6502(BASE_STACK + ((CPU.sp + 1) & 0xFF)) | ((uint16_t)read6502(BASE_STACK + ((CPU.sp + 2) & 0xFF)) << 8);
    CPU.sp += 2;
    return(temp16);
}

inline static uint8_t pull8(void) {
    return (read6502(BASE_STACK + ++CPU.sp));
}

inline static uint8_t read8(void) {
    return read6502(CPU.pc++);
}

inline static uint16_t read16(void) {
    const uint16_t value = (uint16_t)read6502(CPU.pc) | ((uint16_t)read6502(CPU.pc+1) << 8);
    CPU.pc += 2;
    return value;
}

#endif // SUPPORT_H
