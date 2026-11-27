The color fix now lives in the display init sequence instead of in the game's colors. I've moved the whole display setup into src/display_4848s040.h. It builds, but I haven't seen it on the screen, so please flash and check.

Why it was inverted: the manufacturer's init sequence for this board has its "invert colors" command (0x21) commented out, so the correct setting is no inversion. Setting IPS = true turned inversion on. Setting IPS = false didn't fix it because the board has no reset line for the LCD controller. Flashing resets the ESP32 but not the panel, so the panel kept the inversion an earlier firmware had turned on, and nothing ever turned it off.

What changed:

Clean start every boot: the init sequence now begins with a software reset (0x01), so the panel no longer depends on what the last firmware left behind.
Inversion off: it sends "inversion off" (0x20) explicitly.
IPS = false: the library never turns inversion back on.
Workaround removed: all colors are plain RGB565 again.
To use it in your other projects: copy display_4848s040.h into src/. Then replace the init array and the bus/gfx definitions with #include "display_4848s040.h". The include must come before touch.h, which uses gfx. It works with LVGL and with direct framebuffer projects; only the init and constructor change. If you'd rather edit each project by hand instead, it's three changes:

Add BEGIN_WRITE, WRITE_COMMAND_8, 0x01, END_WRITE, DELAY, 120, at the start of the init array.
Add WRITE_COMMAND_8, 0x20, just before the 0x3A line.
Change true /* IPS */ to false.
If colors still look wrong after flashing, unplug the board for a few seconds and try again. If that fixes it, the software reset isn't being applied and I'll need to look again.