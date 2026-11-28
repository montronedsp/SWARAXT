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

## What the current software does

`ShruthiSvf2p` and `ShruthiSvfDual` still own a prototype VCA:

- PWM smoothing time constant `10000 * 33 nF`.
- Linear multiply of the selected filter sample by that smoothed gain.
- Separate VCA state on the single-filter path and again on the serial/parallel path.
- No input coupling network and no C20/C21 output coupling.

That time constant is not the v03 VCA network. v03 uses **C9 = 6.8 nF** on the VCA control path.

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

R53 is **2.2 MΩ** from the negative rail into the control summing node on the v03 sheet. The assembly notes describe a 2 MΩ bleed resistor on pads marked “opt” as a builder modification that closes the VCA harder and slightly hardens the envelope. Those two descriptions are the same function with a small value difference. The nominal model decision (fitted 2.2 MΩ versus omitted bleed) stays open until the static closed-VCA gain is computed both ways. It is not a user control.

## Component table (audio and VCA path)

Confidence: **schematic** means the v03 Eagle value field. **datasheet** means the V2164/SSM2164 electrical table. **assembly** means the published build notes agree. Topology details that still need a traced net are marked **partial**.

| Designator | Value | Role | Confidence |
| --- | --- | --- | --- |
| C34 | 4.7 µF | Digital-mix input coupling | schematic + assembly |
| C1 | 4.7 µF | External input coupling. Software source is zero | schematic + assembly |
| R1, R2 | jumper | Nominal input/output gain | assembly |
| IC2A | TL072 | Input summer | schematic, gain resistors partial |
| C20 | 1 µF | Filter-1 output coupling into the VCA | schematic + assembly |
| C21 | 1 µF | Filter-2 output coupling into the VCA | schematic + assembly |
| R31 | 33 kΩ | Series resistor on one VCA input leg | schematic, which leg is partial |
| R24 | 100 kΩ | Series resistor on the other VCA input leg | schematic, which leg is partial |
| C9 | 6.8 nF | VCA PWM smoothing | schematic + assembly |
| R9, R14 | 100 kΩ | Control-servo resistors | schematic, exact nodes partial |
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

D2–D5 are the integrator soft clippers. They are not part of the output VCA.

## Routing

IC12 and IC13 (4053) select LP/BP/HP and the serial versus parallel connection of the two cores. C20 and C21 sit **after** the mode switches and **before** the audio VCA, so each routed filter output has its own coupling state. A disconnected branch does not share the other branch’s capacitor. Reconnection must not be modeled as one high-pass after a mix unless the resistor loading is shown to be identical, and it is not: the series resistors on the two legs are 33 kΩ and 100 kΩ.

Section 1 remains the existing software listening path. It uses the same output coupling and VCA once that path is the single selected branch.

## Still unresolved before production equations

These items are blocked, not guessed:

1. Exact PWM source resistor into C9, and therefore the control time constant.
2. Which of R24 (100 kΩ) and R31 (33 kΩ) belongs to filter 1 versus filter 2, and the reference-cell input resistor that sets \(I_{\text{ref,in}}\).
3. Whether R53 is the shipped nominal or only the optional bleed build.
4. Loaded cutoff of C34/C1 and of C20/C21 once those resistors are assigned.
5. Sign of IC2A and IC2B.

Until those nodes are traced, production Dual DSP stays on the current prototype VCA. The host 0.7 Hz blocker is unchanged by this note.

## Next measurement

Trace the v03 nets for the VCA control summer and the two coupling legs, then write the independent reference (static gain versus PWM, and the two coupling transfer functions) before replacing `kVcaCvTauSeconds`.
