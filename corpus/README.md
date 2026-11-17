# Corpus

Benchmark inputs live here and are not tracked by git.

| File | What it is | Where to get it |
|---|---|---|
| `enwik8` | First 100 MB of an English Wikipedia dump; the daily development target | <https://mattmahoney.net/dc/enwik8.zip> |
| `enwik9` | First 1 GB of the same dump; the file the Large Text Compression Benchmark ranks on | <https://mattmahoney.net/dc/enwik9.zip> |
| `12302019.NASDAQ_ITCH50` | One day of the NASDAQ TotalView-ITCH 5.0 feed, 30 December 2019; stage 5's benchmark | `12302019.NASDAQ_ITCH50.gz` at <https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/> |
| `07302019.NASDAQ_ITCH50` | Another day of the same feed, 30 July 2019; stage 5's second day, not used in design | `07302019.NASDAQ_ITCH50.gz`, same listing |

Unzip the files into this folder, then run:

```
cpress-bench corpus/enwik8
```

## The ITCH day file

| | Bytes | |
|---|---|---|
| `12302019.NASDAQ_ITCH50.gz` | 3,524,013,057 | MD5 `23514ef98821448a3f8f4c5b4095f3f5` |
| `12302019.NASDAQ_ITCH50` | 8,251,407,909 | 268,744,780 messages |
| `07302019.NASDAQ_ITCH50.gz` | 3,662,140,094 | MD5 `8744aba2ea125bfdde1a340ee2cea924` |
| `07302019.NASDAQ_ITCH50` | 8,661,679,413 | 282,229,684 messages |

The MD5s were computed on downloads made on 12 and 14 November 2026.
Nasdaq lists a `.md5sum` file beside each archive, but both return 404, so
there is nothing of theirs to compare with; `gzip -t` checks an archive
against its own CRC.

```
gzip -dk corpus/12302019.NASDAQ_ITCH50.gz
```

Each message in the file is preceded by its length as a 2-byte big-endian
integer. The other `NASDAQ_ITCH50` files in the listing are other days in
the same form; `07302019` is the one stage 5 took as its second day.

The first 30,000,000 messages are 951,200,923 bytes, which is small enough
for the codecs that read a whole file into memory:

```
cpress-columns --head 30000000 corpus/12302019.NASDAQ_ITCH50 corpus/itch-30m
```

`cpress-columns --count FILE` prints how many messages of each type a file
holds, and refuses a file that is not ITCH 5.0 from end to end.

## The baselines

```
bench/baselines.sh --build build corpus/12302019.NASDAQ_ITCH50
```

runs gzip, zstd, xz, `mix9` and the column-split baseline of the stage 5
brief on a file, one core each, and prints one table. It needs gzip, zstd,
xz and a POSIX shell (WSL or Git Bash on Windows). Check two things first:

- **Disk.** About half the file's size in the scratch directory (`--scratch`,
  by default under `$TMPDIR`): the largest compressed copy, and then the
  first 30,000,000 messages beside their archive. For the day file that is
  about 4 GB, on top of the 8.25 GB file itself. The columns are never
  written: each is piped from `cpress-columns` straight into zstd.
- **Time.** On the first 30,000,000 messages it took 2 hours 16 minutes
  of one core: the compressors of the whole file, `mix9`, and a pass over
  the file for each of its 320 columns. Expect about sixteen hours for the
  day file.

`--jobs N` runs up to N of those at once, each still on one core, and needs
bash 4.3 or later. The time is then that of the longest, `zstd --ultra
-22`, given cores for the columns beside it: on the day file, with
`--jobs 12`, 2 hours 31 minutes. The scratch directory can hold every
whole-file compressor's output at once, about one and a half times the
file's size. The sizes are the same; the wall-clock speeds are not, since
the jobs share the caches and memory, so take speeds from a run with
`--jobs 1`, on a slice: the README's are from the first 30,000,000
messages. A baseline's size does not change with cpress's code, so it is
measured once per file and tool version, not once per commit.
