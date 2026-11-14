#!/usr/bin/env bash
# The baselines of stage 5: what other people's compressors make of a file
# of ITCH 5.0 messages, in bytes per message and MB/s, as one table. They
# are commands, not dependencies: nothing links them and the build does not
# need them.
#
# Every baseline runs on one core. Each is timed by the wall clock, except
# the fair baseline, whose time is zstd's own processor time.
set -euo pipefail

usage() {
    cat <<'EOF'
usage: bench/baselines.sh [--build DIR] [--scratch DIR] [--head N] FILE

Runs every baseline on FILE, a file of ITCH 5.0 messages, and prints a table:

  gzip -9, zstd -19, zstd --ultra -22, xz -9
        the whole file, one core each
  cpress mix9
        the text model, on the first N messages (default 30,000,000): it
        reads its whole input into memory
  the fair baseline
        cpress-columns splits FILE into a column for each field of each
        message type, in up to three encodings; each column is compressed
        with zstd -19, and the baseline is the sum of the smallest encoding
        of every column, with the three fields every message starts with
        counted once, as columns of each type or as one column across all
        types, whichever is smaller. It is a size and a compression speed:
        nothing puts the columns back together.

  --build DIR    where cpress and cpress-columns are (default: build)
  --scratch DIR  where to put temporary files (default: a new directory under
                 $TMPDIR, removed afterwards); given, it is left in place,
                 with the size of every column in columns.txt
  --head N       how many messages the text model gets

What it costs. The scratch directory needs about half the size of FILE: the
largest thing in it is gzip's output, and then the first N messages beside
their archive. The columns never touch the disk: each is piped from
cpress-columns into zstd, at the cost of a pass over FILE for each column.
On the first 30,000,000 messages of the brief's day, 951 MB, it took 2 hours
16 minutes of one core; for the whole day, 8.25 GB, expect about sixteen.

The row for cpress's own codec comes from cpress-bench --codec itch FILE.
EOF
}

build=build
scratch=
head=30000000
file=
while [ $# -gt 0 ]; do
    case "$1" in
        --build) build=${2:?--build needs a directory}; shift 2 ;;
        --scratch) scratch=${2:?--scratch needs a directory}; shift 2 ;;
        --head) head=${2:?--head needs a count}; shift 2 ;;
        -h | --help) usage; exit 0 ;;
        -*) usage >&2; exit 2 ;;
        *) file=$1; shift ;;
    esac
done
if [ -z "$file" ] || [ ! -f "$file" ]; then
    usage >&2
    exit 2
fi

# The two programs of this repository, wherever the generator put them.
find_program() {
    local name=$1 candidate
    for candidate in "$build/$name" "$build/Release/$name" "$build/$name.exe" \
        "$build/Release/$name.exe"; do
        if [ -x "$candidate" ]; then
            echo "$candidate"
            return
        fi
    done
    echo "baselines.sh: no $name in '$build'; build it, or pass --build DIR" >&2
    exit 1
}
cpress=$(find_program cpress)
columns=$(find_program cpress-columns)
for tool in gzip zstd xz awk; do
    if ! command -v "$tool" > /dev/null; then
        echo "baselines.sh: $tool is not installed" >&2
        exit 1
    fi
done

keep_scratch=yes
if [ -z "$scratch" ]; then
    scratch=$(mktemp -d)
    keep_scratch=no
fi
mkdir -p "$scratch"
cleanup() {
    rm -f "$scratch/packed" "$scratch/head" "$scratch/head.cpz" "$scratch/head.out"
    if [ "$keep_scratch" = no ]; then rm -rf "$scratch"; fi
}
trap cleanup EXIT

size_of() {
    wc -c < "$1" | tr -d ' '
}

# Runs a command and leaves the seconds it took, by the wall clock, in
# $elapsed.
timed() {
    local start end
    start=$(date +%s.%N)
    "$@"
    end=$(date +%s.%N)
    elapsed=$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.3f", b - a }')
}
to_file() {
    local out=$1
    shift
    "$@" > "$out"
}
to_nowhere() {
    "$@" > /dev/null
}

# row NAME ARCHIVE_BYTES COMPRESS_SECONDS DECOMPRESS_SECONDS INPUT_BYTES MESSAGES
# A time of 0 is a direction that was not run.
row() {
    awk -v name="$1" -v size="$2" -v c="$3" -v d="$4" -v bytes="$5" -v messages="$6" '
        function speed(seconds) {
            return seconds > 0 ? sprintf("%.2f", bytes / 1e6 / seconds) : "-"
        }
        BEGIN {
            printf "%-34s %14d %8.3f %10s %10s\n", name, size, size / messages, speed(c), speed(d)
        }'
}

