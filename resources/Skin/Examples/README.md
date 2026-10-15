# Custom skin artwork

Use the standalone **SWARA XT Skin Editor** (`SwaraXtSkinEditor`) for a live colour preview, then Install for Plugin or Save As a `.txt` file. You can also load files from the plugin context menus.

Right-click the plugin editor to load a user file. Installed copies live next to `Swara XT.settings`, under `skin_assets/`. They are not stored inside the plugin bundle.

| Example | Menu | Installed name | Limit |
|---------|------|----------------|-------|
| `manufacturer_mark_example.svg` | Manufacturer Mark → Load SVG… | `manufacturer_mark.svg` | SVG, 256 KiB |
| `decoration_example.svg` | Decoration → Load SVG… | `decoration.svg` | SVG, 256 KiB |
| `background_example.svg` | Background → Load PNG or SVG… | `background.svg` | SVG, 256 KiB |
| `background_example.png` | Background → Load PNG or SVG… | `background.png` | PNG, 4 MiB, max 8192 px per side |
| `colours_example.txt` | Interface Colours → Load colour text… | `colours.txt` | text, 64 KiB |

Preferences folder:

- Windows: `%APPDATA%\MontroneDSP\Swara XT\skin_assets\`
- Linux: `~/.config/MontroneDSP/Swara XT/skin_assets/`
- macOS: `~/Library/Application Support/MontroneDSP/Swara XT/skin_assets/`

## Rules

- Manufacturer mark and column decoration accept `.svg` only.
- Background accepts `.png` or `.svg`. Loading one replaces the other.
- SVG must contain an `<svg>` root and at least one drawable shape. JUCE keeps paths and fills. Filters, masks, text, and embedded images are ignored.
- Mark colours that use `#ffffff`, `#d0d0d0`, `#0a0608`, `#f5e9fb`, or `#9b4f9e` are rewritten to the active skin. Other colours stay as drawn.
- Decoration strokes or fills of white (`#ffffff`) are rewritten to the skin braid tint.
- Background colours are left as authored and stretched over the 1113×521 design surface. Module panels still paint their own fills, so the picture shows in the gaps.
- PNG must decode as an image no larger than 8192×8192.

## Interface colours

`colours_example.txt` lists every colour the editor uses. Each line is `name = #RRGGBB`. Leave a line out to keep that colour from the selected built-in skin (Pastel, Midnight Gold, and the others). Changing the built-in skin keeps the text file and reapplies it on top of the new skin.

Text keys: `primary_text`, `secondary_text`, `muted_text`, `module_header_text`, `combo_text`, `display_text`, `matrix_text`. The other keys cover panels, knobs, menus, the visualizer, and the modulation matrix.

## Fallbacks

- A file that fails the extension, size, or parse check is not installed. The previous custom file, or the built-in art, stays in place.
- A previously installed file that later fails to parse is ignored. The editor draws the built-in mark, decoration, background, or skin colours, and the menu shows “invalid — using built-in”.
- A colour line with an unknown name or a bad hex value is skipped. The other lines still apply. If none of the lines are valid, the file is not installed.
- Clear custom, or pick a built-in mark, decoration, background, or colour set, removes that user file.
