# Contributing

Contributions are accepted under GPL-3.0-or-later.

## Developer Certificate of Origin

Non-trivial contributions must include a `Signed-off-by` line, for example:

```text
Signed-off-by: Full Name <email@example.com>
```

You can normally add it with `git commit -s`. The sign-off asserts that you
are legally entitled to submit the contribution under this project's license.
See the [Developer Certificate of Origin 1.1](https://developercertificate.org/)
for the full statement. SWARA XT does not require a CLA.

- Keep parameter IDs stable; they are part of the host automation interface.
- Preserve copyright and provenance in Shruthi-derived files.
- Keep the audio path allocation-free, lock-free, and free of I/O or logging.
- Add focused regression coverage for behavioral changes.
- Avoid changing approved DSP behavior without reproducible evidence.
- Run a Release build and the complete CTest suite before submitting changes.

Do not force-push shared branches or add dependencies without documenting their
version, licence, and purpose.
