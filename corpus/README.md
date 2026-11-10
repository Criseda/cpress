# Corpus

Benchmark inputs live here and are not tracked by git.

| File | What it is | Where to get it |
|---|---|---|
| `enwik8` | First 100 MB of an English Wikipedia dump; the daily development target | <https://mattmahoney.net/dc/enwik8.zip> |
| `enwik9` | First 1 GB of the same dump; the file the Large Text Compression Benchmark ranks on | <https://mattmahoney.net/dc/enwik9.zip> |
| NASDAQ ITCH samples | Full-day exchange feed files for the market-data stage | <https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/> |

Unzip the files into this folder, then run:

```
cpress-bench corpus/enwik8
```
