# Third-party notices

## Mutable Instruments Shruthi software

Swara XT contains source adapted from the Shruthi firmware by Emilie Gillet.

- Upstream: https://github.com/pichenettes/shruthi-1
- Revision: `56bfe78a27cd7430ab4531439d4efc2834353b17`
- License: GNU General Public License version 3
- Path: `third_party/shruthi-1`

Upstream notices remain in vendored and adapted source files. Swara XT's
host-side ownership, filter, VCA, and sample-rate conversion are project work;
this does not alter ownership of Shruthi-derived algorithms and resources.

## Shruthi-derived panel artwork

Portions of the panel artwork are adapted from the Shruthi-1 hardware/panel
design by Emilie Gillet / Mutable Instruments under CC BY-SA 3.0 Unported. The
adapted artwork remains under CC BY-SA 3.0. Asset-level attribution, modification
information, and the license reference are provided in
`resources/Skin/ATTRIBUTION.md`.

This artwork license does not apply to Swara XT software, which is distributed
under GPL-3.0-or-later.

## avrlib

- Upstream: https://github.com/pichenettes/avril
- Revision: `af7266e5b48ae20c0f83d4352c75c1068545cffc`
- Path: `third_party/shruthi-1/avrlib`
- License: as distributed with the upstream GPLv3 project

## JUCE

- Upstream: https://github.com/juce-framework/JUCE
- Version: 9.0.1
- Revision: `e18f7f506c0b96f2c738a0bcd7fe6467a5005ad8`
- License mode: GPLv3 application option

JUCE's own license and bundled third-party notices apply to its source and VST3
support materials.

## Alte DIN 1451 Mittelschrift

- File: `resources/Fonts/din1451alt.ttf`
- Family: Alte DIN 1451 Mittelschrift
- Style: Regular / Upright
- Embedded manufacturer attribution: Ludwig Goller
- Digital font distribution: Peter Wiegel, https://www.peter-wiegel.de/alteDin1451.html
- Source used to verify the exact file: https://www.1001fonts.com/alte-din-1451-mittelschrift-font.html
- License: SIL Open Font License 1.1
- SHA-256: `3CDBDD35C7637FF4A15D22BDCB8B952F48AFADD2E41EDB5AB29D80EAAD8D48FD`
- Usage: Swara XT GUI typography

The exact redistributed file matches the published Alte DIN 1451 package by
SHA-256. The applicable OFL text, including Peter Wiegel's copyright statement
and Reserved Font Name, is included at `resources/Fonts/OFL-1.1.txt`.
