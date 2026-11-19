# creature-cm5-sensors

Linux hwmon support for the sensors on the Creature CM5 Carrier board, so
`sensors`, `/sys/class/hwmon` and anything that reads hwmon (Prometheus
`node_exporter`, creature-controller) can see board power and temperature.

| Address (I2C1) | Chip | Driver |
|---|---|---|
| 0x10 | PAC1954 power monitor | `driver/pac1954.c` (this repo, out-of-tree, DKMS) |
| 0x18 | MCP9808 temperature | in-tree `jc42` |

Both are declared in one device-tree overlay, `overlay/creature-cm5-sensors.dts`.

The repo also builds **`creature-cm5-audio`**, for the board's audio outputs
(see [Audio](#audio)), and **`creature-cm5-revb-workarounds`**, for rev B
boards only (see [Rev B workarounds](#rev-b-workarounds)).

## PAC1954 channels

All shunts are 4 mΩ (see `~/code/creature-cm5-board/schematic-design.md`).

| sysfs | Label | What it measures |
|---|---|---|
| in1 / curr1 / power1 | Board In | SYS_24V, total board input (after the ORing diodes) |
| in2 / curr2 / power2 | CM5 5V | 5.1 V rail into the CM5 |
| in3 / curr3 / power3 | Character | USB-C character power |
| in4 / curr4 / power4 | PoE | AG5810 output only (PoE's share of the input) |

Units are the hwmon standard: `in` mV, `curr` mA, `power` µW.
`*_input` is the latest sample; `*_average` is the chip's rolling
average of 8 samples. Readings are cached for 100 ms.

## Package

The sensors ship as one Debian package, `creature-cm5-sensors` (arch: all).
Install it and you're done. No reboot or manual steps.

```sh
./make_packages.sh        # builds both .debs into dist/ + lintian report
sudo apt install ./dist/creature-cm5-sensors_<ver>_all.deb
sensors
```

Build on trixie. It needs `debhelper`, `dh-dkms` and `device-tree-compiler`.

What installing does:

- **Driver:** the source goes to `/usr/src/pac1954-<ver>/`. DKMS builds it
  for every installed kernel that has headers. The package depends on
  `linux-headers-rpi-2712`, so every kernel upgrade brings new headers, and
  DKMS's `header_postinst.d` hook rebuilds the module for that kernel.
- **Overlay:** `creature-cm5-sensors.dtbo` is copied to
  `/boot/firmware/overlays/`. The Pi kernel hook syncs overlays with
  `rsync` and no `--delete`, so ours survives kernel upgrades. A marked
  block in `/boot/firmware/config.txt` enables it at boot.
- **Live start:** the overlay is also applied to the running system
  (`dtoverlay`), so the sensors appear immediately. On upgrade, a newer
  module replaces the loaded one.
- **MCP9808 limits:** a udev rule sets min/max/crit when the jc42 device
  appears (`/usr/lib/udev/rules.d/60-creature-cm5-sensors.rules`).
- **Labels:** `/etc/sensors.d/creature-cm5-sensors.conf` (conffile).

Removing the package deletes the `config.txt` block and the overlay,
removes the DKMS builds and unloads the module.

`man 4 pac1954` documents the driver, its sysfs attributes and the files.

### Development without packaging

```sh
make                                   # module + overlay in the tree
sudo insmod driver/pac1954.ko
sudo dtoverlay -d overlay creature-cm5-sensors
```

Don't mix this with an installed package on the same host.

## Notes

- **The driver reconfigures the chip at probe.** At power-on the PAC1954's
  SLOW/ALERT1 pin is a SLOW input. This board pulls it up (`PAC_!ALERT`),
  which drops the chip to 8 samples/s. The driver sets the pin to ALERT
  (alerts disabled), so it runs at 1024 SPS.
- All channels use the unipolar ranges: 0–32 V, 0–25 A (100 mV across 4 mΩ),
  0–800 W.
- At light load the current readings carry a fixed offset of a few tens of mA
  (Character reads ~25 mA with nothing attached). That's about 0.1% of full
  scale, so it only matters at very low currents.
- Once loaded, the driver owns 0x10, so `i2cget`/`i2ctransfer` need `-f`.
  Only read with them: a REFRESH from userspace interferes with the
  driver's readings.
- The MCP9808's limit registers power up at 0 °C, which `sensors` shows as
  ALARM. The package's udev rule sets them; override it by copying the rule
  to `/etc/udev/rules.d/` under the same name.

## Audio

`creature-cm5-audio` drives the PCM5122 DAC (0x4D on I2C1, RP1 I2S0 on
GPIO18/19/21) behind the RCA line out, using only generic in-kernel drivers:
`simple-audio-card`, `pcm512x` and `designware-i2s`
(`overlay/creature-cm5-audio.dts`). The DAC's SCK is grounded, so the Pi
drives BCLK/LRCLK and the DAC's PLL runs from BCLK.

| ALSA ID | Display name | Output |
|---|---|---|
| `CreatureAnalog` | Creature Analog Audio | RCA line out (white = left, red = right) |
| `CreatureDigital` | vc4-hdmi-0 | HDMI connector |

The IDs come from a udev rule (`/usr/lib/udev/rules.d/60-creature-cm5-audio.rules`),
so `hw:CreatureAnalog` and `hw:CreatureDigital` work whatever order the cards
register in. HDMI's display name is fixed in the vc4 driver. HDMI1 isn't
wired on this board, but its card (`vc4hdmi1`) still appears.

```sh
speaker-test -D hw:CreatureAnalog -c 2 -t wav -l 1
```

Installing replaces `dtoverlay=allo-boss-dac-pcm512x-audio,slave`: postinst
comments that line out and postrm restores it. Both overlays claim 0x4D,
so when switching from Allo the new card appears after the next reboot.
On a board with neither enabled, the overlay is applied live.

Unlike the Allo driver, the generic card doesn't cap the **Digital** volume
at 0 dB. The control goes up to +24 dB, which clips. The DAC powers up at
0 dB, so leave it there or lower.

## Rev B workarounds

`creature-cm5-revb-workarounds` is for **rev B boards only**. It installs
`creature-cm5-revb-workarounds.service`, a oneshot that runs at boot.

| Bug | Workaround |
|---|---|
| The character USB-C port can't deliver 12 V. The TPS25750 sources only 5 V, because its high-voltage path (PPHV) is sink-only, but the EEPROM also offers 12 V. A sink that asks for 12 V (most phones do) loops through hard resets, and CHAR ON blinks. | `pd-5v-only` waits for the TPS25750 (0x21) to reach APP mode, then rewrites its TX Source Caps (0x32) to advertise 5 V / 3 A only, and re-sends them to a sink that's already attached. |

What to know about the 5 V-only change:

- **Where it lives:** it's written to the TPS25750's RAM. The EEPROM is never touched.
- **How long it lasts:** it survives a Pi reboot, because the PD controller stays powered. It's lost when the board loses power, and the service puts it back at the next boot.
- **Safety check:** the script makes no change unless PDO1 is a fixed 5 V supply.
- **Removing the package:** the 12 V offer comes back after the next power cycle, and so do the hard resets.
- **Permanent fix:** an EEPROM image that offers 5 V only. With that image, this service has nothing to do.

The service's output is in the journal:

```sh
journalctl -u creature-cm5-revb-workarounds
```

