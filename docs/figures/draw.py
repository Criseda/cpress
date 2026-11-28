#!/usr/bin/env python3
"""Draws the figures of docs/write-up.md and the README.

Each figure is read from a table beside this script and written twice, as
NAME-light.svg and NAME-dark.svg, for GitHub's two themes. Standard library
only. The same tables give the same bytes: nothing here depends on the
time, the machine or the order of a dictionary.

    python3 docs/figures/draw.py

Every chart has one point: cpress against the rest, or one design against
the last. So every chart is drawn the same way: cpress, or the design, in
one accent colour, everything else in grey, and a number not measured on
this project's machine as a hollow mark. The diagrams, which explain how a
part works rather than plot a number, follow the same rule: the part being
explained in the accent colour, what surrounds it in grey.
"""

import csv
import math
from dataclasses import dataclass
from pathlib import Path
from xml.sax.saxutils import escape

HERE = Path(__file__).resolve().parent


@dataclass(frozen=True)
class Theme:
    name: str
    surface: str
    primary: str  # titles and the values the figure is about
    secondary: str  # subtitles and labels
    muted: str  # axis ticks and notes
    grid: str
    axis: str
    accent: str  # cpress
    other: str  # everything else
    panel: str  # the fill of a box in a diagram
    accent_soft: str  # the fill of the box a diagram is about


# The colours of the dataviz reference palette: series slot 1 and the muted
# grey, each validated for colour-blind separation and contrast on its
# surface. The two fills are tints of the surface and of the accent, light
# enough that primary text on them keeps its contrast.
LIGHT = Theme("light", "#fcfcfb", "#0b0b0b", "#52514e", "#898781", "#e1e0d9", "#c3c2b7",
              "#2a78d6", "#898781", "#f1f0ec", "#dde9f8")
DARK = Theme("dark", "#1a1a19", "#f0efec", "#c3c2b7", "#898781", "#2c2c2a", "#383835",
             "#3987e5", "#898781", "#242423", "#1c2f4a")

WIDTH = 760
PAD = 24
TITLE_Y = 34
SUBTITLE_Y = 56
PLOT_TOP = 84
FONT = "system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif"
MONO = "ui-monospace, 'Cascadia Mono', Consolas, Menlo, monospace"

# Text is measured by a rule, not by a font: about this many pixels a
# character at 13 px. Generous, so a label never runs into its neighbour.
CHAR_WIDTH_13PX = 7.2

BAR = 20  # bar thickness
ROW = 32  # bar row height: the bar and the air around it
RADIUS = 4  # the rounded data end of a bar
DOT = 4.5  # marker radius


def table(name):
    """The rows of NAME.csv, skipping the comment lines that cite its source."""
    with open(HERE / f"{name}.csv", newline="", encoding="utf-8") as f:
        lines = [line for line in f if not line.startswith("#")]
    return list(csv.DictReader(lines))


def n(value):
    """A coordinate, to a tenth of a pixel, without a trailing .0."""
    text = f"{value:.1f}"
    return text[:-2] if text.endswith(".0") else text


def text_width(text, size=13):
    return len(text) * CHAR_WIDTH_13PX * size / 13


