/* ---------------------------------------------------------------- the i386 machine in C
   (aot/x2c_machine.c: part of the C runtime of every translated program; the
   runtime that includes it - targets/macos/x2c_host.c, targets/c/x2c_port.c -
   gives <stdint.h> and the math functions it names first.) The program keeps
   running as the i386 code it was: 32-bit registers and lazy flags, the x87
   stack in double precision (which compiled code sets anyway), and memory as
   a guest address space: guest address a is at host address m + a, the
   layout of which the runtime decides. */

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t i32;
typedef uint64_t u64; typedef int64_t i64; typedef int16_t i16; typedef int8_t i8;
typedef u16 u16u __attribute__((aligned(1),may_alias));
typedef u32 u32u __attribute__((aligned(1),may_alias));
typedef u64 u64u __attribute__((aligned(1),may_alias));
typedef double f64u __attribute__((aligned(1),may_alias));
typedef float f32u __attribute__((aligned(1),may_alias));

/* what aot/x2c.c lays out: code labels at X2C_CODE_BASE + 4 * n (only
   numbers, nothing is behind them), the listing's data from X2C_DATA_BASE */
#define X2C_CODE_BASE   0x00100000u
#define X2C_DATA_BASE   0x08000000u

/* guest memory of the translated code: m is a local copy of M */
#define B(a)  (*(u8*)(m+(u32)(a)))
#define W(a)  (*(u16u*)(m+(u32)(a)))
#define D(a)  (*(u32u*)(m+(u32)(a)))
#define Q(a)  (*(u64u*)(m+(u32)(a)))
#define FD(a) (*(f64u*)(m+(u32)(a)))
#define FS(a) (*(f32u*)(m+(u32)(a)))

/* partial registers */
#define SET8L(r,v)  (r=((r)&0xFFFFFF00u)|(u8)(v))
#define SET8H(r,v)  (r=((r)&0xFFFF00FFu)|((u32)(u8)(v)<<8))
#define SET16(r,v)  (r=((r)&0xFFFF0000u)|(u16)(v))

/* Flags are kept lazily: the kind of the last instruction that set them
   (fk; bits 8+: how far 8/16-bit operands are shifted up so that their sign
   is bit 31), its operands fa fb, result fr and a carry fc. */
enum { FK_ADD, FK_SUB, FK_LOGIC, FK_INC, FK_DEC, FK_SHIFT, FK_MUL, FK_SAHF, FK_ADC, FK_SBB };
enum { CC_O, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A, CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G };
#define X2C_INLINE static inline __attribute__((always_inline))
X2C_INLINE int x2c_cf(u32 k, u32 a, u32 b, u32 r, u32 c){
    switch(k&15){ case FK_ADD: return r<a; case FK_SUB: return a<b; case FK_LOGIC: return 0; case FK_SAHF: return r&1; default: return c&1; }   /* adc, sbb: c */
}
X2C_INLINE int x2c_of(u32 k, u32 a, u32 b, u32 r, u32 c){
    switch(k&15){ case FK_ADD: case FK_INC: case FK_ADC: return (~(a^b)&(a^r))>>31; case FK_SUB: case FK_DEC: case FK_SBB: return ((a^b)&(a^r))>>31;
                  case FK_MUL: return c&1; default: return 0; }
}
X2C_INLINE int x2c_zf(u32 k, u32 r){ return (k&15)==FK_SAHF ? (r>>6)&1 : r==0; }
X2C_INLINE int x2c_sf(u32 k, u32 r){ return (k&15)==FK_SAHF ? (r>>7)&1 : r>>31; }
X2C_INLINE int x2c_pf(u32 k, u32 r){ return (k&15)==FK_SAHF ? (r>>2)&1 : !__builtin_parity((r>>(k>>8))&255); }
X2C_INLINE int x2c_cc(int cc, u32 k, u32 a, u32 b, u32 r, u32 c){
    if((k&15)==FK_SUB) switch(cc){                      /* cmp: straight comparisons */
        case CC_B: return a<b; case CC_AE: return a>=b; case CC_E: return a==b; case CC_NE: return a!=b;
        case CC_BE: return a<=b; case CC_A: return a>b; case CC_L: return (i32)a<(i32)b; case CC_GE: return (i32)a>=(i32)b;
        case CC_LE: return (i32)a<=(i32)b; case CC_G: return (i32)a>(i32)b; default: break; }
    if((k&15)==FK_LOGIC) switch(cc){                    /* test, and, or, xor */
        case CC_B: return 0; case CC_AE: return 1; case CC_E: case CC_BE: return r==0; case CC_NE: case CC_A: return r!=0;
        case CC_L: case CC_S: return (i32)r<0; case CC_GE: case CC_NS: return (i32)r>=0; case CC_LE: return (i32)r<=0; case CC_G: return (i32)r>0;
        case CC_O: return 0; case CC_NO: return 1; default: break; }
    int cf=x2c_cf(k,a,b,r,c), zf=x2c_zf(k,r), sf=x2c_sf(k,r), of=x2c_of(k,a,b,r,c);
    switch(cc){
        case CC_O: return of; case CC_NO: return !of; case CC_B: return cf; case CC_AE: return !cf;
        case CC_E: return zf; case CC_NE: return !zf; case CC_BE: return cf||zf; case CC_A: return !cf&&!zf;
        case CC_S: return sf; case CC_NS: return !sf; case CC_P: return x2c_pf(k,r); case CC_NP: return !x2c_pf(k,r);
        case CC_L: return sf!=of; case CC_GE: return sf==of; case CC_LE: return zf||sf!=of; default: return !zf&&sf==of;
    }
}
#define CC(cc) x2c_cc(cc,fk,fa,fb,fr,fc)
#define CF_NOW x2c_cf(fk,fa,fb,fr,fc)

/* x87: st(i) = fs[(ft+i)&7] */
#define ST(i)    fs[(ft+(i))&7]
#define FPUSH(v) do{ double fpush_=(v); ft=(ft-1)&7; fs[ft]=fpush_; }while(0)
#define FPOP()   (ft=(ft+1)&7)
X2C_INLINE u32 x2c_fcmp(double a, double b){ return a!=a||b!=b ? 0x4500u : a<b ? 0x0100u : a==b ? 0x4000u : 0u; }
#define FCOM(a,b) (fsw=(fsw&~0x4700u)|x2c_fcmp(a,b))
X2C_INLINE double x2c_round(double v, u32 cw){
    switch((cw>>10)&3){ case 0: return nearbyint(v); case 1: return floor(v); case 2: return ceil(v); default: return trunc(v); }
}
X2C_INLINE u32 x2c_fist32(double v, u32 cw){ double r=x2c_round(v,cw); return r>=-2147483648.0 && r<2147483648.0 ? (u32)(i32)r : 0x80000000u; }
X2C_INLINE u16 x2c_fist16(double v, u32 cw){ double r=x2c_round(v,cw); return r>=-32768.0 && r<32768.0 ? (u16)(i16)r : 0x8000u; }
X2C_INLINE u64 x2c_fist64(double v, u32 cw){ double r=x2c_round(v,cw); return r>=-9223372036854775808.0 && r<9223372036854775808.0 ? (u64)(i64)r : 0x8000000000000000ull; }
/* fyl2x: st1 * log2(st0); with the constants compiled code loads for ln and
   log10, the library's functions (the x87 computes those exactly) */
static double x2c_fyl2x(double y, double x){
    if(y==0.6931471805599453) return log(x);
    if(y==0.30102999566398120) return log10(x);
    if(y==1.0) return log2(x);
    return y*log2(x);
}
