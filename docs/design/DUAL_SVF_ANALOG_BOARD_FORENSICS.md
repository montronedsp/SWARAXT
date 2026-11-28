# Dual SVF analog board forensics

Revision under study: **Shruthi-Analog-SVF-v03**, sheet 1, dated 02/04/2010 on the schematic title block.

Sources:

| Source | Role |
| --- | --- |
| `third_party/shruthi-1/shruthi/hardware_design/pcb/Shruthi-Analog-SVF-v03.sch` | Eagle part values |
| `third_party/shruthi-1/shruthi/hardware_design/documentation/svf_analysis.tex` | Filter-core gain law and CV scaling |
| Published schematic PDF `Shruthi-Analog-SVF-v03.pdf` | Topology. SHA-256 `fadfee70e7a934660d6f47e0799c58b20541235702de748a4845a1b46ccec016` |
| Published analysis PDF `svf_analysis.pdf` | Same filter-core note. SHA-256 `ecef582903309914b328e9ec33ea85ca4947e2acdfa156d2205bf8eb1c634df7` |
| Board assembly notes at the Mutable Instruments SVF v2 page | Build-time confirmation of C9, C20, C21, C1, C34 and the optional VCA bleed resistor |
| SSM2164 / V2164 datasheet | Gain constant −33 mV/dB |

v02 Eagle files exist in the same tree. This note does not mix v02 values into the v03 model. The analysis note’s 4.7 V Zener discussion is an earlier design remark; the v03 assembly procedure fits 3.6 V Zeners at D2–D5. Those Zeners sit on the filter integrators, not on the output VCA.

Classic IR3109 circuitry is not a donor. Its LM13700 VCA law is a different circuit.

## Provenance

The v03 schematic and PCB are Eagle 5 binary (`10 80`). They were converted with eagle-exhumer commit `a3ae95b111edb990558828e0bb9f7ec775309913`, `plugins/eagle_bin.py`, on an isolated copy. The originals were not modified. `crosscheck` reported 402 pins, 97 nets, and zero schematic-only, board-only, renamed, split or merged nets. The XML copies and the extract script stay on `Z:\SWARAXT-DC-AUDIT-builds\dual-v2164-ref`. The published PDF has no text layer; pin connectivity below is the XML, not a rendering of the PDF.

MODE pins IC7E and IC9E are single-pin nets `N$68` and `N$71`. Nothing else is attached, so the cells run in the datasheet’s open-MODE class AB. That does not change the ideal current ratio.

## What the software implements

`DualSvfAnalogBoard` is the one VCA used by `ShruthiSvfDual` for section 1, serial and parallel. The SVF integrators are unchanged. The obsolete `10000 * 33 nF` VCA smoother is gone.

- C34/R20 into the inverting mixer, feedback R13 with C13.
- C20/R31 and C21/R32 as independent coupling branches. A mux branch that is not selected is driven from 0 V so its capacitor discharges.
- One shared control voltage from the loaded C9 network and the IC3A/IC7C servo, with schematic R53 fitted.
- IC9D current into the inverting I/V (R12, C14). R1 and R2 stay the assembly jumpers: the signal passes, and the plugin load is high-Z, so R3 does not change the voltage.

## Audio path established from v03

```text
Digital mix ---- C34 4.7 µF ----+
                                 +-- IC2A input summer -- MIX
External input - C1 4.7 µF -----+
                                      |
                                      v
                               SVF cores (existing math)
                                      |
                    C20 1 µF (filter 1)   C21 1 µF (filter 2)
                                      |
                                      v
                               IC9D audio V2164
                                      |
                                      v
                         IC2B I/V  (R12 33 kΩ, C14 100 pF)
                                      |
                                      v
                              R3 220 Ω -- output jack
```

R1 and R2 are the input and output gain positions. The assembly procedure shorts them with jumpers. The software model treats those jumpers as the nominal board and does not add panel gain controls.

