# explorer-lite: agent notes

- `main` is production. Each task gets its own branch from `main` and enters through a PR after Diego's OK. Never push to `main` directly.
- Plan: https://plans.justdiego.com/explorer-lite/2026-10-24-shell (slug `shell`). Mark progress with the plan-html skill.
- Develop **alongside** Windows' explorer. `Winlogon\Shell` in HKCU only in phase 4, with Diego's explicit OK and the way back tested first. Never touch dwm, Defender, lsass, csrss or GPU drivers.
- Build: `just build-mac` (zig c++ cross-compile; `just build` where zig is). Package: `just package-mac X.Y.Z`. Release: a push to `main` runs `.github/workflows/release.yml`, which publishes `vX.Y.Z` in the public `justdiegocom/explorer-lite-releases` (needs the `RELEASE_TOKEN` secret); installed copies update from its `releases/latest`.
- Test on Windows only through `just test-pc <scenario>` (`tools/scenarios/*.txt`, steps in `tools/pc-run.ps1`): `--instance test` (own data folder, mutex, window classes) on a desktop of its own in the user's session, so nothing shows on the user's screens and nothing takes the focus. Screenshots come from the app itself (`--cmd "shot <png>"`), never from the screen.
- Memory is measured as Task Manager does (private working set). The resident must stay near 1-2 MB with no window: Direct2D/DirectWrite load with the first window and go with the last one.
- UI strings in English through `T()`/`Tf()`; `just i18n` checks every language has them (CI too). English is the default, whatever Windows' language.
