/* ========================= The device calls on a computer: a simulated board =========================
   lib/machine.py drives pins and buses with sys.syscall(PPY_DEV_..., ...)
   (src/targets/c/ppy_port.h lists them); a board's port does them. On a
   computer - the interpreter, the macos target, the c target's posix port -
   lx_emul.c sends them here: a board of 64 pins that says on stdout what the
   program does with it, so device code runs and can be tested without one:

     [pin 2] out           [pin 2] = 1          [pwm 5] 1000 Hz, duty 32768
     [i2c 0] scl 22, sda 21, 400000 Hz          [i2c 0] write to 0x3c: 00 af
     [uart 1] write: 68 69 21                    [machine] reset

   Reads give fixed values: an output pin its last value, an input pin 1 with
   a pull-up and 0 else; ADCs half of the scale; I2C reads zeros and finds no
   device; SPI sends back what it is sent; UARTs receive nothing. (Included
   by lx_emul.c, which gives LX_WORD, LX_PTR and LX_FLUSH.) */

#include <stdio.h>
#include <string.h>

typedef struct { int mode, pull, value, pwm; } LxSimPin;
static LxSimPin lx_sim_pins[64];
static unsigned lx_sim_adc_atten[64];

static void lx_sim_say(const char *s){ LX_FLUSH(); size_t n=strlen(s); ssize_t w=write(1,s,n); (void)w; }
static void lx_sim_hex(char *o, size_t cap, const unsigned char *b, uint32_t n){
    size_t k=strlen(o);
    for(uint32_t i=0;i<n && k+4<cap;i++){ snprintf(o+k,cap-k," %02x",b[i]); k+=3; }
    if(k+2<cap){ o[k++]='\n'; o[k]=0; }
}
static int64_t lx_devsim(const LX_WORD *r){
    char t[512]; uint32_t op=(uint32_t)r[0], a=(uint32_t)r[1], b=(uint32_t)r[2], c=(uint32_t)r[3], d=(uint32_t)r[4], e=(uint32_t)r[5];
    static const char *const modes[]={"in","out","open drain"}, *const pulls[]={"",", pull up",", pull down"};
    switch(op){
        case 0x7000:                                    /* PIN_INIT pin, mode, pull */
            if(a>=64) return -ENODEV;
            if(b>2 || c>2) return -EINVAL;
            lx_sim_pins[a].mode=(int)b; lx_sim_pins[a].pull=(int)c;
            snprintf(t,sizeof t,"[pin %u] %s%s\n",a,modes[b],pulls[c]); lx_sim_say(t); return 0;
        case 0x7001:                                    /* PIN_WRITE pin, value */
            if(a>=64) return -ENODEV;
            lx_sim_pins[a].value=b!=0;
            snprintf(t,sizeof t,"[pin %u] = %u\n",a,b!=0); lx_sim_say(t); return 0;
        case 0x7002:                                    /* PIN_READ pin */
            if(a>=64) return -ENODEV;
            if(lx_sim_pins[a].mode==0) return lx_sim_pins[a].pull==1;
            return lx_sim_pins[a].value;
        case 0x7010:                                    /* ADC_INIT pin, attenuation */
            if(a>=64) return -ENODEV;
            if(b>3) return -EINVAL;
            lx_sim_adc_atten[a]=b;
            { static const char *const db[]={"0","2.5","6","11"}; snprintf(t,sizeof t,"[adc %u] %s dB\n",a,db[b]); lx_sim_say(t); }
            return 0;
        case 0x7011: if(a>=64) return -ENODEV; return 32768;               /* ADC_READ */
        case 0x7012:{ static const int64_t fs[]={950000,1250000,1750000,2450000};   /* ADC_READ_UV */
            if(a>=64) return -ENODEV; return fs[lx_sim_adc_atten[a]&3]/2; }
        case 0x7020:                                    /* PWM_INIT pin, freq, duty */
            if(a>=64) return -ENODEV;
            if(!b || c>65535) return -EINVAL;
            lx_sim_pins[a].pwm=1;
            snprintf(t,sizeof t,"[pwm %u] %u Hz, duty %u\n",a,b,c); lx_sim_say(t); return 0;
        case 0x7021: if(a>=64 || !lx_sim_pins[a].pwm) return -ENODEV;      /* PWM_DUTY */
            if(b>65535) return -EINVAL;
            snprintf(t,sizeof t,"[pwm %u] duty %u\n",a,b); lx_sim_say(t); return 0;
        case 0x7022: if(a>=64 || !lx_sim_pins[a].pwm) return -ENODEV;      /* PWM_FREQ */
            if(!b) return -EINVAL;
            snprintf(t,sizeof t,"[pwm %u] %u Hz\n",a,b); lx_sim_say(t); return 0;
        case 0x7023: if(a>=64) return -ENODEV;                             /* PWM_DEINIT */
            lx_sim_pins[a].pwm=0; snprintf(t,sizeof t,"[pwm %u] off\n",a); lx_sim_say(t); return 0;
        case 0x7030:                                    /* I2C_INIT bus, scl, sda, freq */
            if(a>1) return -ENODEV;
            snprintf(t,sizeof t,"[i2c %u] scl %u, sda %u, %u Hz\n",a,b,c,d); lx_sim_say(t); return 0;
        case 0x7031:                                    /* I2C_WRITE bus, addr, buf, len, stop */
            if(a>1) return -ENODEV;
            snprintf(t,sizeof t,"[i2c %u] write to 0x%02x%s:",a,b,e?"":" (no stop)"); lx_sim_hex(t,sizeof t,(const unsigned char*)LX_PTR(r[3]),d); lx_sim_say(t);
            return d;
        case 0x7032:                                    /* I2C_READ bus, addr, buf, len, stop */
            if(a>1) return -ENODEV;
            if(d) memset(LX_PTR(r[3]),0,d);
            snprintf(t,sizeof t,"[i2c %u] read %u bytes from 0x%02x\n",a,d,b); lx_sim_say(t); return d;
        case 0x7033:                                    /* I2C_SCAN bus, buf[128] */
            if(a>1) return -ENODEV;
            memset(LX_PTR(r[2]),0,128); return 0;
        case 0x7040:                                    /* SPI_INIT bus, sck, mosi, miso, baud, mode */
            if(a>3) return -ENODEV;
            snprintf(t,sizeof t,"[spi %u] sck %d, mosi %d, miso %d, %u baud, mode %u\n",a,(int)b,(int)c,(int)d,e,(unsigned)r[6]); lx_sim_say(t); return 0;
        case 0x7041:{                                   /* SPI_XFER bus, tx, rx, len */
            if(a>3) return -ENODEV;
            const unsigned char *tx=b?(const unsigned char*)LX_PTR(r[2]):NULL; unsigned char *rx=c?(unsigned char*)LX_PTR(r[3]):NULL;
            snprintf(t,sizeof t,"[spi %u] %u bytes:",a,d);
            if(tx) lx_sim_hex(t,sizeof t,tx,d); else { size_t k=strlen(t); snprintf(t+k,sizeof t-k," (read)\n"); }
            lx_sim_say(t);
            if(rx){ if(tx) memmove(rx,tx,d); else memset(rx,0xFF,d); }
            return d; }
        case 0x7050:                                    /* UART_INIT uart, baud, tx, rx */
            if(a>2) return -ENODEV;
            snprintf(t,sizeof t,"[uart %u] %u baud, tx %d, rx %d\n",a,b,(int)c,(int)d); lx_sim_say(t); return 0;
        case 0x7051:                                    /* UART_WRITE uart, buf, len */
            if(a>2) return -ENODEV;
            snprintf(t,sizeof t,"[uart %u] write:",a); lx_sim_hex(t,sizeof t,(const unsigned char*)LX_PTR(r[2]),c); lx_sim_say(t); return c;
        case 0x7052: if(a>2) return -ENODEV; return 0;                     /* UART_READ: nothing comes */
        case 0x7053: if(a>2) return -ENODEV; return 0;                     /* UART_ANY */
        case 0x7060: lx_sim_say("[machine] reset\n"); LX_FLUSH(); exit(0);
        case 0x7061: return 160000000;                                     /* FREQ */
        case 0x7062:{ static const unsigned char id[6]={0xa0,0xb1,0xc2,0xd3,0xe4,0xf5};   /* UNIQUE_ID buf, len */
            uint32_t n=a?(b<6?b:6):0; if(n) memcpy(LX_PTR(r[1]),id,n); return n; }
        case 0x7063: snprintf(t,sizeof t,"[machine] deepsleep %u ms\n",a); lx_sim_say(t); LX_FLUSH(); exit(0);
        default: return -ENOSYS;                                           /* (0x7800..: a board's own) */
    }
}