class Svg:
    """An SVG document, built element by element in drawing order."""

    def __init__(self, height, theme, title, description):
        self.height = height
        self.theme = theme
        self.title = title
        self.description = description
        self.parts = []

    def rect(self, x, y, w, h, fill, rx=0):
        corner = f' rx="{n(rx)}"' if rx else ""
        self.parts.append(f'<rect x="{n(x)}" y="{n(y)}" width="{n(w)}" height="{n(h)}"'
                          f'{corner} fill="{fill}"/>')

    def line(self, x1, y1, x2, y2, stroke, width=1):
        self.parts.append(f'<line x1="{n(x1)}" y1="{n(y1)}" x2="{n(x2)}" y2="{n(y2)}" '
                          f'stroke="{stroke}" stroke-width="{n(width)}"/>')

    def dot(self, x, y, colour, hollow=False):
        # A 2 px ring of the surface keeps a dot clear of what it crosses.
        fill = self.theme.surface if hollow else colour
        self.parts.append(f'<circle cx="{n(x)}" cy="{n(y)}" r="{n(DOT + 1)}" '
                          f'fill="{self.theme.surface}"/>')
        self.parts.append(f'<circle cx="{n(x)}" cy="{n(y)}" r="{n(DOT)}" fill="{fill}" '
                          f'stroke="{colour}" stroke-width="{n(2 if hollow else 0)}"/>')

    def bar(self, x0, y, x1, h, fill):
        """A horizontal bar from x0, square at the baseline, rounded at its end."""
        r = min(RADIUS, (x1 - x0) / 2, h / 2)
        self.parts.append(
            f'<path d="M{n(x0)} {n(y)}H{n(x1 - r)}A{n(r)} {n(r)} 0 0 1 {n(x1)} {n(y + r)}'
            f'V{n(y + h - r)}A{n(r)} {n(r)} 0 0 1 {n(x1 - r)} {n(y + h)}H{n(x0)}Z" '
            f'fill="{fill}"/>')

    def box(self, x, y, w, h, fill, stroke, rx=6):
        self.parts.append(f'<rect x="{n(x)}" y="{n(y)}" width="{n(w)}" height="{n(h)}" '
                          f'rx="{n(rx)}" fill="{fill}" stroke="{stroke}" stroke-width="1"/>')

    def polyline(self, points, stroke, width=1.5):
        d = "M" + "L".join(f"{n(x)} {n(y)}" for x, y in points)
        self.parts.append(f'<path d="{d}" fill="none" stroke="{stroke}" '
                          f'stroke-width="{n(width)}" stroke-linejoin="round"/>')

    def arrow(self, x1, y1, x2, y2, colour, width=1.5):
        """A straight line from (x1, y1) whose head touches (x2, y2)."""
        length = math.hypot(x2 - x1, y2 - y1)
        ux, uy = (x2 - x1) / length, (y2 - y1) / length
        head, half = 8, 4
        bx, by = x2 - ux * head, y2 - uy * head
        self.line(x1, y1, bx, by, colour, width)
        self.parts.append(f'<path d="M{n(x2)} {n(y2)}L{n(bx - uy * half)} {n(by + ux * half)}'
                          f'L{n(bx + uy * half)} {n(by - ux * half)}Z" fill="{colour}"/>')

    def text(self, x, y, content, colour, size=13, anchor="start", weight=400, numeric=False,
             mono=False):
        attributes = f' text-anchor="{anchor}"' if anchor != "start" else ""
        if weight != 400:
            attributes += f' font-weight="{weight}"'
        if numeric:
            attributes += ' class="num"'
        if mono:
            # An inline style, because the sheet's rule for text outranks an
            # attribute.
            attributes += f' style="font-family: {MONO}"'
        self.parts.append(f'<text x="{n(x)}" y="{n(y)}" font-size="{size}" fill="{colour}"'
                          f'{attributes}>{escape(content)}</text>')

    def heading(self, title, subtitle):
        self.text(PAD, TITLE_Y, title, self.theme.primary, size=17, weight=600)
        self.text(PAD, SUBTITLE_Y, subtitle, self.theme.secondary)

    def render(self):
        t = self.theme
        head = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" '
                f'height="{n(self.height)}" viewBox="0 0 {WIDTH} {n(self.height)}" '
                f'role="img" aria-labelledby="title desc">\n'
                f'<title id="title">{escape(self.title)}</title>\n'
                f'<desc id="desc">{escape(self.description)}</desc>\n'
                f'<style>text {{ font-family: {FONT}; }} '
                f'.num {{ font-variant-numeric: tabular-nums; }}</style>\n'
                f'<rect width="{WIDTH}" height="{n(self.height)}" rx="8" fill="{t.surface}"/>\n')
        return head + "\n".join(self.parts) + "\n</svg>\n"


@dataclass(frozen=True)
class BarRow:
    label: str
    value: float
    value_text: str
    accent: bool


def bar_chart(theme, title, subtitle, rows, axis_max, tick_step, tick_text, axis_label,
              description):
    """Horizontal bars on one baseline, labelled on the left, valued at the tip."""
    label_width = max(text_width(row.label) for row in rows) + 16
    x0 = PAD + label_width
    value_room = max(text_width(row.value_text) for row in rows) + 12
    x_max = WIDTH - PAD - value_room
    scale = (x_max - x0) / axis_max
    plot_bottom = PLOT_TOP + ROW * len(rows)
    svg = Svg(plot_bottom + 56, theme, title, description)
    svg.heading(title, subtitle)

    tick = 0.0
    while tick <= axis_max + 1e-9:
        x = x0 + tick * scale
        svg.line(x, PLOT_TOP - 6, x, plot_bottom, theme.grid)
        svg.text(x, plot_bottom + 18, tick_text(tick), theme.muted, size=12, anchor="middle",
                 numeric=True)
        tick += tick_step
    svg.text(x0, plot_bottom + 38, axis_label, theme.muted, size=12)

    for i, row in enumerate(rows):
        y = PLOT_TOP + i * ROW + (ROW - BAR) / 2
        middle = y + BAR / 2 + 4.5
        colour = theme.accent if row.accent else theme.other
        svg.text(x0 - 12, middle, row.label, theme.secondary, anchor="end")
        end = x0 + row.value * scale
        svg.bar(x0, y, end, BAR, colour)
        svg.text(end + 8, middle, row.value_text, theme.primary if row.accent else theme.secondary,
                 numeric=True, weight=600 if row.accent else 400)
    svg.line(x0, PLOT_TOP - 6, x0, plot_bottom, theme.axis)
    return svg


def understanding(theme):
    rows = []
    for r in table("itch-slice"):
        per_message = int(r["bytes"]) / int(r["messages"])
        rows.append(BarRow(r["compressor"], per_message, f"{per_message:.2f}", r["cpress"] == "yes"))
    return bar_chart(
        theme,
        "Understanding the data: bytes per ITCH message",
        "The first 30 million messages of a NASDAQ trading day, 951 MB. Shorter is smaller.",
        rows, axis_max=14, tick_step=2, tick_text=lambda t: f"{t:.0f}",
        axis_label="bytes per message",
        description="Bytes per message on the first 30,000,000 messages of the NASDAQ "
        "TotalView-ITCH 5.0 feed of 30 December 2019: " +
        "; ".join(f"{row.label} {row.value_text}" for row in rows) +
        ". cpress mix9 sees bytes; cpress itch uses the same coder and mixer on the orders "
        "the messages describe.")


