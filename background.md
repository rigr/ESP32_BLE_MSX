## Technical background

I used a oscilloscope to understand the strobe signal sent from the Roland S-750.

Every three milliseconds there is a signal:

![osci1](/pix/RolandS750strobe01.png)

Well, in fact it's not just a spike

![osci2](/pix/RolandS750strobe02.png)

it's two spikes

![osci3](/pix/RolandS750strobe03.png)
![osci4](/pix/RolandS750strobe04.png)
![osci5](/pix/RolandS750strobe05.png)

and they have different length

![osci6](/pix/RolandS750strobe06.png)

the first one about 75 microsends

![osci7](/pix/RolandS750strobe07.png)

then there is LOW for about 35 microseconds and again HIGH for another about 35 microseconds

![osci8](/pix/RolandS750strobe08.png)

It was tricky to teach the esp32 this language. The MU-1-mouse, that in fact is a MSX-mouse, was able to talk like that.

## Pulseview files of mouse movement

If interested - here some sequences of a real MU-1-mouse I recorded using pulseview - moving the x- and y-axis separatly:

[Roland Mouse x-axis left](./pulse/romaux--.sr)

[Roland Mouse x-axis left](./pulse/romaux++.sr)

[Roland Mouse y-axis down](./pulse/romauy--2.sr)

[Roland Mouse y-axis down](./pulse/romauy--2a.sr)

[Roland Mouse y-axis up](./pulse/romauy++1.sr)

[Roland Mouse y-axis up](./pulse/romauy++2.sr)


## Pinout and electrical limitations:

I use pins that are all on one side of the board and had to avoid using D12, because when NOT Low at boot-time, the chip fails.

Power comes from the sampler.

Even though the board should NOT be used with connections higher than 3,3 V it works: the Rolans sampler uses high R pullups, so there is almost no current through the pins. For me it works. :)


### General info concerning the pins:

Internal SPI Flash Pins (DO NOT USE): GPIOs 6, 7, 8, 9, 10, and 11 are connected to the internal flash memory. Using these will cause the ESP32 to crash.

Strapping Pins (Use with Caution):
GPIO 0: Must be HIGH to run, LOW to flash.
GPIO 2: Must be LOW or floating to boot.
GPIO 5: Affects boot mode.
GPIO 12: Must be LOW at boot; high pulls flash voltage to 1.8V, causing fails.
GPIO 15: Affects boot mode.

Input Only Pins (34-39): GPIOs 34, 35, 36, and 39 are input-only. They do not have pull-up or pull-down resistors and cannot output voltage.
UART Pins: GPIO 1 (TX) and GPIO 3 (RX) are used for flashing/serial debugging, often interfering with external hardware.

Voltage Restriction: All ESP32 GPIOs are 3.3V, not 5V tolerant.
