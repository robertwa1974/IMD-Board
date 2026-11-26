# DIY insulation monitor (IMD) for EV conversions

An insulation monitoring device for DIY EV conversions. It measures the insulation resistance from
HV+ and HV- to chassis using a switched-resistor bridge, and has Bender IR155-compatible outputs
(MHS PWM + OKHS). It stacks on the contactor economizer board, or runs stand-alone.

**Status:** first-pass schematic (no PCB yet). DIY and uncertified: not a substitute for a
certified IMD in a product.

| Path | What |
|---|---|
| `HANDOFF.md` | Requirements, measurement method, economizer interface |
| `DESIGN_NOTES.md` | Part choices, calculations, PCB rules, open items |
| `hw/` | KiCad 10 schematic generated from `hw/design.py` (`imd_schematic.pdf` to view) |
| `lib/ir155_decoder/`, `src/main.cpp` | IR155 MHS decoder for an M5Dial (ESP32-S3) |
| `test/test_decode/` | Host unit tests of the decoder |

```
pio run -e m5dial          # build the decoder for the M5Dial
pio test -e native         # host tests (needs gcc/g++ on PATH)
```

MIT licensed. See `LICENSE`.
