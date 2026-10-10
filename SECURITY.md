# Security Policy

libuvcpp ships prebuilt binaries for six platforms, so a vulnerability here can
reach someone without them ever compiling anything. Please report it privately.

## Supported versions

| Version | Supported |
| --- | --- |
| 1.6.x | The current release line — fixes land here. |
| 1.5.x | Security fixes only. |
| 1.4.x and older | Not supported; please upgrade. |

Prebuilt packages are produced by this repository's own CI from the release tag
(`.github/workflows/release.yml`) and attached to the GitHub Release for that
tag. A package is supported exactly when its tag is one of the lines above.

## Reporting a vulnerability

Use GitHub's **private security advisory** form:

<https://github.com/Antruly/libuvcpp/security/advisories/new>

That channel is visible only to you and the maintainers. **Do not open a public
issue, pull request or discussion for a security problem** — a public report is
a zero-day for everyone who reads it before a fix ships.

This project has no security email address. The advisory form above is the
intended — and only — reporting channel.

### What to include

- the release tag, or the commit you built from, and which package you used
  (`linux-x64`, `msvc-arm64`, `mingw-x64`, …) — or the switches you configured
  with, if you built from source;
- a minimal reproduction: a small source file, or the exact commands and inputs;
- what you believe the impact is, and any fix you have in mind.

### What to expect

This is a single-maintainer project, so the following are best-effort targets
rather than a contract:

- an acknowledgement within about a week;
- an assessment of whether the report is exploitable, and a rough timeline;
- credit in the published advisory, unless you would rather stay anonymous.

### Out of scope

- Bugs in the libraries libuvcpp builds on — libuv, OpenSSL, nghttp2, ngtcp2,
  llhttp, nlohmann/json. Report those upstream; this project cannot fix them
  from here.
- Anything that requires a modified build, a patched binary, or a hostile local
  environment. An attacker who can already read your process memory or write
  your files is outside the boundary this library defends.
- Crashes reachable only in an instrumented build, or only through the test
  suite's own helper macros.

## Disclosure

Once a fix is ready, the advisory is published together with the release that
carries it, and the Release notes for that version point at the advisory. If you
reported it, you are asked first whether and how you want to be credited.
