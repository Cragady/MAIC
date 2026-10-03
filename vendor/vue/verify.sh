#!/bin/sh
# Fetches the pinned files again and checks them against PROVENANCE's hashes and the committed copies.
set -e
cd "$(dirname "$0")"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
wget -q -O "$tmp/vue.global.prod.js" https://unpkg.com/vue@3.5.43/dist/vue.global.prod.js
wget -q -O "$tmp/LICENSE" https://unpkg.com/vue@3.5.43/LICENSE
for f in vue.global.prod.js LICENSE; do
    want=$(grep -A2 "^$f\$" PROVENANCE | sed -n 's/.*sha256 *//p')
    got=$(sha256sum "$tmp/$f" | cut -d' ' -f1)
    [ "$want" = "$got" ] || { echo "$f: upstream sha256 $got, PROVENANCE says $want"; exit 1; }
    cmp -s "$tmp/$f" "$f" || { echo "$f: the committed copy differs from upstream"; exit 1; }
done
echo "vue 3.5.43: upstream matches PROVENANCE and the committed copy"