def stages(theme):
    rows = []
    for r in table("stages"):
        megabytes = int(r["bytes"]) / 1e6
        rows.append(BarRow(r["label"], megabytes, f"{megabytes:.1f} MB", r["cpress"] == "yes"))
    return bar_chart(
        theme, "enwik8, stage by stage",
        "The first 100 MB of English Wikipedia, after each stage's codec.",
        rows, axis_max=100, tick_step=20, tick_text=lambda t: f"{t:.0f}",
        axis_label="megabytes (millions of bytes)",
        description="Size of enwik8 after each codec: " +
        "; ".join(f"{row.label} {row.value_text}" for row in rows) + ".")


def itch_parts(theme):
    rows = []
    for r in table("itch-parts"):
        per_message = int(r["bytes"]) / int(r["messages"])
        rows.append(BarRow(r["label"], per_message, f"{per_message:.3f}", False))
    rows[-1] = BarRow(rows[-1].label, rows[-1].value, rows[-1].value_text, True)
    return bar_chart(
        theme, "What each part of the ITCH model is worth",
        "A whole trading day, 268,744,780 messages; each row adds one thing to the row above.",
        rows, axis_max=7, tick_step=1, tick_text=lambda t: f"{t:.0f}",
        axis_label="bytes per message",
        description="Bytes per message of the ITCH model as parts are added: " +
        "; ".join(f"{row.label} {row.value_text}" for row in rows) + ".")


def itch_bytes(theme):
    rows = table("itch-bytes")
    total = sum(float(r["bits_per_message"]) for r in rows)
    shown, folded = [], 0.0
    for r in rows:
        bits = float(r["bits_per_message"])
        # Fields under 1% each are folded into one row, as the table's own
        # last row already is.
        if bits / total < 0.01 or r["field"] == "Everything else":
            folded += bits
        else:
            shown.append((r["field"], bits))
    shown.append(("Everything else", folded))
    bars = [BarRow(field, bits, f"{bits:.2f} bits, {100 * bits / total:.0f}%", field == "Timestamp")
            for field, bits in shown]
    return bar_chart(
        theme, "Where the bytes go: half of every message is its time",
        f"Bits per message in the ITCH model, a whole trading day: {total:.2f} in all.",
        bars, axis_max=18, tick_step=2, tick_text=lambda t: f"{t:.0f}",
        axis_label="bits per message",
        description="Bits per message by field in the ITCH model over a whole day: " +
        "; ".join(f"{row.label} {row.value_text}" for row in bars) + ".")


def memory(theme):
    """Dumbbells: v0.3 and stage 4 at three table sizes, each direction."""
    rows = table("memory")
    by_key = {(r["level"], r["build"]): r for r in rows}
    levels = ["23", "16", "12"]
    names = {"23": "Level 23, as shipped", "16": "Level 16, tables in cache",
             "12": "Level 12, tables in cache"}
    directions = [("compress_ns_per_byte", "Compressing"), ("decompress_ns_per_byte", "Decompressing")]
    lines = []
    for column, direction in directions:
        for level in levels:
            before = float(by_key[(level, "v0.3")][column])
            after = float(by_key[(level, "stage 4")][column])
            lines.append((direction, names[level], before, after))

    title = "Waiting for memory: what stage 4 took away"
    subtitle = "mix9 on enwik8, nanoseconds per byte; smaller tables fit in the caches."
    label_width = max(text_width(name) for _, name, _, _ in lines) + 16
    x0 = PAD + label_width
    x1 = WIDTH - PAD - 70
    low, high = 400, 750
    scale = (x1 - x0) / (high - low)
    group_gap = 26
    top = PLOT_TOP + 28  # room for the legend
    plot_bottom = top + ROW * len(lines) + group_gap * (len(directions) - 1) + 8
    svg = Svg(plot_bottom + 56, theme, title,
              "Nanoseconds per byte of mix9 on enwik8, v0.3 against stage 4, at table levels "
              "23, 16 and 12: " + "; ".join(f"{d} {name}: {b:.1f} to {a:.1f}"
                                             for d, name, b, a in lines) + ".")
    svg.heading(title, subtitle)

    svg.dot(x0 + 6, PLOT_TOP + 2, theme.other)
    svg.text(x0 + 16, PLOT_TOP + 6.5, "v0.3", theme.secondary)
    svg.dot(x0 + 70, PLOT_TOP + 2, theme.accent)
    svg.text(x0 + 80, PLOT_TOP + 6.5, "stage 4, the same archive", theme.secondary)

    tick = low
    while tick <= high:
        x = x0 + (tick - low) * scale
        svg.line(x, top - 6, x, plot_bottom, theme.grid)
        svg.text(x, plot_bottom + 18, f"{tick}", theme.muted, size=12, anchor="middle",
                 numeric=True)
        tick += 50
    svg.text(x0, plot_bottom + 38, "nanoseconds per byte", theme.muted, size=12)

    y = top
    last_direction = None
    for direction, name, before, after in lines:
        if direction != last_direction:
            if last_direction is not None:
                y += group_gap
            svg.text(PAD, y + 2, direction, theme.primary, size=12, weight=600)
            y += 8
            last_direction = direction
        middle = y + ROW / 2
        svg.text(x0 - 12, middle + 4.5, name, theme.secondary, anchor="end")
        xb = x0 + (before - low) * scale
        xa = x0 + (after - low) * scale
        svg.line(xa, middle, xb, middle, theme.axis, width=2)
        svg.dot(xb, middle, theme.other)
        svg.dot(xa, middle, theme.accent)
        svg.text(xb + 12, middle + 4.5, f"−{before - after:.0f}", theme.secondary, numeric=True)
        y += ROW
    return svg


