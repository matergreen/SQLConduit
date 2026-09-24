# Version support

SQLConduit is still a preview release. The support window is intentionally simple and is reviewed
at every minor release.

| Version line | Status | Support level |
| --- | --- | --- |
| `0.8.x` | Development | Fixes land here before the 0.8.0 release; not a production support line yet |
| `0.7.x` | Supported | Security fixes and critical correctness/build fixes |
| `0.6.x` and older | Unsupported | Upgrade to a supported line before reporting a version-specific issue |

When 0.8.0 is released, `0.8.x` becomes supported and `0.7.x` receives security fixes for a
90-day transition period. After that period, `0.7.x` becomes unsupported. Patch releases do not
shorten an existing support window.

Support means the project will evaluate applicable reports and may publish a patch; it does not
promise an SLA. Database-server and client-library versions also need to be supported by their
vendors. Reproductions should state the SQLConduit version, OS, compiler, database server, client
SDK, and driver configuration.
