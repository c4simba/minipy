# The LED of an ESP32 board blinks, and the board says so.
#
#   plainpy --compile --target esp32 examples/esp32/blink.py -o blink
#   cd blink && idf.py flash monitor
#
# On a computer (plainpy examples/esp32/blink.py) a simulated board prints
# what the program does with its pins.
from machine import Pin, sleep_ms, ticks_ms, ticks_diff, unique_id

led = Pin(2, Pin.OUT)            # GPIO 2: the LED of many boards (an S3 DevKit's is a WS2812 on 48)
start = ticks_ms()
for i in range(6):
    led.toggle()
    sleep_ms(250)
print("blinked 3 times in", ticks_diff(ticks_ms(), start) // 100 * 100, "ms")
print("board", unique_id().hex(":"))