# The ratio-against-speed figure. A point measured on this project's
# machine is filled; one taken from the Large Text Compression Benchmark,
# timed on its own machines, is hollow.

LOG_LOW, LOG_HIGH = 0, 6  # 1 ns to 1 ms per byte


@dataclass(frozen=True)
class Point:
    label: str
    megabytes: float
    ns_per_byte: float
    accent: bool
    hollow: bool


def place_labels(points, to_xy, box_of_point, area):
    """Puts each label beside its point, inside AREA, where it overlaps no
    point and no label placed before it: right, left, above, below, then
    the four corners, in that order of preference. Labels are placed in the
    order of POINTS."""
    taken = [box_of_point(*to_xy(p)) for p in points]
    placed = []
    for p in points:
        x, y = to_xy(p)
        w = text_width(p.label, 12)
        candidates = [
            (x + 9, y + 4, "start", (x + 8, y - 8, x + 10 + w, y + 6)),
            (x - 9, y + 4, "end", (x - 10 - w, y - 8, x - 8, y + 6)),
            (x, y - 10, "middle", (x - w / 2, y - 22, x + w / 2, y - 8)),
            (x, y + 19, "middle", (x - w / 2, y + 7, x + w / 2, y + 21)),
            (x + 8, y - 10, "start", (x + 7, y - 22, x + 9 + w, y - 8)),
            (x + 8, y + 19, "start", (x + 7, y + 7, x + 9 + w, y + 21)),
            (x - 8, y - 10, "end", (x - 9 - w, y - 22, x - 7, y - 8)),
            (x - 8, y + 19, "end", (x - 9 - w, y + 7, x - 7, y + 21)),
        ]
        chosen = candidates[0]
        for candidate in candidates:
            box = candidate[3]
            if inside(box, area) and not any(overlaps(box, other) for other in taken):
                chosen = candidate
                break
        taken.append(chosen[3])
        placed.append((p, chosen[0], chosen[1], chosen[2]))
    return placed


def overlaps(a, b):
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


def inside(a, b):
    return b[0] <= a[0] and a[2] <= b[2] and b[1] <= a[1] and a[3] <= b[3]


def scatter_panel(svg, top, height, points, title, y_low, y_high):
    t = svg.theme
    x0 = PAD + 44
    x1 = WIDTH - PAD - 8
    bottom = top + height
    xscale = (x1 - x0) / (LOG_HIGH - LOG_LOW)
    yscale = height / (y_high - y_low)

    def to_xy(p):
        return (x0 + (math.log10(p.ns_per_byte) - LOG_LOW) * xscale,
                bottom - (p.megabytes - y_low) * yscale)

    svg.text(x0, top - 12, title, t.primary, size=13, weight=600)
    for exponent in range(LOG_LOW, LOG_HIGH + 1):
        x = x0 + exponent * xscale
        svg.line(x, top, x, bottom, t.grid)
        svg.text(x, bottom + 18, f"{10 ** exponent:,}", t.muted, size=12, anchor="middle",
                 numeric=True)
    megabytes = y_low
    while megabytes <= y_high:
        y = bottom - (megabytes - y_low) * yscale
        svg.line(x0, y, x1, y, t.grid)
        svg.text(x0 - 8, y + 4, f"{megabytes}", t.muted, size=12, anchor="end", numeric=True)
        megabytes += 5
    svg.line(x0, bottom, x1, bottom, t.axis)

    # Grey first, so that cpress is drawn over anything it touches.
    ordered = sorted(points, key=lambda p: p.accent)
    for p in ordered:
        x, y = to_xy(p)
        svg.dot(x, y, t.accent if p.accent else t.other, hollow=p.hollow)
    # cpress's labels first, so that they take the nearest free places.
    placed = place_labels(ordered[::-1], to_xy,
                          lambda x, y: (x - DOT - 2, y - DOT - 2, x + DOT + 2, y + DOT + 2),
                          (x0 + 2, top, x1, bottom))
    for p, x, y, anchor in placed:
        svg.text(x, y, p.label, t.primary if p.accent else t.secondary, size=12, anchor=anchor,
                 weight=600 if p.accent else 400)


def superseded(row, rows):
    """Whether another of cpress's codecs is smaller than ROW and faster both
    ways: such a codec is history, and is left out of the figure."""
    def beats(other):
        return (other is not row and other["cpress"] == "yes"
                and int(other["bytes"]) < int(row["bytes"])
                and float(other["compress_ns_per_byte"]) < float(row["compress_ns_per_byte"])
                and float(other["decompress_ns_per_byte"]) < float(row["decompress_ns_per_byte"]))
    return row["cpress"] == "yes" and any(beats(other) for other in rows)


