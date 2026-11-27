# Planned generic parts: loads, sources, interference

This is the plan for the generic part catalogue beyond the first 24 parts.
Every entry becomes a `generic_*` part file that follows [SPEC.md](../SPEC.md).
Tasks: LIB-05 to LIB-08 in [docs/TASKS.md](../../docs/TASKS.md).

## Principles

- **Generic, not vendor parts.** A part describes a kind of device
  (`generic_battery_li_ion`), not a product. Real products may appear in
  `compatible`.
- **Any number of instances.** A part is a definition; a board or a project can
  use it as often as needed, each instance with its own parameters.
- **Two ways to attach.** As a symbol on the schematic (on-board part), or as
  an off-board device attached to connector pins in the project sidecar file
  `<project>.mspice.toml` (fan on J3, battery on J1, motor on J5). Off-board
  devices never touch the KiCad files.
- **Every parameter adjustable** within its declared range, per instance, and
  sweepable for variants and Monte Carlo.
- **Every driving quantity has a stimulus mode** (see below), so a solar panel
  can see a constant irradiance, a step list or a whole day profile.
- **Switchable detail** through `features`: for example `thermal`, `aging`,
  `saturation`, `inrush`, `regeneration`.

## Stimulus modes for driving quantities

Driving quantities are inputs such as irradiance, wind speed, load torque,
ambient temperature, duty cycle, state of charge at start.

| Mode | Example |
|---|---|
| `constant` | irradiance = 800 W/m2 |
| `steps` | torque 0.1 N m for 2 s, then 0.4 N m |
| `ramp` | wind 3 to 12 m/s in 10 s |
| `pwl` / `curve` | CSV file with time and value |
| `sine` | ripple on a supply, mains-frequency load |
| `profile` | named profile shipped with the library: day irradiance, gusty wind, urban e-scooter drive cycle |
| `random` | band-limited noise with seed, for robustness runs |

## Loads

| Part id | Model | Key parameters | Features |
|---|---|---|---|
| `generic_fan_4pin` (exists) | current vs. duty curve, tach output, PWM input | rated voltage, current, rpm | inrush |
| `generic_fan_2pin`, `generic_fan_3pin` | DC fan, with and without tach | voltage, current | inrush |
| `generic_led` (exists), `generic_led_strip` (exists), `generic_rgb_led_addressable` (exists) | diode curve, per-metre current, protocol state machine | Vf, If, length, segments | thermal, voltage drop |
| `generic_lamp_incandescent` | tungsten resistance vs. filament temperature, thermal time constant | rated voltage and power, cold/hot ratio | inrush (always on) |
| `generic_pump_centrifugal` | DC or BLDC motor plus load torque proportional to speed squared | flow at rated speed, head, motor data | dry-run |
| `generic_motor_dc` (exists) | R, L, kE/kT, inertia, load torque | | thermal |
| `generic_motor_bldc` (exists) | trapezoidal back-EMF, three phases | Kv, phase R/L, poles | |
| `generic_motor_hub_bldc` | e-scooter / e-bike hub motor, motoring and braking | rated power and voltage, wheel diameter, Kv | regeneration, thermal |
| `generic_motor_stepper` (exists) | two-phase hybrid stepper | step angle, phase R/L, holding torque | |
| `generic_motor_pmsm` | synchronous motor (permanent magnet), dq model | Ld, Lq, flux, poles | saturation |
| `generic_motor_induction` | asynchronous motor, dq model; steady-state equivalent circuit for fast runs | rated power, voltage, frequency, slip, Rs, Rr, Lm | thermal |
| `generic_solenoid`, `generic_relay_coil` | coil with plunger force, release time | R, L, pull-in/drop-out voltage | |
| `generic_heater_resistive` | resistance with temperature coefficient and thermal mass | power, TCR | thermal |
| `generic_load_custom` (exists) | fixed value, curve, formula or state machine | | |

## Sources and storage

| Part id | Model | Key parameters | Features |
|---|---|---|---|
| `generic_battery_li_ion` (chemistry presets NMC, NCA, LFP), `generic_battery_lipo` | open-circuit voltage vs. state of charge and temperature, R0 plus two RC pairs, capacity, cell count series/parallel | capacity, cells, chemistry, SoC at start, temperature | aging, thermal, protection limits |
| `generic_battery_lead_acid` | same structure, lead-acid curves, Peukert effect | capacity, cells | aging |
| `generic_battery_nimh` | NiMH curves | capacity, cells | |
| `generic_battery_primary` (alkaline, lithium coin) | non-rechargeable, internal resistance rising with discharge | capacity, type | |
| `generic_supercapacitor` | capacitance with ESR and leakage, two-branch model | C, ESR, rated voltage | |
| `generic_solar_panel` | single-diode model with irradiance and temperature, cells in series | Voc, Isc, Vmp, Imp, cells, temperature coefficients | shading (per substring), bypass diodes |
| `generic_wind_turbine` | power coefficient vs. tip-speed ratio, rotor inertia, permanent-magnet generator, optional rectifier | rotor diameter, rated power and wind speed, generator Kv | stall, rectifier |
| `generic_generator_hub_bldc` | hub motor used as a generator (regenerative braking, bicycle dynamo) | as hub motor | |
| `generic_mains_supply` | sine source with impedance, sags and swells | voltage, frequency, source impedance | flicker, harmonics |
| `generic_power_supply_lab` | constant voltage / constant current with transitions | V, I limit, slew | |

## Interference and fields

| Part id | Model | Key parameters |
|---|---|---|
| `generic_noise_ripple` (exists), `generic_noise_burst` (exists) | conducted noise on a node | amplitude, frequency, repetition |
| `generic_noise_surge` | combination-wave shaped surge (1.2/50 us voltage, 8/20 us current shape) | peak, source impedance |
| `generic_noise_esd` | human-body-model shaped discharge | voltage, discharge network |
| `generic_noise_eft` | fast transient burst | amplitude, burst frequency |
| `generic_field_wire` | external current-carrying wire near the board; induces voltage into board loops through mutual inductance (needs INT-04) | current waveform, position, direction |
| `generic_field_coil` | external coil or motor stray field as a magnetic dipole | moment waveform, position, orientation |
| `generic_field_uniform` | uniform alternating magnetic field (mains hum test) | flux density, frequency, orientation |

Standards are only named as the source of the waveform shape; their text is
never copied.

## Acceptance (per part)

- Validates with `mspice-libcheck check --deny-warnings`.
- `quality = "draft"` until values are checked against a cited source; then
  `datasheet` or `measured`.
- At least one reference test per stimulus mode the part supports, simulated
  by the engine once LIB-01 is done.
- Negative tests: out-of-range parameters, missing profile file, a feature
  that requires another feature.