There is **no output coupling capacitor** after IC2B. C20 and C21 are the filter-output coupling capacitors, before the VCA.

## V2164 law

Datasheet gain constant: **−33 mV/dB**.

\[
G(V_c) = 10^{-V_c / 0.66}
\]

The filter-core note writes the same law as \(10^{-(3/2) V_c}\), because \(1/0.66 \approx 3/2\).

IC7C is the reference cell in the feedback of IC3A. IC9D is the audio cell and shares that control voltage. For matched cells the exponential cancels at equilibrium:

\[
\frac{I_{\text{audio,out}}}{I_{\text{audio,in}}} = \frac{I_{\text{control}}}{I_{\text{ref,in}}}
\]

The audio VCA is therefore a **linear current ratio** set by the control servo, not an open-loop exponential applied to the PWM voltage. C10 (100 pF) is the local compensation around IC3A. It sets servo dynamics; it is not an extra audio high-pass.

R53 is a placed 2.2 MΩ from VEE to the servo summing node (`R53.1` on VEE, `R53.2` on `N$11`). The assembly notes describe a similar ~2 MΩ on pads marked “opt” as a builder tweak that closes the VCA when the PWM sits a few millivolts above ground. Both are the same offset term. The nominal model fits the schematic part. Leaving it off removes the offset and is not a separate DSP path.

## Component table (audio and VCA path)

Confidence: **schematic** means the v03 Eagle value field. **datasheet** means the V2164/SSM2164 electrical table. **assembly** means the published build notes agree. Topology details that still need a traced net are marked **partial**.

| Designator | Value | Role | Confidence |
| --- | --- | --- | --- |
| C34 | 4.7 µF | Digital-mix input coupling | schematic + assembly |
| C1 | 4.7 µF | External input coupling. Software source is zero | schematic + assembly |
| R1, R2 | jumper | Nominal input/output gain | assembly |
| IC2A | TL072 | Inverting input summer. `+IN` is GND | schematic + netlist |
| R20 | 100 kΩ | Digital-mix series resistor into the summer | netlist `N$39` to `N$6` |
| R13 | 33 kΩ | Summer feedback, with C13 100 pF | netlist `N$6` to MIX |
| R17 | 10 kΩ | External input into the same summer. Software source is 0 V | netlist |
| C20 | 1 µF | Filter-1 coupling. `+` is FILTER1_MIX (IC13.Z), `−` is R31 | netlist |
| C21 | 1 µF | Filter-2 coupling. `+` is FILTER2_OUT (IC12.Y), `−` is R32 | netlist |
| R31, R32 | 33 kΩ | Both audio-cell input branches, summed at IC9D.IN | netlist `N$67` |
| R24 | 100 kΩ | Reference-cell input from VEE to IC7C.IN. Not an audio branch | netlist `N$10` |
| R14 | 100 kΩ | PWM source into C9 and R9 | netlist `N$45`, `N$22` |
| C9 | 6.8 nF | PWM capacitor to GND | netlist |
| R9 | 100 kΩ | From the C9 node into the servo summer | netlist `N$22` to `N$11` |
| R53 | 2.2 MΩ | VCA bleed into the control summer | schematic; kit treats a 2 MΩ part as optional |
| C10 | 100 pF | Control-op-amp compensation | schematic + assembly |
| IC3A | TL072 | Control servo | schematic |
| IC7C | V2164 | Reference gain cell | schematic |
| IC9D | V2164 | Audio gain cell | schematic |
| R23, R33 | 470 Ω | Gain-cell input stabilization | schematic |
| C25, C29 | 560 pF | Gain-cell input HF shunt | schematic + assembly |
| R12 | 33 kΩ | Output I/V feedback | schematic |
| C14 | 100 pF | Output I/V HF feedback | schematic + assembly |
| IC2B | TL072 | Output amplifier | schematic |
| R3 | 220 Ω | Series output build-out | schematic |
| IC12, IC13 | 4053 | LP/BP/HP and serial/parallel routing | schematic |

