# PWM, the ADC and an I2C scan: the parts of a small controller.
#
#   plainpy --compile --target esp32 examples/esp32/sensors.py -o sensors
from machine import Pin, PWM, ADC, I2C, sleep_ms

pwm = PWM(Pin(5), freq=1000, duty_u16=0)          # an LED on GPIO 5 fades in
for d in range(0, 65536, 16384):
    pwm.duty_u16(d)
    sleep_ms(100)
pwm.deinit()

adc = ADC(Pin(34), atten=ADC.ATTN_11DB)           # GPIO 34: an input-only pin with ADC1
raw = adc.read_u16()
print("ADC: %d of 65535, %d mV" % (raw, adc.read_uv() // 1000))

i2c = I2C(0, scl=Pin(22), sda=Pin(21), freq=400000)
found = i2c.scan()
print("I2C:", ", ".join(hex(a) for a in found) if found else "no devices")
