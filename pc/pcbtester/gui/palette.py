"""Plot color system.

Categorical hues (dark-surface steps) validated with the dataviz palette
validator against SURFACE: lightness band, chroma floor and contrast all PASS;
adjacent-pair CVD separation sits in the 8-12 floor band, which is legal only
with secondary encoding — the plots therefore always ship a legend and
per-channel visibility checkboxes (identity is never color-alone).

Assignment is FIXED: LVLP CH1-8 own slots 0-7 permanently; the HP/HV plot
assigns its three series slots 0-2 within its own axes. Colors follow the
channel, never its position in the visible set.
"""

CATEGORICAL = [
    "#3987e5",  # slot 0 blue
    "#199e70",  # slot 1 aqua
    "#c98500",  # slot 2 yellow
    "#008300",  # slot 3 green
    "#9085e9",  # slot 4 violet
    "#e66767",  # slot 5 red
    "#d55181",  # slot 6 magenta
    "#d95926",  # slot 7 orange
]

SURFACE = "#1a1a19"
TEXT_PRIMARY = "#ffffff"
TEXT_SECONDARY = "#c3c2b7"
GRID_ALPHA = 0.25


def channel_color(ch: int) -> str:
    """Fixed color for LVLP channel 1-8."""
    return CATEGORICAL[(ch - 1) % len(CATEGORICAL)]
