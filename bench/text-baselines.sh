#!/usr/bin/env bash
# The text baselines of stage 6: what other people's compressors make of
# one file, usually enwik8, and how fast, each alone on one pinned logical
# processor. They are commands, not dependencies: nothing links them and
# the build does not need them. Linux only: it pins with taskset and reads
# peak memory from GNU time.
set -euo pipefail

usage() {
    cat <<'EOF'
usage: bench/text-baselines.sh [--cpu N] [--repeat K] [--scratch DIR]
                               [--bsc PROGRAM] [--lpaq1 PROGRAM] FILE

Compresses and decompresses FILE with each program below, K times each way
(default 3), on logical processor N alone (default 2), and prints one CSV
row per program: the archive's size, the fastest time each way in
nanoseconds per byte of FILE, and the peak memory each way. Every round
trip is compared with FILE byte for byte; one that differs stops the
script.

  gzip -9
  bzip2 -9
  zstd --ultra -22                                   one thread
  xz --lzma2=preset=9e,dict=1GiB,lc=4,pb=0           one thread
  brotli -q 11 -w 24
  7-Zip, PPMd: -m0=PPMd:mem=1630m:o=10               one thread
  bsc -b1000 -e2                                     one thread; with --bsc
  lpaq1 9                                            with --lpaq1

The options are the Large Text Compression Benchmark's for each program
(https://mattmahoney.net/dc/text.html), so that a size can be checked
against its row there. bsc and lpaq1 are built from source; pass the
programs.

  --cpu N        the logical processor every program runs on
  --repeat K     round trips per program; the fastest of each direction counts
  --scratch DIR  where FILE is copied and the archives go (default: a new
                 directory under $TMPDIR, removed afterwards). Use a local
                 disk: a program reading through a network or 9P mount, as
                 /mnt/c is under WSL, is timed on the mount.

What it costs. The scratch directory holds FILE, one archive and one
restored copy. xz at these options needs about 11 GiB of memory. On
enwik8, at --repeat 3, the eight programs took 25 minutes.

cpress's own rows come from cpress-bench --cpu N --repeat K FILE, in the
same sitting.
EOF
}

cpu=2
repeat=3
scratch=
bsc=
lpaq1=
file=
while [ $# -gt 0 ]; do
    case "$1" in
        --cpu) cpu=${2:?--cpu needs a processor number}; shift 2 ;;
        --repeat) repeat=${2:?--repeat needs a count}; shift 2 ;;
        --scratch) scratch=${2:?--scratch needs a directory}; shift 2 ;;
        --bsc) bsc=${2:?--bsc needs a program}; shift 2 ;;
        --lpaq1) lpaq1=${2:?--lpaq1 needs a program}; shift 2 ;;
        -h | --help) usage; exit 0 ;;
        -*) usage >&2; exit 2 ;;
        *) file=$1; shift ;;
    esac
done
if [ -z "$file" ] || [ ! -f "$file" ]; then
    usage >&2
    exit 2
fi
case "$repeat" in
    '' | *[!0-9]* | 0) echo "text-baselines.sh: --repeat needs a whole number above 0" >&2; exit 2 ;;
esac
for tool in taskset /usr/bin/time gzip bzip2 zstd xz brotli 7z cmp awk; do
    if ! command -v "$tool" > /dev/null; then
        echo "text-baselines.sh: $tool is not installed" >&2
        exit 1
    fi
done
for program in "$bsc" "$lpaq1"; do
    if [ -n "$program" ] && [ ! -x "$program" ]; then
        echo "text-baselines.sh: $program is not a program" >&2
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
    rm -f "$scratch/input" "$scratch/archive.out" "$scratch/restored" "$scratch/seconds" \
        "$scratch/kib"
    if [ "$keep_scratch" = no ]; then rm -rf "$scratch"; fi
}
trap cleanup EXIT

# Every program from here on runs on one logical processor: this shell's
# affinity is inherited by everything it starts.
taskset -pc "$cpu" $$ > /dev/null

# The first line PROGRAM prints that holds PATTERN: its name and version.
# Some programs print their banner and then fail, as bsc does without
# arguments; that is not a failure here.
banner() {
    local pattern=$1
    shift
    { "$@" 2>&1 || true; } | grep -m 1 -e "$pattern" || true
}

# The programs, in the order of the table. Each command is run by bash -c,
# with the file to read as $1 and the file to write as $2, and execs the
# program, so that the peak memory GNU time reports is the program's own.
names=(gzip bzip2 zstd xz brotli 7zip)
declare -A options compress decompress version
options[gzip]='-9'
compress[gzip]='exec gzip -9 -c "$1" > "$2"'
decompress[gzip]='exec gzip -d -c "$1" > "$2"'
version[gzip]=$(banner gzip gzip --version)

options[bzip2]='-9'
compress[bzip2]='exec bzip2 -9 -c "$1" > "$2"'
decompress[bzip2]='exec bzip2 -d -c "$1" > "$2"'
version[bzip2]=$(banner Version bzip2 --help)

options[zstd]='--ultra -22'
compress[zstd]='exec zstd -q --ultra -22 -T1 -c "$1" > "$2"'
decompress[zstd]='exec zstd -q -d -c "$1" > "$2"'
version[zstd]=$(banner Zstandard zstd --version)

