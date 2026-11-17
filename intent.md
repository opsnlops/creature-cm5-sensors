# Creature CM5 Sensor Reader

We've built our own CM5 carrier board for use with April's Creature Workshop's creatures.

This board has a PAC1954 on it, just like how our creature controller boards do. It's the
same chip. This CM5 has has i2c enabled and a device is showing up on 0x10.

We need some way to get that sensor data into a place where we can find it. How that should
work I'm not sure about yet (either we poll it directly from creature-controller, or we use
the lmsensors built into Linux to read it and make it available system-wide).


## Reference Material

* The schematic for the CM5 you are running on: ~/code/creature-cm5-board
* The Creature Controller code so you can see how we do it from the controller firmware:
  ~/code/creature-controller. (The firmware is in the firmware/ directory.)