def ratio_speed(theme):
    rows = table("enwik8")
    rows = [r for r in rows if not superseded(r, rows)]
    compress = [Point(r["label"], int(r["bytes"]) / 1e6, float(r["compress_ns_per_byte"]),
                      r["cpress"] == "yes", r["source"] != "here") for r in rows]
    decompress = [Point(r["label"], int(r["bytes"]) / 1e6, float(r["decompress_ns_per_byte"]),
                        r["cpress"] == "yes", r["source"] != "here") for r in rows]
    largest = max(p.megabytes for p in compress)
    smallest = min(p.megabytes for p in compress)
    y_low = 5 * math.floor(smallest / 5)
    y_high = 5 * math.ceil(largest / 5)

    title = "Where cpress sits: size against speed on enwik8"
    subtitle = "Megabytes against nanoseconds per byte, on a log scale: lower is smaller, left is faster."
    panel = 250
    first_top = PLOT_TOP + 22
    second_top = first_top + panel + 66
    height = second_top + panel + 62
    svg = Svg(height, theme, title,
              "Compressed size of enwik8 in megabytes against nanoseconds per byte, "
              "compressing and decompressing, log scale: " +
              "; ".join(f"{p.label} {p.megabytes:.1f} MB, {p.ns_per_byte:g} and {q.ns_per_byte:g} ns/byte"
                        for p, q in zip(compress, decompress)) + ".")
    svg.heading(title, subtitle)
    scatter_panel(svg, first_top, panel, compress, "Compressing", y_low, y_high)
    scatter_panel(svg, second_top, panel, decompress, "Decompressing", y_low, y_high)
    svg.text(PAD + 44, height - 18, "Filled: measured on this machine, each alone on one core. "
             "Hollow: the benchmark's own, timed on its machines.", theme.muted, size=12)
    return svg


# The diagrams. These explain a mechanism rather than plot a measurement:
# the part being explained is in the accent colour, what surrounds it in
# grey. A number in a diagram is an example, and its subtitle says so,
# unless the number is a measurement the write-up quotes.


def pipeline(theme):
    t = theme
    title = "One bit, from what came before to the archive"
    subtitle = "Every bit goes round this loop, in the compressor and the decompressor alike."
    models = [("The last byte", "0.62"), ("The last 2 to 7 bytes", "0.97"),
              ("The word so far", "0.93"), ("The word before", "0.88"),
              ("The longest repeat", "0.99")]
    svg = Svg(404, t, title,
              "What came before feeds five models, each of which guesses the probability that "
              "the next bit is a 1. A mixer weighs the five guesses into one, two adjusting "
              "maps correct it, and the coder pays for the bit that came. Then every part "
              "learns from that bit.")
    svg.heading(title, subtitle)

    middle = 205
    svg.box(24, 150, 136, 110, t.panel, t.axis)
    svg.text(92, 176, "What came before", t.secondary, size=12, anchor="middle")
    svg.text(92, 210, "…compressio", t.primary, size=14, anchor="middle", mono=True)
    svg.text(92, 242, "and the next bit?", t.muted, size=12, anchor="middle")

    model_x, model_w, model_h = 196, 176, 34
    mixer_x, mixer_w = 404, 96
    for i, (name, guess) in enumerate(models):
        y = 100 + i * 44
        svg.arrow(160, middle, model_x - 2, y + model_h / 2, t.muted, width=1)
        svg.box(model_x, y, model_w, model_h, t.panel, t.axis)
        svg.text(model_x + 10, y + 22, name, t.primary, size=12)
        svg.text(model_x + model_w - 10, y + 22, guess, t.secondary, size=12, anchor="end",
                 numeric=True)
        svg.arrow(model_x + model_w, y + model_h / 2, mixer_x - 2, 165 + i * 20, t.muted,
                  width=1)

    svg.box(mixer_x, 150, mixer_w, 110, t.accent_soft, t.accent)
    svg.text(mixer_x + mixer_w / 2, 194, "Mixer", t.primary, size=14, anchor="middle",
             weight=600)
    svg.text(mixer_x + mixer_w / 2, 214, "weighs the", t.secondary, size=12, anchor="middle")
    svg.text(mixer_x + mixer_w / 2, 230, "five guesses", t.secondary, size=12, anchor="middle")

    apm_x, apm_w = 540, 84
    svg.arrow(mixer_x + mixer_w, middle, apm_x - 2, middle, t.secondary)
    svg.text((mixer_x + mixer_w + apm_x) / 2, middle - 8, "0.97", t.secondary, size=12,
             anchor="middle", numeric=True)
    svg.box(apm_x, 170, apm_w, 70, t.panel, t.axis)
    svg.text(apm_x + apm_w / 2, 201, "Adjust", t.primary, size=14, anchor="middle", weight=600)
    svg.text(apm_x + apm_w / 2, 221, "two APMs", t.secondary, size=12, anchor="middle")

    coder_x, coder_w = 664, 72
    svg.arrow(apm_x + apm_w, middle, coder_x - 2, middle, t.secondary)
    svg.text((apm_x + apm_w + coder_x) / 2, middle - 8, "0.98", t.secondary, size=12,
             anchor="middle", numeric=True)
    svg.box(coder_x, 150, coder_w, 110, t.panel, t.axis)
    svg.text(coder_x + coder_w / 2, 194, "Coder", t.primary, size=14, anchor="middle",
             weight=600)
    svg.text(coder_x + coder_w / 2, 214, "pays", t.secondary, size=12, anchor="middle")
    svg.text(coder_x + coder_w / 2, 230, "−log₂ p bits", t.secondary, size=12, anchor="middle")
    svg.arrow(720, 260, 720, 300, t.secondary)
    svg.text(736, 318, "archive", t.primary, size=12, anchor="end")

    # Learning: the bit that came goes back to every part that predicted it.
    feedback_y = 348
    svg.polyline([(684, 260), (684, feedback_y), (model_x + model_w / 2, feedback_y)], t.accent)
    svg.arrow(model_x + model_w / 2, feedback_y, model_x + model_w / 2, 312, t.accent)
    svg.arrow(mixer_x + mixer_w / 2, feedback_y, mixer_x + mixer_w / 2, 262, t.accent)
    svg.arrow(apm_x + apm_w / 2, feedback_y, apm_x + apm_w / 2, 242, t.accent)
    svg.text(model_x + model_w / 2 - 4, 376,
             "Then the bit that came is shown to every part, and each learns from it.",
             t.secondary, size=12)
    svg.text(24, 396, "The probabilities are an example: each says how likely the next bit is "
             "to be a 1.", t.muted, size=12)
    return svg


