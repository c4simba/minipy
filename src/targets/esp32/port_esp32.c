/* ========================= ppy_port_esp32.c: the port of plainpy programs to the ESP32 family (ESP-IDF 5.2+) =========================
   plainpy --compile --target esp32 app.py writes an ESP-IDF project, app/,
   with the program (main/app.c), ppy_port.h and this file; `idf.py build
   flash monitor` in it does the rest (plainpy runs `idf.py build` itself
   when ESP-IDF is set up). Any chip of the family: ESP32, -S2, -S3 (Xtensa),
   -C3, -C6, -H2, -P4 (RISC-V) - `--chip`, or `idf.py set-target`.

     console    print() and input(): the ESP-IDF console (UART0 or the USB
                serial port, as sdkconfig has it; 115200 baud)
     time       esp_timer (monotonic), gettimeofday (the wall clock: since boot
                until SNTP or your code sets it), vTaskDelay
     memory     the arena: PSRAM when the board has it, else the largest free
                block of internal RAM less what ESP-IDF needs (PPY_ARENA_SIZE,
                --arena, asks for an exact size)
     devices    lib/machine.py: Pin (GPIO), ADC (oneshot, calibrated where the
                chip can), PWM (LEDC), I2C (i2c_master), SPI (spi_master), UART,
                reset, freq, unique_id, deepsleep
     the end    the program's exit status is printed and the chip idles
                (PPY_ESP32_RESTART: restarts it instead, after a second)

   C functions of ESP-IDF itself are at hand through ctypes: a C file the
   program names with ctypes.CDLL(None) is linked with it, so
   CDLL(None).esp_get_free_heap_size() just works. Add your own device calls
   (0x7800.., machine.call(n, ...)) in ppy_port_user() at the end. */

#include "ppy_port.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_sleep.h"
#include "esp_rom_sys.h"
#include "sdkconfig.h"
#include "esp_idf_version.h"
#include "soc/soc_caps.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

/* ---------------------------------------------------------------- memory */
void *ppy_port_arena(uint32_t *size){
    uint32_t n=*size; void *p=NULL;
#if CONFIG_SPIRAM
    if(!n){ size_t big=heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT); if(big>(1u<<20)) n=(uint32_t)(big>(8u<<20)?(8u<<20):big); }
    if(n){ p=heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT); if(p){ *size=n; return p; } }
#endif
    if(!n){                                             /* internal RAM: leave ESP-IDF room for its own (Wi-Fi needs more) */
        size_t big=heap_caps_get_largest_free_block(MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL);
        n=big>32768u?(uint32_t)(big-24576u):(uint32_t)(big/2);
    }
    p=heap_caps_malloc(n,MALLOC_CAP_8BIT|MALLOC_CAP_INTERNAL);
    if(!p) p=heap_caps_malloc(n,MALLOC_CAP_8BIT);
    *size=p?n:0;
    return p;
}

