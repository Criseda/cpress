# Corpus

Benchmark inputs live here and are not tracked by git.

| File | What it is | Where to get it |
|---|---|---|
| `enwik8` | First 100 MB of an English Wikipedia dump; the daily development target | <https://mattmahoney.net/dc/enwik8.zip> |
| `enwik9` | First 1 GB of the same dump; the file the Large Text Compression Benchmark ranks on | <https://mattmahoney.net/dc/enwik9.zip> |
| `12302019.NASDAQ_ITCH50` | One day of the NASDAQ TotalView-ITCH 5.0 feed, 30 December 2019; stage 5's benchmark | `12302019.NASDAQ_ITCH50.gz` at <https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/> |

Unzip the files into this folder, then run:

```
cpress-bench corpus/enwik8
```

## The ITCH day file

| | Bytes | |
|---|---|---|
| `12302019.NASDAQ_ITCH50.gz` | 3,524,013,057 | MD5 `23514ef98821448a3f8f4c5b4095f3f5` |
| `12302019.NASDAQ_ITCH50` | 8,251,407,909 | 268,744,780 messages |

The MD5 was computed on a download made on 12 November 2026. Nasdaq lists a
`.md5sum` file beside the archive, but it returns 404, so there is nothing
of theirs to compare with; `gzip -t` checks the archive against its own CRC.

```
gzip -dk corpus/12302019.NASDAQ_ITCH50.gz
```

Each message in the file is preceded by its length as a 2-byte big-endian
integer. The other `NASDAQ_ITCH50` files in the listing are other days in
the same form; the stage 5 brief asks for one of them as a second day.

Stage 5's baseline script splits the file into one column per field, about
24 GB more while it runs, and takes about nine hours of one core on the day
file. Check the disk first.

The first 30,000,000 messages are 951,200,923 bytes, which is small enough
for the codecs that read a whole file into memory:

```
head -c 951200923 corpus/12302019.NASDAQ_ITCH50 > corpus/itch-30m
```
