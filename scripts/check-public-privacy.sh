#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

patterns="
Struc""ture2
Struc""tureAOO
S2""AM
/Users/
Maki""Pad
josh""Phone
Juce/Struc""ture2_B
"

failed=0
for pattern in $patterns; do
    if git grep -n -I -F "$pattern" -- ':!scripts/check-public-privacy.sh'; then
        printf 'public privacy gate: forbidden private identifier: %s\n' "$pattern" >&2
        failed=1
    fi
done

if git grep -n -I -E 'github\.com/[^/]+/(Structure|structure)[^/]*' -- \
    ':!scripts/check-public-privacy.sh'; then
    printf 'public privacy gate: private product repository URL detected\n' >&2
    failed=1
fi

exit "$failed"
