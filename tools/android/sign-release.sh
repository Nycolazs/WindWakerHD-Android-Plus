#!/bin/bash
# Signs a release APK with the permanent release key (android/keystore.properties: storeFile,
# storePassword, keyAlias, keyPassword) and, with --publish TAG, uploads it to that GitHub release
# (a draft is published). For an APK built without the key (it carries the build machine's debug
# signature, which apksigner replaces), so every update still installs over the previous version.
#   tools/android/sign-release.sh <unsigned-or-debug.apk> <out.apk> [--publish v1.10]
set -e
cd "$(dirname "$0")/../.."
in=$1 out=$2
[ -f "$in" ] && [ -n "$out" ] || { echo "usage: $0 <in.apk> <out.apk> [--publish TAG]"; exit 1; }
props=android/keystore.properties
[ -f $props ] || { echo "no $props (storeFile, storePassword, keyAlias, keyPassword)"; exit 1; }
get() { grep -E "^$1=" $props | cut -d= -f2-; }
sdk=$(grep -E '^sdk.dir=' android/local.properties | cut -d= -f2-)
apksigner=$(ls -d "$sdk"/build-tools/*/apksigner | tail -1)
"$apksigner" sign --ks "$(get storeFile)" --ks-pass "pass:$(get storePassword)" --ks-key-alias "$(get keyAlias)" \
  --key-pass "pass:$(get keyPassword)" --out "$out" "$in"
"$apksigner" verify --print-certs "$out" | grep -E "SHA-256"
if [ "$3" = "--publish" ] && [ -n "$4" ]; then
  gh release upload "$4" "$out" --clobber
  gh release edit "$4" --draft=false --latest
fi