def coder(theme):
    t = theme
    steps = [(0.8, 1), (0.9, 1), (0.7, 0)]  # P(1) the model gave, and the bit that came
    title = "The coder: each bit narrows an interval"
    subtitle = "Three bits, as an example. A 1 takes the lower part, a 0 the upper."
    row = 56
    top = PLOT_TOP + 30
    x0, x1 = 300, WIDTH - PAD
    height = top + row * (len(steps) + 1) + 28
    svg = Svg(height, t, title,
              "The interval from 0 to 1 is split in proportion to the probability of a 1. "
              "Bits 1, 1 and 0 with probabilities 0.8, 0.9 and 0.3 leave an interval of width "
              "0.216, which takes 2.21 bits to name.")
    svg.heading(title, subtitle)

    def x(value):
        return x0 + value * (x1 - x0)

    svg.text(x0, top - 10, "0", t.muted, size=12, anchor="middle", numeric=True)
    svg.text(x1, top - 10, "1", t.muted, size=12, anchor="middle", numeric=True)
    low, high, total = 0.0, 1.0, 0.0
    for i, (p_one, bit) in enumerate(steps):
        y = top + i * row
        split = low + (high - low) * p_one
        p_bit = p_one if bit else 1 - p_one
        cost = -math.log2(p_bit)
        total += cost
        svg.line(x0, y + 11, x1, y + 11, t.grid, width=2)
        kept, other = ((low, split), (split, high)) if bit else ((split, high), (low, split))
        svg.box(x(kept[0]), y, x(kept[1]) - x(kept[0]), 22, t.accent_soft, t.accent, rx=3)
        svg.box(x(other[0]), y, x(other[1]) - x(other[0]), 22, t.panel, t.axis, rx=3)
        svg.text((x(low) + x(split)) / 2, y + 16, "1", t.primary, size=12, anchor="middle")
        svg.text((x(split) + x(high)) / 2, y + 16, "0", t.secondary, size=12, anchor="middle")
        svg.text(PAD, y + 9, f"Bit {i + 1}: the model says P(1) = {p_one:g}", t.primary, size=12)
        svg.text(PAD, y + 27, f"it is a {bit}, which costs {cost:.2f} bits", t.secondary,
                 size=12)
        low, high = kept

    y = top + len(steps) * row
    svg.line(x0, y + 11, x1, y + 11, t.grid, width=2)
    svg.box(x(low), y, x(high) - x(low), 22, t.accent, t.accent, rx=3)
    width = high - low
    svg.text(PAD, y + 9, f"What is left: {width:.3f} of the start", t.primary, size=12,
             weight=600)
    svg.text(PAD, y + 27, f"−log₂ {width:.3f} = {total:.2f} bits, the sum of the costs",
             t.secondary, size=12)
    svg.text(PAD, height - 14, "The archive is the shortest binary fraction inside what is left.",
             t.muted, size=12)
    return svg


def contexts(theme):
    t = theme
    title = "Contexts: what came before, at three lengths"
    subtitle = "Guessing the byte after “of data compressio”. A longer context says more, once seen."
    text = "of data compressio"
    rows = [(1, "The last byte, o: followed by almost any letter", "?", False),
            (3, "The last 3 bytes, sio: nearly always followed by n", "n", False),
            (6, "The last 6 bytes, ressio: followed by n every time", "n", True)]
    cell = 12
    start = 100
    svg = Svg(270, t, title,
              "Three contexts of the text 'of data compressio': the last byte, o, is followed "
              "by almost any letter; the last three, sio, nearly always by n; the last six, "
              "ressio, by n every time they were seen.")
    svg.heading(title, subtitle)

    def centre(i):
        return start + i * cell + cell / 2

    text_y = 116
    unknown = len(text)
    svg.box(start + unknown * cell - 1, text_y - 19, cell + 2, 26, t.accent_soft, t.accent, rx=4)
    for i, ch in enumerate(text + "?"):
        svg.text(centre(i), text_y, ch, t.primary, size=18, anchor="middle", mono=True)

    for k, (order, description, guess, confident) in enumerate(rows):
        y = 152 + k * 34
        left = start + (len(text) - order) * cell + 1
        right = start + len(text) * cell - 1
        svg.polyline([(left, y - 8), (left, y), (right, y), (right, y - 8)],
                     t.accent if confident else t.axis)
        svg.text(centre(unknown), y + 5, guess, t.accent if confident else t.secondary,
                 size=16, anchor="middle", weight=600 if confident else 400, mono=True)
        svg.text(centre(unknown) + 22, y + 4, description,
                 t.primary if confident else t.secondary, size=12)
    svg.text(PAD, 252, "A context table remembers, for each context it has met, what followed "
             "it. The descriptions are an example.", t.muted, size=12)
    return svg


