# DIY insulation monitor (IMD) for EV conversions

An insulation monitoring device for DIY EV conversions. It measures the insulation resistance from
HV+ and HV- to chassis using a switched-resistor bridge, and has Bender IR155-compatible outputs
(MHS PWM + OKHS). It stacks on the contactor economizer board, or runs stand-alone.

**Status:** first-pass schematic (no PCB yet); firmware core written and host-tested, not yet run on hardware. DIY and uncertified: not a substitute for a
certified IMD in a product.

| Path | What |
|---|---|
| `HANDOFF.md` | Requirements, measurement method, economizer interface |
| `DESIGN_NOTES.md` | Part choices, calculations, PCB rules, open items |
| `hw/` | KiCad 10 schematic generated from `hw/design.py` (`imd_schematic.pdf` to view) |
| `src/imd/`, `lib/imd_core/` | IMD board firmware (STM32G031) and its host-testable core |
| `lib/ir155_decoder/`, `src/m5dial/` | IR155 MHS decoder for an M5Dial (ESP32-S3) |
| `test/` | Host unit tests: decoder, bridge maths, state machine vs. a simulated vehicle |

```
pio run -e imd             # IMD firmware (bench: Nucleo-G031K8, upload with -t upload)
pio run -e m5dial          # decoder for the M5Dial
pio test -e native         # host tests; needs gcc/g++ on PATH, e.g. MSYS2:
                           #   PATH=/c/msys64/mingw64/bin:$PATH pio test -e native
```

MIT licensed. See `LICENSE`.