bytes=$(size_of "$file")
messages=$("$columns" --count "$file" | awk '$1 == "all" { print $2 }')

echo "file      $file: $bytes bytes, $messages messages"
echo "machine   $(uname -srm), $(awk -F': *' '/model name/ { print $2; exit }' /proc/cpuinfo 2> /dev/null || true)"
echo "tools     $(gzip --version | head -n 1); $(zstd --version); $(xz --version | head -n 1); $("$cpress" --version)"
echo
printf "%-34s %14s %8s %10s %10s\n" baseline bytes B/msg "comp MB/s" "dec MB/s"

# A compressor that takes the whole file: NAME, then its command, which is
# given -c FILE to compress and -d -c to decompress.
whole_file() {
    local name=$1 compress_seconds size
    shift
    timed to_file "$scratch/packed" "$@" -c "$file"
    compress_seconds=$elapsed
    size=$(size_of "$scratch/packed")
    timed to_nowhere "$1" -d -c "$scratch/packed"
    rm -f "$scratch/packed"
    row "$name" "$size" "$compress_seconds" "$elapsed" "$bytes" "$messages"
}
whole_file "gzip -9" gzip -9
whole_file "zstd -19" zstd -19 -T1
whole_file "zstd --ultra -22" zstd --ultra -22 -T1
whole_file "xz -9" xz -9 -T1

# The text model, on what whole-file I/O can hold.
"$columns" --head "$head" "$file" "$scratch/head" > /dev/null
head_bytes=$(size_of "$scratch/head")
head_messages=$("$columns" --count "$scratch/head" | awk '$1 == "all" { print $2 }')
timed to_nowhere "$cpress" c --codec mix9 "$scratch/head" "$scratch/head.cpz"
mix9_seconds=$elapsed
timed to_nowhere "$cpress" d "$scratch/head.cpz" "$scratch/head.out"
row "cpress mix9, first $head_messages" "$(size_of "$scratch/head.cpz")" "$mix9_seconds" \
    "$elapsed" "$head_bytes" "$head_messages"
rm -f "$scratch/head" "$scratch/head.cpz" "$scratch/head.out"

# The fair baseline. One line for each column: its name, its size under
# zstd -19, and the processor seconds zstd spent, user and system. The
# splitter's own time is not the baseline's: a real tool would split once.
TIMEFORMAT='%3U %3S'
: > "$scratch/columns.txt"
for column in $("$columns" --list "$file"); do
    if ! measured=$({ "$columns" --column "$column" "$file" |
        { time zstd -19 -T1 -c | wc -c; }; } 2>&1); then
        echo "baselines.sh: column $column failed: $measured" >&2
        exit 1
    fi
    # wc's count and the two times are on lines of their own: join them.
    echo "$column" $measured |
        awk '{ printf "%s %d %.3f\n", $1, $2, $3 + $4 }' >> "$scratch/columns.txt"
done

# The smallest encoding of each column; then each of the fields that every
# type starts with once, as its columns by type or as its one column across
# all types, whichever is smaller. Prints the total and its seconds.
fair=$(awk '
    {
        key = $1
        if (key != "type.raw") sub(/\.[a-z]+$/, "", key)  # drops the encoding
        if (!(key in best) || $2 < best[key]) {
            best[key] = $2
            seconds[key] = $3
        }
    }
    END {
        for (key in best) {
            split(key, part, ".")
            if (part[1] == "all") {
                across[part[2]] = best[key]
                across_seconds[part[2]] = seconds[key]
            }
        }
        for (key in best) {
            split(key, part, ".")
            if (part[1] == "all") continue
            if (part[2] in across) {
                by_type[part[2]] += best[key]
                by_type_seconds[part[2]] += seconds[key]
            } else {
                total += best[key]
                total_seconds += seconds[key]
            }
        }
        for (field in across) {
            if (across[field] < by_type[field]) {
                total += across[field]
                total_seconds += across_seconds[field]
            } else {
                total += by_type[field]
                total_seconds += by_type_seconds[field]
            }
        }
        printf "%d %.3f\n", total, total_seconds
    }' "$scratch/columns.txt")
row "columns, zstd -19 each (fair)" "${fair% *}" "${fair#* }" 0 "$bytes" "$messages"
