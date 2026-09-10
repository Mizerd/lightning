#!/usr/bin/env python3
"""Emit REL_WHEEL through a virtual uinput mouse.

ydotool 1.0.4 has no wheel command at all — its `click` accepts buttons
0x00-0x07 and wheel is REL_WHEEL, not a button. /dev/uinput is writable here
via an ACL, so this creates its own device instead.

  wheel.py <clicks>       positive = scroll UP, negative = scroll DOWN
  wheel.py <clicks> <ms>  delay between clicks
"""
import ctypes, fcntl, os, struct, sys, time

UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_RELBIT = 0x40045564, 0x40045565, 0x40045566
UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502
EV_SYN, EV_KEY, EV_REL = 0, 1, 2
REL_X, REL_Y, REL_WHEEL, REL_WHEEL_HI_RES = 0, 1, 8, 0x0b
BTN_LEFT, SYN_REPORT = 0x110, 0

fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
# A device libinput will accept as a MOUSE: relative x/y and a button, not
# just a wheel axis. A wheel-only device is ignored by the seat.
fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
fcntl.ioctl(fd, UI_SET_KEYBIT, BTN_LEFT)
fcntl.ioctl(fd, UI_SET_EVBIT, EV_REL)
for axis in (REL_X, REL_Y, REL_WHEEL, REL_WHEEL_HI_RES):
    fcntl.ioctl(fd, UI_SET_RELBIT, axis)
fcntl.ioctl(fd, UI_SET_EVBIT, EV_SYN)

# struct uinput_user_dev: name[80], input_id{bustype,vendor,product,version},
# ff_effects_max, then 4 x int[64] abs arrays.
name = b"lightning-test-wheel".ljust(80, b"\0")
dev = name + struct.pack("HHHH", 0x03, 0x1234, 0x5678, 1) + struct.pack("i", 0)
dev += b"\0" * (4 * 64 * 4)
os.write(fd, dev)
fcntl.ioctl(fd, UI_DEV_CREATE)
time.sleep(0.4)   # let the compositor bind the new seat device

def emit(etype, code, value):
    # struct input_event { struct timeval time; __u16 type, code; __s32 value; }
    os.write(fd, struct.pack("llHHi", 0, 0, etype, code, value))

clicks = int(sys.argv[1]) if len(sys.argv) > 1 else -3
delay = (int(sys.argv[2]) if len(sys.argv) > 2 else 60) / 1000.0
step = 1 if clicks > 0 else -1
for _ in range(abs(clicks)):
    emit(EV_REL, REL_WHEEL, step)
    # HI_RES too: 120 units per detent. Modern libinput prefers it, and a
    # client that reads only the hi-res axis sees nothing without it.
    emit(EV_REL, REL_WHEEL_HI_RES, step * 120)
    emit(EV_SYN, SYN_REPORT, 0)
    time.sleep(delay)

time.sleep(0.25)
fcntl.ioctl(fd, UI_DEV_DESTROY)
os.close(fd)
print(f"emitted {clicks} wheel click(s)")
