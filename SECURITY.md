# Security policy

## Supported versions

Security fixes follow the version table in [SUPPORT.md](SUPPORT.md). In short, the latest released
minor line receives security and critical correctness fixes. Older preview lines are unsupported
unless that table explicitly says otherwise.

## Reporting a vulnerability

Please do not open a public issue for a suspected vulnerability. Use GitHub's **Report a
vulnerability** form in the repository Security tab so the report, discussion, and any patch remain
private until coordinated disclosure.

Include, when available:

- the affected version, commit, platform, compiler, and enabled drivers;
- a minimal reproducer or proof of concept;
- expected and observed impact, including whether secrets or SQL text can be exposed;
- any known mitigations and whether the issue is already public.

The project aims to acknowledge a complete report within three business days and provide an initial
assessment within seven business days. These are response targets, not a guarantee. The reporter
will receive progress updates when the assessment, fix, and disclosure timeline materially change.

Do not include production credentials, customer data, or access tokens in a report. Test only
systems and data you are authorized to access.

## Disclosure and advisories

Confirmed vulnerabilities are fixed on a private branch when practical, assigned severity using
CVSS as guidance, and published through a GitHub Security Advisory. Release notes identify the
fixed supported versions and mitigations. Credit is given unless the reporter asks to remain
anonymous.

Release archives include SHA-256 checksums, an SPDX JSON SBOM, and GitHub artifact attestations.
Consumers should verify both the checksum and provenance before deployment; see
[the release checklist](docs/release_checklist.md#consumer-verification).
