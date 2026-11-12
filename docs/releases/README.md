# Releases

Each release has its notes here as `v<version>.md`, in English, in the format of [TEMPLATE.md](TEMPLATE.md). The file is the GitHub release body word for word, and the program's update dialog shows it as plain text.

Packages are unpack-and-run ZIPs, built and smoke-tested by CI:

- `FS-Editor-<version>-windows-x64.zip`
- `FS-Editor-<version>-linux-x64.zip`
- `SHA256SUMS.txt`, the SHA-256 checksums of both

(0.1.0 to 0.1.2 were published as `FS-Editor-<version>-win64.zip` and `FS-Editor-<version>-Linux.zip`; their notes keep those names.)

## Procedure

1. **Bump the version** in both places, which must agree: `version` under `[workspace.package]` in `Cargo.toml`, and `project(fs-editor VERSION ...)` in `app/CMakeLists.txt`. Run `cargo check` so `Cargo.lock` follows, and commit.
2. **Write the notes**: copy `TEMPLATE.md` to `docs/releases/v<version>.md`, fill it in, delete the instructions above its line and any unused section, and commit.
3. **Check**: `tools/release-check.sh v<version>` confirms the tag matches both versions and the notes exist, name both packages and have no placeholders left.
4. **Tag and push the tag**: `git tag v<version>` on the release commit (already pushed to `main`), then `git push origin v<version>`.
5. **Wait for CI**: the tag runs the usual CI build and smoke test, then the `release` job drafts the GitHub release "FS Editor <version>" with both ZIPs, `SHA256SUMS.txt` and the notes. A failing check (missing notes, version mismatch, unexpected package names) stops it before anything is drafted; fix it, move the tag (`git tag -f`, `git push -f origin v<version>`) and let it run again. Re-running the job refreshes its own draft and never touches a published release.
6. **Review the draft** on GitHub: the notes, the asset names, and, if you like, a download checked against `SHA256SUMS.txt`.
7. **Publish** it. Only then does the program's update check see it as the latest release.
