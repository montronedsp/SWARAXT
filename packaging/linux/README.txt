Swara XT 1.3.0
MontroneDSP

VST3 — per-user installation:

Copy the complete:
Swara XT.vst3

directory to:

~/.vst3/

Create ~/.vst3 if it does not exist.

System-wide VST3 locations commonly include:
/usr/lib/vst3/
/usr/local/lib/vst3/

System-wide installation may require administrator privileges.

Do not copy only the inner .so file.
Copy the complete .vst3 bundle.

Standalone:

Make the executable runnable if necessary:

chmod +x "Swara XT"

Then launch it directly.

You may place it in a folder such as:
~/.local/bin/
or another location of your choice.

Skin Examples:

The Skin Examples folder contains starter SVG/PNG/TXT assets
(manufacturer mark watermark, column decoration, background, and colours).
Load them directly from the synthesizer editor context menus.
The separate Skin Editor application is not included in version 1.3.0.
Additional SVG decorations are in Skin Examples/Decorations.
Installed user copies live under:

~/.config/MontroneDSP/Swara XT/skin_assets/

See Skin Examples/README.md for artwork rules and limits.

If the DAW does not immediately see Swara XT, rescan VST3 plug-ins.

Support:
support@montronedsp.com

Source:
https://github.com/montronedsp/SWARAXT

Corresponding source
--------------------
See SOURCE.txt for the exact source revision, dependencies and license notices.
The source includes the optional Skin Editor; its application is not packaged.
