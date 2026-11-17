# Build the PAC1954 hwmon module and the board's device-tree overlays.
#   make            build both
#   sudo ./install.sh   install (DKMS + overlay + sensors config)

all: module overlay

module:
	$(MAKE) -C driver

overlay: overlay/creature-cm5-sensors.dtbo overlay/creature-cm5-audio.dtbo

overlay/%.dtbo: overlay/%.dts
	dtc -@ -I dts -O dtb -o $@ $<

clean:
	$(MAKE) -C driver clean
	rm -f overlay/*.dtbo

.PHONY: all module overlay clean
