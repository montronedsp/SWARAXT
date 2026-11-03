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
- Linux version: 9.0.4, revision `3fb5cbc0340758596dac39f7c8de47d562b4896e`
- Windows/macOS version: 9.0.1, revision `e18f7f506c0b96f2c738a0bcd7fe6467a5005ad8`
- Licensing terms: consult the selected upstream `LICENSE.md` and module notices.
  JUCE 9.0.4 describes its modules as AGPLv3/commercial dual-licensed. This
  dependency inventory does not select or certify an application licensing mode.

JUCE's own license and bundled third-party notices apply to its source and VST3
support materials. Preserve the selected upstream notices when distributing
JUCE-derived materials. The Linux 9.0.4 source inventory in `JUCE.spdx.json`
includes the following bundled codecs; symbol visibility does not remove their
notice requirements:

| Component | Pinned upstream version | SPDX-declared license |
| --- | --- | --- |
| libwebp | 1.6.0 | BSD-3-Clause |
| libogg | 1.3.6 | BSD-3-Clause |
| libvorbis | 1.3.7 | BSD-3-Clause |
| Opus | 1.6.1 | BSD-3-Clause |
| opusfile | 0.12-59-g6dfd29e | BSD-3-Clause |
| libopusenc | 0.3 | BSD-3-Clause |

The complete pinned source and notice inventory is authoritative; this table
is not a replacement for bundled copyright/license files.

## Adriatica Next 1.0

- Files: `resources/Fonts/AdriaticaNext-Medium.ttf` and
  `resources/Fonts/AdriaticaNext-CondensedBold.ttf`
- Families/styles: Adriatica Next Medium; Adriatica Next Condensed Bold
- Version: 1.000
- Copyright: MontroneDSP (2026), with portions derived from Fira Sans,
  The Mozilla Foundation and Telefonica S.A. (2012--2015)
- License: SIL Open Font License 1.1, included at `resources/Fonts/OFL-1.1.txt`
- Usage: embedded SWARA XT GUI typography

## Brand artwork

`resources/Skin/Shared/montronedsp_wordmark.svg` and `stones_letters.svg`
are copyright 2026 Andrea Montrone, as confirmed by the rights holder. They
contain outlined vector paths and require no embedded font or font installation.
Their existing CC-BY-SA-3.0 grants remain in effect. The software GPL notice
does not independently license these files. No prior permissions are withdrawn,
and no trademark registration or unverified font provenance is asserted.