Filter capacitors C33, C35, C39, C46 are 220 pF. They belong to the existing SVF cores. This work does not retune those cores.

D2–D5 are the integrator clippers, not part of the output VCA. The sheet text says “3.6V Zener”. D4’s anode is on BP1 and D5’s anode is on the other side of C39; their cathodes meet on `N$55`. D2/D3 do the same across C33 (`N$33`). Each pair is anti-series, so the clamp is the zener rating plus one silicon forward drop, 4.3 V. The knee is linear through 3.6 V and then approaches 4.3 V.

## Filter calibration

There is one production model. The Paper / Eagle selector is gone. Bit 7 of the session word used to store that choice. Unpack ignores it, and pack leaves it clear, so older session words still decode routing, modes and Filter 2 controls.

| Quantity | v03 part | Production value |
| --- | --- | --- |
| Integrator R, C | 33 kΩ, 220 pF | paper law, parts match |
| Rq | R44, R28, 15 kΩ | paper law |
| Ro | R45, R29, 330 kΩ | schematic. The paper’s 220 kΩ is not fitted |
| Resonance smoother | R21/R8 22 kΩ, C22/C8 68 nF, CV pin 5 kΩ | τ = C·(22 kΩ \|\| 5 kΩ). DC attenuation ρ = 5/27 |
| Cutoff smoother | R10 10 kΩ \|\| C11 33 nF | τ = 330 µs. Trimmers R4–R7 set the 2.141 V / 128-note span |
| Self-oscillation | Q singularity | 4.83 V PWM, byte 246, panel 62 |
| Limiter | anti-series 3.6 V Zeners | asymptote 4.3 V |

The paper’s Q and cutoff equations are unchanged in form. Ro and the limiter are the schematic’s. The historical Paper sound is not a compatibility mode.

## Routing

IC12 and IC13 (4053) select LP/BP/HP. C20 follows IC13.Z and C21 follows IC12.Y, so each coupling capacitor is after its mux common. The two series resistors are both 33 kΩ. R24 is the reference resistor and is not in the audio sum. Firmware routing is unchanged: serial sends only the filter-2 common into the VCA, parallel sends both, and section 1 is the single-section listening path on the filter-1 branch. The unused branch is held at 0 V.

IC13.Z1 is GND, so the filter-1 mux can also ground C20. The software’s serial path is that disconnected case: filter 1 is not mixed into the VCA.

## Closed-loop equations

R14 = R9 = 100 kΩ and C9 = 6.8 nF. The loaded PWM node is a one-pole toward `Vpwm / 2` with time constant `C9 * (R14 || R9)` = 340 µs.

The reference input current magnitude is `5 V / R24` = 50 µA. R53 contributes `5 V / 2.2 MΩ` out of the summing node toward VEE. With the audio cell matched to the reference cell and IC2B inverting:

\[
g = \max\left(0,\ \frac{V_f}{5\ \mathrm{V}} - \frac{R_{24}}{R_{53}}\right)
\]

\[
V_{\mathrm{out}} = -g \left(v_{C20} + v_{C21}\right)
\]

`R24/R53` = 1/22. At zero PWM the formula is negative, so the gain is the closed cell, which is what the bleed is for. Without R53 the offset term is zero and `g = Vf / 5`. Full-scale PWM (5 V) gives `g = 0.5 - 1/22` with R53 fitted, and `g = 0.5` without it. Bytes 0 through 23 stay closed; byte 24 is the first positive gain.

The digital input, into a virtual ground, is an inverting high-pass at `1/(2π R20 C34)` ≈ 0.339 Hz with passband gain `-R13/R20` = −0.33, then the C13/R13 pole near 48 kHz. Each coupling branch is a high-pass at `1/(2π · 33 kΩ · 1 µF)` ≈ 4.82 Hz. C14/R12 is the matching pole on the I/V. No capacitor follows IC2B.

The host 0.7 Hz blocker stays on Dual. Coupling before a time-varying gain does not by itself remove a mean.
