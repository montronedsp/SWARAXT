# Contributing

Contributions are accepted under GPL-3.0-or-later.

- Keep parameter IDs stable; they are part of the host automation interface.
- Preserve copyright and provenance in Shruthi-derived files.
- Keep the audio path allocation-free, lock-free, and free of I/O or logging.
- Add focused regression coverage for behavioral changes.
- Avoid changing approved DSP behavior without reproducible evidence.
- Run a Release build and the complete CTest suite before submitting changes.

Do not force-push shared branches or add dependencies without documenting their
version, licence, and purpose.