options[xz]='--lzma2=preset=9e,dict=1GiB,lc=4,pb=0'
compress[xz]='exec xz -T1 --lzma2=preset=9e,dict=1GiB,lc=4,pb=0 -c "$1" > "$2"'
decompress[xz]='exec xz -d -c "$1" > "$2"'
version[xz]=$(banner xz xz --version)

options[brotli]='-q 11 -w 24'
compress[brotli]='exec brotli -q 11 -w 24 -c "$1" > "$2"'
decompress[brotli]='exec brotli -d -c "$1" > "$2"'
version[brotli]=$(banner brotli brotli --version)

# 7-Zip adds to an archive that exists, so the old one goes first, and
# names it .7z unless its name has an extension, which archive.out has.
options[7zip]='-m0=PPMd:mem=1630m:o=10'
compress[7zip]='rm -f "$2"; exec 7z a -bso0 -bsp0 -t7z -m0=PPMd:mem=1630m:o=10 -mmt=1 "$2" "$1"'
decompress[7zip]='exec 7z e -bso0 -bsp0 -so "$1" > "$2"'
version[7zip]=$(banner 7-Zip 7z)

if [ -n "$bsc" ]; then
    names+=(bsc)
    options[bsc]='-b1000 -e2'
    compress[bsc]="exec '$bsc' e \"\$1\" \"\$2\" -b1000 -e2 -T > /dev/null"
    decompress[bsc]="exec '$bsc' d \"\$1\" \"\$2\" -T > /dev/null"
    version[bsc]=$(banner Version "$bsc")
fi
if [ -n "$lpaq1" ]; then
    names+=(lpaq1)
    options[lpaq1]='9'
    compress[lpaq1]="exec '$lpaq1' 9 \"\$1\" \"\$2\" > /dev/null"
    decompress[lpaq1]="exec '$lpaq1' d \"\$1\" \"\$2\" > /dev/null"
    version[lpaq1]='lpaq1 (Matt Mahoney, 24 July 2007)'
fi

# Runs COMMAND with $1 and $2 set to its two files, and leaves the
# milliseconds it took by the wall clock in $milliseconds and its peak
# memory in KiB in $kib. Bash's `time` reports to the group's standard
# error, so the command's own is passed round it on descriptor 3.
run() {
    local TIMEFORMAT=%3R
    { time /usr/bin/time -f %M -o "$scratch/kib" bash -c "$1" _ "$2" "$3" 2>&3; } \
        3>&2 2> "$scratch/seconds"
    milliseconds=$(awk '{ printf "%d", $1 * 1000 + 0.5 }' "$scratch/seconds")
    kib=$(< "$scratch/kib")
}

size_of() {
    wc -c < "$1" | tr -d ' '
}

# A field of free text, quoted for CSV.
quoted() {
    printf '"%s"' "${1//\"/\"\"}"
}

cp "$file" "$scratch/input"
bytes=$(size_of "$scratch/input")

echo "# file: $file, $bytes bytes"
echo "# machine: $(uname -srm), $(awk -F': *' '/model name/ { print $2; exit }' /proc/cpuinfo)"
echo "# logical processor $cpu, fastest of $repeat each way"
echo "program,version,options,bytes,archive_bytes,compress_ns_per_byte,decompress_ns_per_byte,compress_peak_mib,decompress_peak_mib"

for name in "${names[@]}"; do
    best_compress=
    best_decompress=
    compress_kib=0
    decompress_kib=0
    for ((round = 1; round <= repeat; ++round)); do
        run "${compress[$name]}" "$scratch/input" "$scratch/archive.out"
        if [ -z "$best_compress" ] || [ "$milliseconds" -lt "$best_compress" ]; then
            best_compress=$milliseconds
        fi
        if [ "$kib" -gt "$compress_kib" ]; then compress_kib=$kib; fi

        rm -f "$scratch/restored"
        run "${decompress[$name]}" "$scratch/archive.out" "$scratch/restored"
        if [ -z "$best_decompress" ] || [ "$milliseconds" -lt "$best_decompress" ]; then
            best_decompress=$milliseconds
        fi
        if [ "$kib" -gt "$decompress_kib" ]; then decompress_kib=$kib; fi

        if ! cmp -s "$scratch/input" "$scratch/restored"; then
            echo "text-baselines.sh: $name did not give back $file" >&2
            exit 1
        fi
    done
    archive_bytes=$(size_of "$scratch/archive.out")
    printf '%s,%s,%s,%s,%s,' "$name" "$(quoted "${version[$name]}")" \
        "$(quoted "${options[$name]}")" "$bytes" "$archive_bytes"
    awk -v c="$best_compress" -v d="$best_decompress" -v bytes="$bytes" \
        -v ck="$compress_kib" -v dk="$decompress_kib" 'BEGIN {
            printf "%.1f,%.1f,%.1f,%.1f\n", c * 1e6 / bytes, d * 1e6 / bytes, ck / 1024, dk / 1024
        }'
    rm -f "$scratch/archive.out" "$scratch/restored"
done
