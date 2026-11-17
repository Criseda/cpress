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
usage: bench/baselines.sh [--build DIR] [--scratch DIR] [--head N] [--jobs N] FILE

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
  --jobs N       run up to N baselines and columns at once, each still on one
                 core (default 1; more than 1 needs bash 4.3 or later)

What it costs. The scratch directory needs about half the size of FILE: the
largest thing in it is gzip's output, and then the first N messages beside
their archive. The columns never touch the disk: each is piped from
cpress-columns into zstd, at the cost of a pass over FILE for each column.
On the first 30,000,000 messages of the brief's day, 951 MB, it took 2 hours
16 minutes of one core; for the whole day, 8.25 GB, expect about sixteen.

With --jobs, the time is that of the longest baseline, zstd --ultra -22,
once there are cores for the columns beside it: on the whole day, with
--jobs 12, 2 hours 31 minutes. The scratch directory can hold every
whole-file compressor's output at once, about one and a half times the
size of FILE. The sizes are the same. The speeds are not: the jobs share
the machine's caches and memory, so the wall-clock speeds of the
whole-file compressors are lower than each would be alone. The fair
baseline's speed is processor time, which sharing changes less.

The row for cpress's own codec comes from cpress-bench --codec itch FILE.
EOF
}

build=build
scratch=
head=30000000
jobs=1
file=
while [ $# -gt 0 ]; do
    case "$1" in
        --build) build=${2:?--build needs a directory}; shift 2 ;;
        --scratch) scratch=${2:?--scratch needs a directory}; shift 2 ;;
        --head) head=${2:?--head needs a count}; shift 2 ;;
        --jobs) jobs=${2:?--jobs needs a count}; shift 2 ;;
        -h | --help) usage; exit 0 ;;
        -*) usage >&2; exit 2 ;;
        *) file=$1; shift ;;
    esac
done
if [ -z "$file" ] || [ ! -f "$file" ]; then
    usage >&2
    exit 2
fi
case "$jobs" in
    '' | *[!0-9]* | 0) echo "baselines.sh: --jobs needs a whole number above 0" >&2; exit 2 ;;
esac
# wait -n, which the pool below waits with, came in bash 4.3.
if [ "$jobs" -gt 1 ] &&
    { [ "${BASH_VERSINFO[0]}" -lt 4 ] ||
        { [ "${BASH_VERSINFO[0]}" -eq 4 ] && [ "${BASH_VERSINFO[1]}" -lt 3 ]; }; }; then
    echo "baselines.sh: --jobs above 1 needs bash 4.3 or later; this is $BASH_VERSION" >&2
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

