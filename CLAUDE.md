# explorer-lite: agent notes

- `main` is production. Each task gets its own branch from `main` and enters through a PR after Diego's OK. Never push to `main` directly.
- Plan: https://plans.justdiego.com/explorer-lite/2026-10-24-shell (slug `shell`). Mark progress with the plan-html skill.
- Develop **alongside** Windows' explorer. `Winlogon\Shell` in HKCU only in phase 4, with Diego's explicit OK and the way back tested first. Never touch dwm, Defender, lsass, csrss or GPU drivers.
