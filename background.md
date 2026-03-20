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
