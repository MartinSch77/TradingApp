# Change Management Strategy

Referenced by `processes/SUP.10-change-request-management.md`.

## Scope

Applies to any change touching a BASELINED work product (`SUP.8`): a change
to a tagged release, or a change to this process framework's own released
version. Ordinary in-development work on `main` before the next baseline is
governed by `SWE.1`–`SWE.6`'s own review steps, not by this stricter process
— change control exists for the added risk of touching something already
shipped or already released as a process version.

## Labels and states

Same scheme as `SUP.9` (the project owner's explicit instruction to share
it): `proposed → analyzed → approved → implemented → verified → closed`,
realized as GitHub labels on the change-request issue AND mirrored onto its
implementing PR.

## Impact assessment — mandatory fields

A change request is not `analyzed` until it states:

- Requirements affected (REQ ids, or "none").
- Architecture/design affected (component/unit ids, or "none").
- Tests affected (new/modified/removed, or "none").
- **Safety or real-money path affected** — explicitly called out because a
  change touching REQ-N-005 (double-press), the risk/exposure caps, or
  anything in `PaperTrader`'s money arithmetic gets the Section below's
  extra approval step regardless of how small the diff looks.
- Configuration items affected (which baselines/tags this touches).
- Risks introduced or changed (new `MAN.5` entries, or existing ones this
  resolves/worsens).

This is the exact field set `.github/PULL_REQUEST_TEMPLATE.md`'s "Change
impact" section captures — the PR template is this strategy expressed as a
GitHub form, not a second, divergent list.

## Extra approval for safety/money-path changes

A change marked "safety or real-money path affected: yes" requires TWO
independent approvals before `implemented`: the normal technical review AND
a second review from someone (or an independent AI session, per
`process-model.md` §7) who did not author the change — mirroring this
project's own REQ-N-005 double-press principle at the process level, not
just the UI level.
