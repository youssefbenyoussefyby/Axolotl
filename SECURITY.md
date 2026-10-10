# Security Policy

AxolotlDB is a research and learning engine. **Do not use it to store data you care
about.** It has no authentication, no transactions or crash recovery yet (planned for
P5), and the network server is a stub.

## Supported versions

Only the latest commit on the default branch is maintained. There are no backported
fixes.

## Reporting a vulnerability

Even so, memory-safety bugs in the storage layer are worth knowing about. Examples
include out-of-bounds reads on a corrupted page, or a crash when the engine opens a
crafted database file. Please report them privately with
[GitHub private vulnerability reporting](https://github.com/youssefbenyoussefyby/Axolotl/security/advisories/new)
rather than in a public issue.

Include:

- the commit SHA
- a minimal input (SQL text, or a database file) that reproduces the issue
- the sanitizer output, if you have it (see the ASan/UBSan build in
  [CONTRIBUTING.md](CONTRIBUTING.md))

Expect an acknowledgement within two weeks. Fixes land as ordinary commits with a
regression test.
