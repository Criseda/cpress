#!/usr/bin/env python3
"""Draws the figures of docs/write-up.md and the README.

Each figure is read from a table beside this script and written twice, as
NAME-light.svg and NAME-dark.svg, for GitHub's two themes. Standard library
only. The same tables give the same bytes: nothing here depends on the
time, the machine or the order of a dictionary.

    python3 docs/figures/draw.py

Every figure has one point: cpress against the rest, or one design against
the last. So every figure is drawn the same way: cpress, or the design, in
one accent colour, everything else in grey, and a number not measured on
this project's machine as a hollow mark.
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


# The colours of the dataviz reference palette: series slot 1 and the muted
# grey, each validated for colour-blind separation and contrast on its
# surface.
LIGHT = Theme("light", "#fcfcfb", "#0b0b0b", "#52514e", "#898781", "#e1e0d9", "#c3c2b7",
              "#2a78d6", "#898781")
DARK = Theme("dark", "#1a1a19", "#f0efec", "#c3c2b7", "#898781", "#2c2c2a", "#383835",
             "#3987e5", "#898781")

WIDTH = 760
PAD = 24
TITLE_Y = 34
SUBTITLE_Y = 56
PLOT_TOP = 84
FONT = "system-ui, -apple-system, 'Segoe UI', Roboto, sans-serif"

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

    def text(self, x, y, content, colour, size=13, anchor="start", weight=400, numeric=False):
        attributes = f' text-anchor="{anchor}"' if anchor != "start" else ""
        if weight != 400:
            attributes += f' font-weight="{weight}"'
        if numeric:
            attributes += ' class="num"'
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
    point and no label placed before it: right, left, above or below, in that
    order of preference."""
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
    placed = place_labels(ordered, to_xy,
                          lambda x, y: (x - DOT - 2, y - DOT - 2, x + DOT + 2, y + DOT + 2),
                          (x0 + 2, top, x1, bottom))
    for p, x, y, anchor in placed:
        svg.text(x, y, p.label, t.primary if p.accent else t.secondary, size=12, anchor=anchor,
                 weight=600 if p.accent else 400)


def ratio_speed(theme):
    rows = table("enwik8")
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
    svg.text(PAD + 44, height - 18, "Filled: measured on one machine in one sitting. Hollow: "
             "the benchmark's own, timed on its machines.", theme.muted, size=12)
    return svg


FIGURES = {
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