def stretch(theme):
    t = theme
    title = "Stretch: from probability to evidence"
    subtitle = "stretch(p) = ln(p / (1 − p)). Evidence from separate models can be added; probabilities cannot."
    x0, x1 = PAD + 44, WIDTH - PAD - 8
    top, bottom = PLOT_TOP + 14, PLOT_TOP + 254
    low, high = -8, 8
    svg = Svg(bottom + 58, t, title,
              "The stretch of a probability against the probability: 0.5 stretches to 0, 0.9 "
              "to 2.2, 0.999 to 6.9, and 0.1 to -2.2. Near 0 and 1 a small step in probability "
              "is a large step in evidence.")
    svg.heading(title, subtitle)

    def to_xy(p, e):
        return x0 + p * (x1 - x0), bottom - (e - low) * (bottom - top) / (high - low)

    for tick in range(low, high + 1, 4):
        _, y = to_xy(0, tick)
        svg.line(x0, y, x1, y, t.axis if tick == 0 else t.grid)
        svg.text(x0 - 8, y + 4, f"{tick:+d}".replace("-", "−") if tick else "0", t.muted, size=12,
                 anchor="end", numeric=True)
    for tick in (0, 0.25, 0.5, 0.75, 1):
        x, _ = to_xy(tick, low)
        svg.line(x, top, x, bottom, t.grid)
        svg.text(x, bottom + 18, f"{tick:g}", t.muted, size=12, anchor="middle", numeric=True)
    svg.text(x0, bottom + 38, "probability that the bit is a 1", t.muted, size=12)
    svg.text(x0 + 8, top + 14, "evidence", t.muted, size=12)

    curve = []
    for step in range(-80, 81):
        evidence = step / 10
        curve.append(to_xy(1 / (1 + math.exp(-evidence)), evidence))
    svg.polyline(curve, t.accent, width=2)
    # Each label sits on the side of its point away from the curve.
    marks = [(0.1, "0.1 → −2.2", "start", 12, 4), (0.5, "0.5 → 0", "start", 10, 20),
             (0.9, "0.9 → 2.2", "end", -10, -10), (0.999, "0.999 → 6.9", "end", -12, 4)]
    for p, label, anchor, dx, dy in marks:
        x, y = to_xy(p, math.log(p / (1 - p)))
        svg.dot(x, y, t.accent)
        svg.text(x + dx, y + dy, label, t.primary, size=12, anchor=anchor, numeric=True)
    return svg


def match(theme):
    t = theme
    title = "The match model: the same bytes, earlier in the file"
    subtitle = "Where the last bytes occurred before, the byte that followed them then is the guess now."
    earlier = "is a lossless data compression algorithm"
    now = "uses lossless data compression ?"
    first, last = 5, 30  # the matching bytes, the same columns in both lines
    cell = 10
    start = (WIDTH - len(earlier) * cell) / 2
    svg = Svg(300, t, title,
              "Earlier in the file: 'is a lossless data compression algorithm'. Now: 'uses "
              "lossless data compression' and the next byte unknown. The last 26 bytes match, "
              "so the match model guesses the byte that followed them before, a.")
    svg.heading(title, subtitle)

    def centre(i):
        return start + i * cell + cell / 2

    following = last + 1
    for line, y, label in ((earlier, 132, "Earlier in the file"), (now, 214, "Now")):
        svg.text(PAD, y - 26, label, t.muted, size=12)
        svg.box(start + first * cell, y - 18, (last - first + 1) * cell, 25, t.accent_soft,
                t.accent_soft, rx=3)
        svg.box(start + following * cell, y - 18, cell, 25, t.panel, t.accent, rx=3)
        for i, ch in enumerate(line):
            svg.text(centre(i), y, ch, t.primary, size=16, anchor="middle", mono=True)
    svg.arrow(centre(following), 140, centre(following), 194, t.accent)
    svg.text(centre(following) + 12, 172, "guess: a", t.accent, size=12, weight=600)
    svg.text(PAD, 262, f"The last {last - first + 1} bytes match. The longer a match has run, "
             "the more the mixer believes it.", t.secondary, size=12)
    svg.text(PAD, 282, "The two lines can be millions of bytes apart.", t.muted, size=12)
    return svg


