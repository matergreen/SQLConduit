# Release checklist

Use this checklist for every SQLConduit release. A release is complete only after verification of
the published assets, not merely after pushing a tag.

## Prepare

- [ ] Confirm the target version is supported by the policy in `SUPPORT.md`.
- [ ] Freeze the intended commit; review public API, configuration schema, metrics contract, and
      migration notes.
- [ ] Update `CMakeLists.txt`, `include/sqlconduit/version.h`, consumer contract tests, and the
      versioned JSON Schema `$id` to the same version.
- [ ] Move user-visible entries from `Unreleased` to an exact `## [x.y.z]` heading in
      `CHANGELOG.md`.
- [ ] Run formatting/static checks used by the project and `git diff --check`.
- [ ] Run the full unit test matrix plus MySQL, PostgreSQL, SQL Server/ODBC, and Oracle integration
      suites.
- [ ] Run optimized performance benchmarks on the release candidate and compare with the previous
      release on the same machine; explain repeatable regressions above 10%.
- [ ] Build and install each enabled component independently and run the installed consumer smoke
      test.
- [ ] Review `SECURITY.md`, `SUPPORT.md`, and known issues; resolve or explicitly document blockers.

## Publish

- [ ] Create an annotated `vX.Y.Z` tag from the reviewed commit and push it.
- [ ] Confirm every GitHub Actions matrix job succeeds and the release job is not skipped.
- [ ] Confirm the release contains every platform archive, `SHA256SUMS`, the SPDX JSON SBOM, and
      provenance/SBOM Sigstore bundles.
- [ ] Confirm GitHub shows build-provenance and SBOM attestations for the platform archives.
- [ ] Confirm release notes came from the matching changelog section and contain upgrade warnings.

## Consumer verification

Download all files into one directory, then verify integrity:

```sh
sha256sum --check SHA256SUMS
```

Verify GitHub build provenance for each archive:

```sh
gh attestation verify sqlconduit-X.Y.Z-PLATFORM.tar.gz --repo OWNER/SQLConduit
```

Verify the SBOM attestation by adding the SPDX predicate type:

```sh
gh attestation verify sqlconduit-X.Y.Z-PLATFORM.tar.gz \
  --repo OWNER/SQLConduit \
  --predicate-type https://spdx.dev/Document/v2.3
```

On Windows, use `Get-FileHash -Algorithm SHA256` to compare an individual archive with
`SHA256SUMS`.

## After release

- [ ] Install one published archive on a clean supported host and run a real connection smoke test.
- [ ] Update the support table when a new minor line starts or a transition window ends.
- [ ] Announce security or compatibility changes through the same channels as the release.
- [ ] Keep the tag immutable. If assets are wrong, publish a new patch release rather than silently
      replacing content after consumers may have verified it.
