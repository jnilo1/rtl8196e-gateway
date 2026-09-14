# Driver audit — cumulative-ledger template

Use this structure for every driver `AUDIT.md`.  The document is a cumulative
ledger: it keeps the evidence and reasoning from successive audit passes, while
the opening table is the single authoritative statement of the current state.

```markdown
# <driver> — cumulative security and performance audit

> **Cumulative audit ledger.** The current-state table and current finding
> registry are authoritative. Older detailed sections are retained as
> historical evidence; an unqualified status or version inside one of those
> sections applies only to that audit pass.

| Current state | Authoritative value |
|---|---|
| **Current implementation** | `<driver>` vX.Y |
| **Release target** | firmware v4.0.0 |
| **Last audit pass** | YYYY-MM-DD — pass A<n> |
| **Last fully audited baseline** | vX.Y |
| **Post-baseline changes** | none, or an explicit list and review level |
| **Validation state** | static / build / target / field evidence and pending gates |
| **Maintained kernels** | Linux 6.18 and/or 7.2 |
| **Current finding registry** | section link |

## Audit-pass ledger

| Pass | Date | Baseline | Result | Validation |
|---|---|---|---|---|
| A1 | YYYY-MM-DD | vX.Y | findings and produced version | evidence |

## Current verdict

...

## Detailed audit record

### Pass A<n> — YYYY-MM-DD

> Historical evidence for pass A<n>. Current dispositions are in the opening
> table and current finding registry.
```

## Required terminology

- **Current implementation** is the version in the source tree now.
- **Last audit pass** is the most recent review, whether full or targeted.
- **Last fully audited baseline** is the exact version covered end-to-end.
- **Post-baseline changes** prevents a newer source version from being
  accidentally presented as fully audited.
- **Validation state** distinguishes source review, compilation, target tests
  and field soak.
- **Release target** is a planned firmware release, not proof that the code has
  shipped.

Do not use a single ambiguous `Audit date`, `Driver version`, or `Active
release` field for a cumulative document. Preserve finding IDs across passes.
Historical prose may keep its original conclusion, but it must be introduced
as pass-scoped evidence and must not override the current finding registry.