def prefetch(theme):
    t = theme
    title = "Waiting for memory, and asking early"
    subtitle = "Not to scale. Coding a byte needs table entries that are mostly in main memory."
    wait, work = 60, 110
    x0 = 200
    svg = Svg(312, t, title,
              "Fetching when needed, each byte waits for memory before it is coded. Fetching a "
              "byte early, the entries for the next byte are requested while this one is coded, "
              "so the waits overlap the work and the same bytes finish sooner.")
    svg.heading(title, subtitle)

    def block(x, y, w, label, busy):
        svg.box(x + 1, y, w - 2, 30, t.accent_soft if busy else t.panel,
                t.accent if busy else t.axis, rx=4)
        svg.text(x + w / 2, y + 20, label, t.primary if busy else t.muted, size=12,
                 anchor="middle")

    y = 104
    svg.text(PAD, y + 20, "Fetching when needed", t.primary, size=12)
    x = x0
    for byte in (1, 2, 3):
        block(x, y, wait, "wait", False)
        block(x + wait, y, work, f"code byte {byte}", True)
        x += wait + work
    slow_end = x

    y = 180
    svg.text(PAD, y + 20, "Fetching a byte early", t.primary, size=12)
    block(x0, y, wait, "wait", False)
    x = x0 + wait
    for byte in (1, 2, 3):
        block(x, y, work, f"code byte {byte}", True)
        svg.box(x + 6, y + 36, work - 12, 8, t.panel, t.axis, rx=2)
        x += work
    svg.text(x0 + wait + 6, y + 60, "and meanwhile, fetch for the next byte", t.muted, size=12)
    svg.arrow(x + 4, y + 15, slow_end, y + 15, t.secondary, width=1)
    svg.text((x + slow_end) / 2, y - 4, "time saved", t.secondary, size=12, anchor="middle")
    svg.arrow(x0, 272, WIDTH - PAD, 272, t.axis, width=1)
    svg.text(x0, 292, "time", t.muted, size=12)
    return svg


def order_place(theme):
    t = theme
    title = "Naming an order by its place"
    subtitle = "Live orders in one stock, most recent first, as an example. A later message names one."
    orders = [("7,385,104,951", "buy", "100", "182.31"), ("7,385,104,630", "sell", "200", "182.36"),
              ("7,385,104,412", "buy", "50", "182.30"), ("7,385,104,226", "sell", "100", "182.37"),
              ("7,385,103,987", "buy", "300", "182.29"), ("7,385,103,515", "sell", "100", "182.40")]
    chosen = 3
    svg = Svg(350, t, title,
              "A table of six live orders in one stock. A delete message names an order by "
              "its 64-bit reference, about 23.6 bits a delete when coded as a column; cpress "
              "names it by its place in the list, about 4.5 bits on average, and then knows "
              "its side, shares and price.")
    svg.heading(title, subtitle)

    columns = [(44, "Place", "middle"), (76, "Reference", "start"), (206, "Side", "start"),
               (316, "Shares", "end"), (404, "Price", "end")]
    for x, name, anchor in columns:
        svg.text(x, 108, name, t.muted, size=12, anchor=anchor)
    row_h = 28
    for i, (reference, side, shares, price) in enumerate(orders):
        y = 120 + i * row_h
        if i == chosen:
            svg.box(PAD, y, 404 - PAD + 12, row_h - 2, t.accent_soft, t.accent, rx=4)
        colour = t.primary if i == chosen else t.secondary
        for (x, _, anchor), value in zip(columns, (str(i + 1), reference, side, shares, price)):
            svg.text(x, y + 18, value, colour, size=13, anchor=anchor, numeric=True)

    box_x, box_w = 470, WIDTH - PAD - 470
    svg.box(box_x, 112, box_w, 72, t.panel, t.axis)
    svg.text(box_x + 12, 132, "A delete, as sent", t.muted, size=12)
    svg.text(box_x + 12, 155, "delete 7,385,104,226", t.secondary, size=13, mono=True)
    svg.text(box_x + 12, 174, "as a column: 23.6 bits a delete", t.muted, size=12)
    svg.box(box_x, 204, box_w, 72, t.accent_soft, t.accent)
    svg.text(box_x + 12, 224, "The same delete, as coded", t.secondary, size=12)
    svg.text(box_x + 12, 247, "delete place 4", t.primary, size=13, mono=True, weight=600)
    svg.text(box_x + 12, 266, "4.5 bits on average", t.secondary, size=12)
    chosen_y = 120 + chosen * row_h + row_h / 2 - 1
    svg.arrow(box_x, 240, 404 + 16, chosen_y, t.accent)
    svg.text(PAD, 322, "Once the order is found, its side, shares and price are known: a "
             "delete has nothing else to say.", t.secondary, size=12)
    return svg


FIGURES = {
    "pipeline": pipeline,
    "coder": coder,
    "contexts": contexts,
    "stretch": stretch,
    "match": match,
    "prefetch": prefetch,
    "order-place": order_place,
    "understanding": understanding,
    "ratio-speed": ratio_speed,
    "stages": stages,
    "memory": memory,
    "itch-parts": itch_parts,
    "itch-bytes": itch_bytes,
}


def main():
    for name, draw in FIGURES.items():
        for theme in (LIGHT, DARK):
            path = HERE / f"{name}-{theme.name}.svg"
            path.write_text(draw(theme).render(), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
