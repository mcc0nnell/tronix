# Watchcore 0.1.0 — canonical source and public TRONIX mirror

Status: experimental draft only. This is not a release, promotion or deployment.

## Canonical source identity

- Technical canonical owner: mcc0nnell/NeonOxide, components/watchcore/
- Immutable canonical source commit: c408c190b8e1bc30df7aa23cfd9677e88eb6afb6
- Source origin: mcc0nnell/tronix, commit 019e681d43e5906ec0c42b63ea61c2f17f3a3cab
- Release contract: neonoxide.watchcore.release.v1, source version 0.1.0
- Aggregate source-and-attribution tree SHA-256:
  fe3896262506d9bfb62152f563987ba28e84b9d509c74cad541e87e718ef2911

NeonOxide is the intended canonical home for subsequent Watchcore source work.
Historical provenance and licensing remain explicitly recorded; this change
does not claim any retroactive copyright transfer.

## TRONIX is independently buildable

TRONIX keeps the current public C++20 snapshot under integrations/atlas-watchcore
and the existing Celix adapters. The exact lock copied from NeonOxide
components/watchcore/RELEASE.json is
integrations/atlas-watchcore/NEONOXIDE_SOURCE_LOCK.json.

The separate Python verifier checks all five source/build/test files against
immutable SHA-256 hashes, as well as TRONIX's public LICENSE and NOTICE and
the aggregate tree digest. It rejects unexpected paths, changed commits,
duplicate fields, missing source and symlink substitution.

Commands, without any private repository credentials:

    python3 scripts/verify_watchcore_source.py
    python3 -m unittest discover -s tests -p 'test_watchcore_source_lock.py' -v

These commands run in native.yml before fetching third-party dependencies and
in license.yml. Public Celix CI remains fully independent of NeonOxide access.
Existing CMake references, service identifiers, decision behavior, source
headers, Python Watcher authority and Rizzer shadow integration are unchanged.

## Bounded source update procedure

1. Prepare and review a versioned NeonOxide Watchcore source change.
2. Generate its deterministic offline release archive and signed distribution
   process in a separate review; the present source export is not signed.
3. Explicitly approve and vendor a reviewed public snapshot plus release lock.
4. Review changes to the independent TRONIX pin verifier.
5. Verify identical bytes from the canonical archive, run native Watchcore
   assertions, Celix lifecycle and fail-closed tests, Python parity and public
   exact-head CI.
6. Only consider merge/source-owner handover after provenance/license review
   and explicit owner approval.

For now, no runtime calls fetch private code, no authority moves to NeonOxide,
and no code is deleted from TRONIX.
