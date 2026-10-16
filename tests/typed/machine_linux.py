# lib/machine.py over the device calls: on a computer (the interpreter, the
# macos target, the c target's posix port) the simulated board of
# targets/posix/devsim.c answers them; an i386 Linux program has no such
# calls (expected/machine_linux.linux.out: OSError ENOSYS).
from machine import Pin, ADC, PWM, I2C, SPI, UART, freq, unique_id, ticks_ms, ticks_diff, sleep_ms

led = Pin(2, Pin.OUT, value=1)
print(led, led.value())
led.off()
led.toggle()
print("toggled:", led.value())
button = Pin(0, Pin.IN, Pin.PULL_UP)
print("button:", button.value(), Pin(4, Pin.IN).value())

pwm = PWM(Pin(5), freq=500, duty_u16=1000)
print(pwm.freq(), pwm.duty_u16(30000), pwm.freq(2000))
pwm.deinit()

adc = ADC(Pin(34))
print("adc:", adc.read_u16(), adc.read_uv())
adc.atten(ADC.ATTN_0DB)
print("adc 0 dB:", adc.read_uv())

i2c = I2C(0, scl=Pin(22), sda=Pin(21), freq=100000)
print(i2c.scan(), i2c.writeto(0x3C, b"\x00\xaf"))
print(i2c.readfrom_mem(0x68, 0x75, 2))

spi = SPI(1, baudrate=2000000, sck=Pin(18), mosi=Pin(23), miso=Pin(19))
print(spi.write_read(b"\x9f\x01\x02"), spi.read(2))
spi.write(b"hi")

uart = UART(1, 9600, tx=Pin(17), rx=Pin(16))
print(uart.write(b"AT\r\n"), uart.any(), uart.read(4, 10))

print(freq(), unique_id())
t = ticks_ms()
sleep_ms(20)
print("slept:", ticks_diff(ticks_ms(), t) >= 20)
try:
    Pin(99, Pin.OUT)
except OSError as e:
    print("OSError", e.errno, e.strerror)