/* ---------------------------------------------------------------- console, time, random */
int32_t ppy_port_write(int fd, const void *buf, uint32_t n){
    ssize_t w=write(fd==2?2:1,buf,n);
    return w<0 ? -PPY_EIO : (int32_t)w;
}
int32_t ppy_port_read(int fd, void *buf, uint32_t n){   /* a line; the console's reads do not wait, so poll */
    (void)fd;
    char *p=(char*)buf; uint32_t k=0;
    while(k<n){
        ssize_t r=read(0,p+k,1);
        if(r<=0){ vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if(p[k]=='\r') p[k]='\n';
        if(p[k++]=='\n') break;
    }
    return (int32_t)k;
}
uint64_t ppy_port_clock_ns(int clock){
    if(clock==0){ struct timeval tv; gettimeofday(&tv,NULL); return (uint64_t)tv.tv_sec*1000000000u+(uint64_t)tv.tv_usec*1000u; }
    return (uint64_t)esp_timer_get_time()*1000u;
}
void ppy_port_sleep_ns(uint64_t ns){                     /* at least that long: vTaskDelay(n) may wake a part of a tick early */
    int64_t until=esp_timer_get_time()+(int64_t)(ns/1000u), tick_us=(int64_t)portTICK_PERIOD_MS*1000;
    for(;;){
        int64_t left=until-esp_timer_get_time();
        if(left<=0) return;
        if(left>tick_us) vTaskDelay((TickType_t)(left/tick_us));   /* (other tasks run meanwhile) */
        else esp_rom_delay_us((uint32_t)left);
    }
}
void ppy_port_random(void *buf, uint32_t n){ esp_fill_random(buf,n); }
void ppy_port_panic(const char *msg){
    (void)msg;                                          /* (printed already) */
    vTaskDelay(pdMS_TO_TICKS(1000));
#ifdef PPY_ESP32_RESTART
    esp_restart();
#endif
    for(;;) vTaskDelay(portMAX_DELAY);
}

/* ---------------------------------------------------------------- GPIO */
static int ppy_pin_ok(uint32_t pin){ return pin<64 && GPIO_IS_VALID_GPIO((int)pin); }
static int64_t ppy_pin_init(uint32_t pin, uint32_t mode, uint32_t pull){
    if(!ppy_pin_ok(pin) || (mode && !GPIO_IS_VALID_OUTPUT_GPIO((int)pin))) return -PPY_ENODEV;
    if(mode>2 || pull>2) return -PPY_EINVAL;
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, mode==0?GPIO_MODE_INPUT:mode==1?GPIO_MODE_INPUT_OUTPUT:GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_pull_mode((gpio_num_t)pin, pull==1?GPIO_PULLUP_ONLY:pull==2?GPIO_PULLDOWN_ONLY:GPIO_FLOATING);
    return 0;
}

/* ---------------------------------------------------------------- ADC (oneshot) */
static adc_oneshot_unit_handle_t ppy_adc_unit[2];
static adc_cali_handle_t ppy_adc_cali[64];
static uint8_t ppy_adc_atten[64];
static int64_t ppy_adc_init(uint32_t pin, uint32_t atten){
    adc_unit_t unit; adc_channel_t ch;
    if(pin>=64 || adc_oneshot_io_to_channel((int)pin,&unit,&ch)!=ESP_OK) return -PPY_ENODEV;
    if(atten>3) return -PPY_EINVAL;
    int u=unit==ADC_UNIT_1?0:1;
    if(!ppy_adc_unit[u]){ adc_oneshot_unit_init_cfg_t cfg={.unit_id=unit}; if(adc_oneshot_new_unit(&cfg,&ppy_adc_unit[u])!=ESP_OK) return -PPY_EBUSY; }
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
    static const adc_atten_t at[4]={ADC_ATTEN_DB_0,ADC_ATTEN_DB_2_5,ADC_ATTEN_DB_6,ADC_ATTEN_DB_12};   /* (11 dB was renamed 12) */
#else
    static const adc_atten_t at[4]={ADC_ATTEN_DB_0,ADC_ATTEN_DB_2_5,ADC_ATTEN_DB_6,ADC_ATTEN_DB_11};
#endif
    adc_oneshot_chan_cfg_t cc={.atten=at[atten],.bitwidth=ADC_BITWIDTH_DEFAULT};
    if(adc_oneshot_config_channel(ppy_adc_unit[u],ch,&cc)!=ESP_OK) return -PPY_EINVAL;
    ppy_adc_atten[pin]=(uint8_t)atten;
    ppy_adc_cali[pin]=NULL;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    { adc_cali_curve_fitting_config_t k={.unit_id=unit,.chan=ch,.atten=at[atten],.bitwidth=ADC_BITWIDTH_DEFAULT}; adc_cali_create_scheme_curve_fitting(&k,&ppy_adc_cali[pin]); }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    { adc_cali_line_fitting_config_t k={.unit_id=unit,.atten=at[atten],.bitwidth=ADC_BITWIDTH_DEFAULT}; adc_cali_create_scheme_line_fitting(&k,&ppy_adc_cali[pin]); }
#endif
    return 0;
}
static int64_t ppy_adc_read(uint32_t pin, int uv){
    adc_unit_t unit; adc_channel_t ch; int raw=0;
    if(pin>=64 || adc_oneshot_io_to_channel((int)pin,&unit,&ch)!=ESP_OK) return -PPY_ENODEV;
    int u=unit==ADC_UNIT_1?0:1;
    if(!ppy_adc_unit[u]){ int64_t r=ppy_adc_init(pin,3); if(r) return r; }
    if(adc_oneshot_read(ppy_adc_unit[u],ch,&raw)!=ESP_OK) return -PPY_EIO;
    int maxraw=(1<<SOC_ADC_RTC_MAX_BITWIDTH)-1;
    if(!uv) return (int64_t)raw*65535/maxraw;
    int mv;
    if(ppy_adc_cali[pin] && adc_cali_raw_to_voltage(ppy_adc_cali[pin],raw,&mv)==ESP_OK) return (int64_t)mv*1000;
    static const int64_t fs[4]={950000,1250000,1750000,2450000};   /* (uncalibrated: the nominal full scale) */
    return fs[ppy_adc_atten[pin]&3]*raw/maxraw;
}

/* ---------------------------------------------------------------- PWM (LEDC, low speed: every chip has it) */
typedef struct { int8_t channel, timer; uint8_t bits; uint32_t freq; } PpyPwm;
static PpyPwm ppy_pwm[64];
static uint32_t ppy_timer_freq[LEDC_TIMER_MAX]; static uint8_t ppy_timer_users[LEDC_TIMER_MAX], ppy_timer_bits[LEDC_TIMER_MAX];
static uint8_t ppy_channel_used[SOC_LEDC_CHANNEL_NUM];
static int ppy_pwm_bits(uint32_t freq){                  /* the finest duty the timer's clock allows at freq */
    int b=1; while(b<SOC_LEDC_TIMER_BIT_WIDTH && (80000000u>>(b+1))>=freq) b++;
    return b;
}
static int ppy_pwm_timer(uint32_t freq){                 /* a timer at freq: shared, or a free one configured */
    for(int t=0;t<LEDC_TIMER_MAX;t++) if(ppy_timer_users[t] && ppy_timer_freq[t]==freq){ ppy_timer_users[t]++; return t; }
    for(int t=0;t<LEDC_TIMER_MAX;t++) if(!ppy_timer_users[t]){
        int bits=ppy_pwm_bits(freq);
        ledc_timer_config_t tc={.speed_mode=LEDC_LOW_SPEED_MODE,.duty_resolution=(ledc_timer_bit_t)bits,.timer_num=(ledc_timer_t)t,.freq_hz=freq,.clk_cfg=LEDC_AUTO_CLK};
        if(ledc_timer_config(&tc)!=ESP_OK) return -1;
        ppy_timer_freq[t]=freq; ppy_timer_users[t]=1; ppy_timer_bits[t]=(uint8_t)bits; return t; }
    return -1;
}
static void ppy_pwm_release(PpyPwm *p){
    if(p->freq){ ledc_stop(LEDC_LOW_SPEED_MODE,(ledc_channel_t)p->channel,0); ppy_channel_used[p->channel]=0;
        if(!--ppy_timer_users[p->timer]) ledc_timer_rst(LEDC_LOW_SPEED_MODE,(ledc_timer_t)p->timer); }
    p->freq=0;
}
static int64_t ppy_pwm_init(uint32_t pin, uint32_t freq, uint32_t duty){
    if(!ppy_pin_ok(pin) || !GPIO_IS_VALID_OUTPUT_GPIO((int)pin)) return -PPY_ENODEV;
    if(!freq || duty>65535) return -PPY_EINVAL;
    PpyPwm *p=&ppy_pwm[pin];
    ppy_pwm_release(p);
    int ch=-1; for(int c=0;c<SOC_LEDC_CHANNEL_NUM;c++) if(!ppy_channel_used[c]){ ch=c; break; }
    if(ch<0) return -PPY_EBUSY;
    int t=ppy_pwm_timer(freq); if(t<0) return -PPY_EBUSY;
    int bits=ppy_timer_bits[t];
    ledc_channel_config_t cc={.gpio_num=(int)pin,.speed_mode=LEDC_LOW_SPEED_MODE,.channel=(ledc_channel_t)ch,.timer_sel=(ledc_timer_t)t,
                              .duty=duty>>(16-bits),.hpoint=0};
    if(ledc_channel_config(&cc)!=ESP_OK){ if(!--ppy_timer_users[t]) ledc_timer_rst(LEDC_LOW_SPEED_MODE,(ledc_timer_t)t); return -PPY_EINVAL; }
    ppy_channel_used[ch]=1; p->channel=(int8_t)ch; p->timer=(int8_t)t; p->bits=(uint8_t)bits; p->freq=freq;
    return 0;
}
static int64_t ppy_pwm_duty(uint32_t pin, uint32_t duty){
    if(pin>=64 || !ppy_pwm[pin].freq) return -PPY_ENODEV;
    if(duty>65535) return -PPY_EINVAL;
    PpyPwm *p=&ppy_pwm[pin];
    ledc_set_duty(LEDC_LOW_SPEED_MODE,(ledc_channel_t)p->channel,duty>>(16-p->bits));
    ledc_update_duty(LEDC_LOW_SPEED_MODE,(ledc_channel_t)p->channel);
    return 0;
}
static int64_t ppy_pwm_freq(uint32_t pin, uint32_t freq){
    if(pin>=64 || !ppy_pwm[pin].freq) return -PPY_ENODEV;
    uint32_t duty=(uint32_t)ledc_get_duty(LEDC_LOW_SPEED_MODE,(ledc_channel_t)ppy_pwm[pin].channel)<<(16-ppy_pwm[pin].bits);
    return ppy_pwm_init(pin,freq,duty>65535?65535:duty);
}

/* ---------------------------------------------------------------- I2C (i2c_master: a write without a stop waits for the next read: one transaction) */
#define PPY_I2C_BUSES 2
#define PPY_I2C_TIMEOUT_MS 100
static i2c_master_bus_handle_t ppy_i2c[PPY_I2C_BUSES];
static uint32_t ppy_i2c_freq[PPY_I2C_BUSES];
typedef struct { uint16_t addr; i2c_master_dev_handle_t dev; } PpyI2cDev;
static PpyI2cDev ppy_i2c_devs[PPY_I2C_BUSES][8];
static uint8_t ppy_i2c_pending[PPY_I2C_BUSES][32]; static uint32_t ppy_i2c_npending[PPY_I2C_BUSES]; static int ppy_i2c_pending_addr[PPY_I2C_BUSES];
static int64_t ppy_i2c_init(uint32_t bus, uint32_t scl, uint32_t sda, uint32_t freq){
    if(bus>=PPY_I2C_BUSES || bus>=SOC_I2C_NUM) return -PPY_ENODEV;
    if(!ppy_pin_ok(scl) || !ppy_pin_ok(sda)) return -PPY_ENODEV;
    if(ppy_i2c[bus]){ for(int i=0;i<8;i++) if(ppy_i2c_devs[bus][i].dev){ i2c_master_bus_rm_device(ppy_i2c_devs[bus][i].dev); ppy_i2c_devs[bus][i].dev=NULL; }
        i2c_del_master_bus(ppy_i2c[bus]); ppy_i2c[bus]=NULL; }
    i2c_master_bus_config_t bc={.i2c_port=(i2c_port_num_t)bus,.sda_io_num=(gpio_num_t)sda,.scl_io_num=(gpio_num_t)scl,.clk_source=I2C_CLK_SRC_DEFAULT,.glitch_ignore_cnt=7};
    bc.flags.enable_internal_pullup=1;
    if(i2c_new_master_bus(&bc,&ppy_i2c[bus])!=ESP_OK) return -PPY_EBUSY;
    ppy_i2c_freq[bus]=freq?freq:400000; ppy_i2c_npending[bus]=0; ppy_i2c_pending_addr[bus]=-1;
    return 0;
}
static i2c_master_dev_handle_t ppy_i2c_dev(uint32_t bus, uint32_t addr){
    PpyI2cDev *d=ppy_i2c_devs[bus];
    for(int i=0;i<8;i++) if(d[i].dev && d[i].addr==addr) return d[i].dev;
    int k=0; for(int i=0;i<8;i++) if(!d[i].dev){ k=i; break; }
    if(d[k].dev){ i2c_master_bus_rm_device(d[k].dev); d[k].dev=NULL; }
    i2c_device_config_t dc={.dev_addr_length=I2C_ADDR_BIT_LEN_7,.device_address=(uint16_t)addr,.scl_speed_hz=ppy_i2c_freq[bus]};
    if(i2c_master_bus_add_device(ppy_i2c[bus],&dc,&d[k].dev)!=ESP_OK) return NULL;
    d[k].addr=(uint16_t)addr;
    return d[k].dev;
}
static int64_t ppy_i2c_write(uint32_t bus, uint32_t addr, const uint8_t *b, uint32_t n, uint32_t stop){
    if(bus>=PPY_I2C_BUSES || !ppy_i2c[bus]) return -PPY_ENODEV;
    if(!stop && n<=sizeof ppy_i2c_pending[bus]){ memcpy(ppy_i2c_pending[bus],b,n); ppy_i2c_npending[bus]=n; ppy_i2c_pending_addr[bus]=(int)addr; return n; }
    i2c_master_dev_handle_t d=ppy_i2c_dev(bus,addr); if(!d) return -PPY_EIO;
    return i2c_master_transmit(d,b,n,PPY_I2C_TIMEOUT_MS)==ESP_OK ? (int64_t)n : -PPY_EIO;
}
static int64_t ppy_i2c_read(uint32_t bus, uint32_t addr, uint8_t *b, uint32_t n){
    if(bus>=PPY_I2C_BUSES || !ppy_i2c[bus]) return -PPY_ENODEV;
    i2c_master_dev_handle_t d=ppy_i2c_dev(bus,addr); if(!d) return -PPY_EIO;
    esp_err_t e;
    if(ppy_i2c_pending_addr[bus]==(int)addr && ppy_i2c_npending[bus]){   /* a register address written without a stop: a repeated start */
        e=i2c_master_transmit_receive(d,ppy_i2c_pending[bus],ppy_i2c_npending[bus],b,n,PPY_I2C_TIMEOUT_MS);
        ppy_i2c_npending[bus]=0; ppy_i2c_pending_addr[bus]=-1;
    } else e=i2c_master_receive(d,b,n,PPY_I2C_TIMEOUT_MS);
    return e==ESP_OK ? (int64_t)n : -PPY_EIO;
}
static int64_t ppy_i2c_scan(uint32_t bus, uint8_t *found){
    if(bus>=PPY_I2C_BUSES || !ppy_i2c[bus]) return -PPY_ENODEV;
    int n=0; memset(found,0,128);
    for(uint16_t a=0x08;a<0x78;a++) if(i2c_master_probe(ppy_i2c[bus],a,20)==ESP_OK){ found[a]=1; n++; }
    return n;
}

/* ---------------------------------------------------------------- SPI (bus 1: SPI2_HOST, 2: SPI3_HOST where there is one) */
static spi_device_handle_t ppy_spi[4];
static int ppy_spi_host(uint32_t bus){
    if(bus==1) return SPI2_HOST;
#if SOC_SPI_PERIPH_NUM > 2
    if(bus==2) return SPI3_HOST;
#endif
    return -1;
}
static int64_t ppy_spi_init(uint32_t bus, int32_t sck, int32_t mosi, int32_t miso, uint32_t baud, uint32_t mode){
    int host=ppy_spi_host(bus); if(host<0 || bus>=4) return -PPY_ENODEV;
    if(ppy_spi[bus]){ spi_bus_remove_device(ppy_spi[bus]); spi_bus_free((spi_host_device_t)host); ppy_spi[bus]=NULL; }
    spi_bus_config_t bc={.mosi_io_num=mosi,.miso_io_num=miso,.sclk_io_num=sck,.quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=4096};
    if(spi_bus_initialize((spi_host_device_t)host,&bc,SPI_DMA_CH_AUTO)!=ESP_OK) return -PPY_EBUSY;
    spi_device_interface_config_t dc={.mode=(uint8_t)(mode&3),.clock_speed_hz=(int)(baud?baud:1000000),.spics_io_num=-1,.queue_size=1};
    if(spi_bus_add_device((spi_host_device_t)host,&dc,&ppy_spi[bus])!=ESP_OK){ spi_bus_free((spi_host_device_t)host); return -PPY_EINVAL; }
    return 0;
}
static int64_t ppy_spi_xfer(uint32_t bus, const uint8_t *tx, uint8_t *rx, uint32_t n){
    if(bus>=4 || !ppy_spi[bus]) return -PPY_ENODEV;
    for(uint32_t done=0;done<n;){                       /* (in pieces the bus takes) */
        uint32_t k=n-done>4096?4096:n-done;
        spi_transaction_t t; memset(&t,0,sizeof t);
        t.length=8*k; t.tx_buffer=tx?tx+done:NULL; t.rx_buffer=rx?rx+done:NULL;
        if(!tx){ static uint8_t ff[64]; memset(ff,0xFF,sizeof ff); if(k>sizeof ff) k=sizeof ff; t.length=8*k; t.tx_buffer=ff; }
        if(spi_device_polling_transmit(ppy_spi[bus],&t)!=ESP_OK) return -PPY_EIO;
        done+=k;
    }
    return n;
}

/* ---------------------------------------------------------------- UART (0 is the console's) */
static uint8_t ppy_uart_on[SOC_UART_NUM];
static int64_t ppy_uart_init(uint32_t u, uint32_t baud, int32_t tx, int32_t rx){
    if(u>=SOC_UART_NUM) return -PPY_ENODEV;
    uart_config_t c={.baud_rate=(int)(baud?baud:115200),.data_bits=UART_DATA_8_BITS,.parity=UART_PARITY_DISABLE,.stop_bits=UART_STOP_BITS_1,
                     .flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
    if(!ppy_uart_on[u]){ if(uart_driver_install((uart_port_t)u,1024,0,0,NULL,0)!=ESP_OK) return -PPY_EBUSY; ppy_uart_on[u]=1; }
    if(uart_param_config((uart_port_t)u,&c)!=ESP_OK) return -PPY_EINVAL;
    if(uart_set_pin((uart_port_t)u,tx,rx,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE)!=ESP_OK) return -PPY_EINVAL;
    return 0;
}

/* ---------------------------------------------------------------- the device calls */
static int64_t ppy_port_user(const uint32_t r[7]){      /* yours: 0x7800.. (machine.call(n, ...) is 0x7800 + n) */
    (void)r;
    return -PPY_ENOSYS;
}
int64_t ppy_port_syscall(const uint32_t r[7]){
    switch(r[0]){
        case PPY_DEV_PIN_INIT:   return ppy_pin_init(r[1],r[2],r[3]);
        case PPY_DEV_PIN_WRITE:  if(!ppy_pin_ok(r[1])) return -PPY_ENODEV; gpio_set_level((gpio_num_t)r[1],r[2]!=0); return 0;
        case PPY_DEV_PIN_READ:   if(!ppy_pin_ok(r[1])) return -PPY_ENODEV; return gpio_get_level((gpio_num_t)r[1]);
        case PPY_DEV_ADC_INIT:   return ppy_adc_init(r[1],r[2]);
        case PPY_DEV_ADC_READ:   return ppy_adc_read(r[1],0);
        case PPY_DEV_ADC_READ_UV:return ppy_adc_read(r[1],1);
        case PPY_DEV_PWM_INIT:   return ppy_pwm_init(r[1],r[2],r[3]);
        case PPY_DEV_PWM_DUTY:   return ppy_pwm_duty(r[1],r[2]);
        case PPY_DEV_PWM_FREQ:   return ppy_pwm_freq(r[1],r[2]);
        case PPY_DEV_PWM_DEINIT: if(r[1]>=64) return -PPY_ENODEV; ppy_pwm_release(&ppy_pwm[r[1]]); return 0;
        case PPY_DEV_I2C_INIT:   return ppy_i2c_init(r[1],r[2],r[3],r[4]);
        case PPY_DEV_I2C_WRITE:  return ppy_i2c_write(r[1],r[2],(const uint8_t*)ppy_guest_ptr(r[3]),r[4],r[5]);
        case PPY_DEV_I2C_READ:   return ppy_i2c_read(r[1],r[2],(uint8_t*)ppy_guest_ptr(r[3]),r[4]);
        case PPY_DEV_I2C_SCAN:   return ppy_i2c_scan(r[1],(uint8_t*)ppy_guest_ptr(r[2]));
        case PPY_DEV_SPI_INIT:   return ppy_spi_init(r[1],(int32_t)r[2],(int32_t)r[3],(int32_t)r[4],r[5],r[6]);
        case PPY_DEV_SPI_XFER:   return ppy_spi_xfer(r[1],r[2]?(const uint8_t*)ppy_guest_ptr(r[2]):NULL,r[3]?(uint8_t*)ppy_guest_ptr(r[3]):NULL,r[4]);
        case PPY_DEV_UART_INIT:  return ppy_uart_init(r[1],r[2],(int32_t)r[3],(int32_t)r[4]);
        case PPY_DEV_UART_WRITE: if(r[1]>=SOC_UART_NUM || !ppy_uart_on[r[1]]) return -PPY_ENODEV; return uart_write_bytes((uart_port_t)r[1],ppy_guest_ptr(r[2]),r[3]);
        case PPY_DEV_UART_READ:  if(r[1]>=SOC_UART_NUM || !ppy_uart_on[r[1]]) return -PPY_ENODEV;
                                 { int n=uart_read_bytes((uart_port_t)r[1],ppy_guest_ptr(r[2]),r[3],pdMS_TO_TICKS(r[4])); return n<0?-PPY_EIO:n; }
        case PPY_DEV_UART_ANY:   if(r[1]>=SOC_UART_NUM || !ppy_uart_on[r[1]]) return -PPY_ENODEV;
                                 { size_t n=0; uart_get_buffered_data_len((uart_port_t)r[1],&n); return (int64_t)n; }
        case PPY_DEV_RESET:      esp_restart(); return 0;
        case PPY_DEV_FREQ:
#ifdef CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
                                 return (int64_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ*1000000;
#else
                                 return 160000000;
#endif
        case PPY_DEV_UNIQUE_ID:{ uint8_t mac[8]; uint32_t n=r[2]<6?r[2]:6; if(esp_efuse_mac_get_default(mac)!=ESP_OK) return -PPY_EIO;
                                 memcpy(ppy_guest_ptr(r[1]),mac,n); return n; }
        case PPY_DEV_DEEPSLEEP:  if(r[1]) esp_deep_sleep((uint64_t)r[1]*1000u); else esp_deep_sleep_start(); return 0;
    }
    if(r[0]>=PPY_DEV_USER && r[0]<PPY_DEV_USER+0x800) return ppy_port_user(r);
    return -PPY_ENOSYS;                                 /* (files, sockets: not in this port yet) */
}

/* ---------------------------------------------------------------- start */
void app_main(void){
    static char name[]="main", *argv[]={name,NULL};
    int status=ppy_main(1,argv);
    printf("\n[the program ended: exit status %d]\n",status);
    fflush(stdout);
#ifdef PPY_ESP32_RESTART
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
#endif
    for(;;) vTaskDelay(portMAX_DELAY);
}