# Stops a process and every process it started. A job is a subshell, and
# stopping only the subshell would leave its compressor running.
stop_tree() {
    local child
    for child in $(pgrep -P "$1" 2> /dev/null || true); do
        stop_tree "$child"
    done
    kill "$1" 2> /dev/null || true
}
cleanup() {
    local job
    for job in $(jobs -p); do
        stop_tree "$job"
    done
    rm -f "$scratch"/*.packed "$scratch"/*.seconds "$scratch"/*.row "$scratch"/*.column \
        "$scratch/head" "$scratch/head.cpz" "$scratch/head.out"
    if [ "$keep_scratch" = no ]; then rm -rf "$scratch"; fi
}
trap cleanup EXIT

# The pool. start runs a command as a job, first waiting for one to finish
# if $jobs are running; finish waits for the rest. A job that fails stops
# the script, through set -e, when it is waited for. With one job at a time
# start just runs the command, and the script needs nothing bash 3 lacks.
# Every job keeps its files in the scratch directory under names of its
# own, so that jobs running at once never share one.
running=0
start() {
    if [ "$jobs" -eq 1 ]; then
        "$@"
        return
    fi
    if [ "$running" -ge "$jobs" ]; then
        wait -n
        running=$((running - 1))
    fi
    "$@" &
    running=$((running + 1))
}
finish() {
    while [ "$running" -gt 0 ]; do
        wait -n
        running=$((running - 1))
    done
}

size_of() {
    wc -c < "$1" | tr -d ' '
}

# Runs a command and leaves the seconds it took, by the wall clock, in
# $elapsed; TAG names the job, and so the file the seconds pass through.
# Bash's own `time` gives milliseconds everywhere, where `date` gives only
# whole seconds on macOS. Its report goes to the group's standard error, so
# the command's own is passed round it on descriptor 3.
timed() {
    local tag=$1 TIMEFORMAT=%3R
    shift
    { time "$@" 2>&3; } 3>&2 2> "$scratch/$tag.seconds"
    elapsed=$(< "$scratch/$tag.seconds")
    rm -f "$scratch/$tag.seconds"
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
echo "jobs      $jobs at once"
echo
printf "%-34s %14s %8s %10s %10s\n" baseline bytes B/msg "comp MB/s" "dec MB/s"

# A compressor that takes the whole file, as job NUMBER: NAME, then its
# command, which is given -c FILE to compress and -d -c to decompress. Its
# row goes to NUMBER.row, printed in order once every job is done.
whole_file() {
    local number=$1 name=$2 compress_seconds size
    shift 2
    local packed="$scratch/$number.packed"
    timed "$number" to_file "$packed" "$@" -c "$file"
    compress_seconds=$elapsed
    size=$(size_of "$packed")
    timed "$number" to_nowhere "$1" -d -c "$packed"
    rm -f "$packed"
    row "$name" "$size" "$compress_seconds" "$elapsed" "$bytes" "$messages" > "$scratch/$number.row"
}

# The text model, as job 5, on what whole-file I/O can hold.
text_model() {
    local head_bytes head_messages mix9_seconds
    "$columns" --head "$head" "$file" "$scratch/head" > /dev/null
    head_bytes=$(size_of "$scratch/head")
    head_messages=$("$columns" --count "$scratch/head" | awk '$1 == "all" { print $2 }')
    timed 5 to_nowhere "$cpress" c --codec mix9 "$scratch/head" "$scratch/head.cpz"
    mix9_seconds=$elapsed
    timed 5 to_nowhere "$cpress" d "$scratch/head.cpz" "$scratch/head.out"
    row "cpress mix9, first $head_messages" "$(size_of "$scratch/head.cpz")" "$mix9_seconds" \
        "$elapsed" "$head_bytes" "$head_messages" > "$scratch/5.row"
    rm -f "$scratch/head" "$scratch/head.cpz" "$scratch/head.out"
}

# One column of the fair baseline, as a job: a line of its name, its size
# under zstd -19, and the processor seconds zstd spent, user and system, in
# NAME.column. Only zstd is timed: the splitter's time is not the
# baseline's, as a real tool would split once, and wc's is not either. The
# times go to a file of their own, and the commands' standard error to the
# script's, so that nothing they print is read as a size or a time.
one_column() {
    local name=$1 size TIMEFORMAT='%3U %3S'
    if ! size=$("$columns" --column "$name" "$file" |
        { time zstd -19 -T1 -c 2>&3; } 3>&2 2> "$scratch/$name.seconds" | wc -c); then
        echo "baselines.sh: column $name failed" >&2
        exit 1
    fi
    echo "$name" "$size" "$(< "$scratch/$name.seconds")" |
        awk '{ printf "%s %d %.3f\n", $1, $2, $3 + $4 }' > "$scratch/$name.column"
    rm -f "$scratch/$name.seconds"
}

# The longest first, so that with a few jobs the short ones fill in beside
# them.
start whole_file 3 "zstd --ultra -22" zstd --ultra -22 -T1
start whole_file 4 "xz -9" xz -9 -T1
start whole_file 2 "zstd -19" zstd -19 -T1
start whole_file 1 "gzip -9" gzip -9
start text_model
column_names=$("$columns" --list "$file")
for name in $column_names; do
    start one_column "$name"
done
finish

for number in 1 2 3 4 5; do
    cat "$scratch/$number.row"
    rm -f "$scratch/$number.row"
done
: > "$scratch/columns.txt"
for name in $column_names; do
    cat "$scratch/$name.column" >> "$scratch/columns.txt"
    rm -f "$scratch/$name.column"
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
