# Security Policy

## Reporting a security-sensitive issue

For a potential vulnerability, release-integrity concern, or vulnerability in a
bundled dependency, contact MontroneDSP privately at
[support@montronedsp.com](mailto:support@montronedsp.com). Include the affected
version, platform and host where relevant, a minimal reproduction, and any
workaround you know.

Please use public [GitHub Issues](https://github.com/montronedsp/SWARAXT/issues)
for ordinary bugs, feature requests, and non-sensitive build questions. Do not
publish a proof of concept or sensitive details in a public issue before the
maintainer has had a reasonable opportunity to assess the report.

## Scope and expectations

SWARA XT is a local audio application and plug-in. It processes audio, MIDI,
and local project, preset, and state data supplied by the host or user. It is
not an authentication system, a user-password database, or a security
boundary.

Obtain builds from the project's official distribution channels and verify
published checksums when they are available. Treat malformed or untrusted
project, preset, state, and other input files cautiously; they should not be
assumed harmless merely because they are local.

Memory-safety defects, unsafe parsing, dependency vulnerabilities, and release
or build-integrity defects are security-relevant when applicable. The project
does not claim to be sandboxed, memory-safe, telemetry-free, or unable to make
network requests in every host or environment. Reports are handled by the
project maintainer according to available capacity; no response-time or support
guarantee is implied.
