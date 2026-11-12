#!/usr/bin/env bash
# Checks a release tag against the project before it is pushed or published: the tag's version must
# be the one in Cargo.toml and app/CMakeLists.txt, and docs/releases/<tag>.md must exist and name
# both packages. The release job in .github/workflows/ci.yml runs it; run it yourself before tagging.
#
#   tools/release-check.sh v0.1.3
set -euo pipefail

tag=${1:?usage: tools/release-check.sh v<major>.<minor>.<patch>}
root=$(cd "$(dirname "$0")/.." && pwd)
errors=0

fail() {
    if [ -n "${GITHUB_ACTIONS:-}" ]; then
        echo "::error::$1"
    else
        echo "error: $1" >&2
    fi
    errors=$((errors + 1))
}

version=${tag#v}
if [[ $tag != v* || ! $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    fail "tag '$tag' is not v<major>.<minor>.<patch>"
fi

# CR stripped: a Windows checkout has CRLF line ends
cargo=$(tr -d '\r' <"$root/Cargo.toml" | sed -n '/^\[workspace\.package\]/,/^\[/s/^version *= *"\([^"]*\)".*/\1/p')
cmake=$(tr -d '\r' <"$root/app/CMakeLists.txt" | sed -n 's/^project(fs-editor VERSION \([0-9.]*\).*/\1/p')
[ "$cargo" = "$version" ] || fail "tag $tag, but Cargo.toml [workspace.package] has version '$cargo'"
[ "$cmake" = "$version" ] || fail "tag $tag, but app/CMakeLists.txt project() has VERSION '$cmake'"

notes="docs/releases/$tag.md"
if [ ! -f "$root/$notes" ]; then
    fail "$notes is missing: write the release notes from docs/releases/TEMPLATE.md"
else
    for pkg in "FS-Editor-$version-windows-x64.zip" "FS-Editor-$version-linux-x64.zip"; do
        grep -qF "$pkg" "$root/$notes" || fail "$notes does not name $pkg in its Downloads table"
    done
    # TEMPLATE.md writes every placeholder as <Capitalised text>; HTML-like tags in real notes
    # (`<i>`, `<b>`) are lower case
    if grep -nE '<[A-Z][^<>]*>|<version>|^-{20,}$|^<!--' "$root/$notes"; then
        fail "$notes still has template placeholders, instructions or comments (lines above)"
    fi
fi

if [ "$errors" -gt 0 ]; then
    exit 1
fi
echo "release $tag: version $version, notes $notes"
