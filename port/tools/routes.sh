#!/bin/sh
# Run every port/routes/*.route, shots in OUT/<route>/.
# With BASELINE (an OUT folder of an earlier run) list the shots that changed.
#   port/tools/routes.sh EXE OUT [BASELINE]
#   RUN=wine port/tools/routes.sh tmc_pc.exe out base     (Windows build on Linux)
# PAK=path/tmc_data.pak picks the game data (default: next to EXE).
set -u
EXE=$(realpath "$1"); OUT=$2; BASE=${3:-}
ROUTES=$(dirname "$(realpath "$0")")/../routes
PAK=$(realpath "${PAK:-$(dirname "$EXE")/tmc_data.pak}")
RUN=${RUN:-}
mkdir -p "$OUT"
changed=0; failed=0
for r in "$ROUTES"/*.route; do
    name=$(basename "$r" .route)
    dir="$OUT/$name"
    rm -rf "$dir"; mkdir -p "$dir"
    args=$(sed -n 's/^#! //p' "$r")
    # shellcheck disable=SC2086
    (cd "$dir" && $RUN "$EXE" --headless --data "$PAK" --save route.sav $args --route "$(realpath "$r")" > log.txt 2>&1)
    rc=$?
    shots=$(ls "$dir"/*.bmp 2>/dev/null | wc -l)
    if [ $rc -ne 0 ] || [ "$shots" -eq 0 ]; then
        echo "$name: FAILED (exit $rc, $shots shots, see $dir/log.txt)"
        failed=$((failed + 1))
        continue
    fi
    if [ -n "$BASE" ]; then
        diff=0; new=0
        for s in "$dir"/*.bmp; do
            b="$BASE/$name/$(basename "$s")"
            if [ ! -f "$b" ]; then
                new=$((new + 1))
            elif ! cmp -s "$s" "$b"; then
                echo "  changed: $s"
                diff=$((diff + 1))
            fi
        done
        # shots the baseline doesn't have (a new or edited route) are counted, not failed
        echo "$name: $shots shots, $diff changed$([ $new -gt 0 ] && echo ", $new not in the baseline")"
        changed=$((changed + diff))
    else
        echo "$name: $shots shots"
    fi
done
[ $failed -eq 0 ] && [ $changed -eq 0 ]
