"""Visual identity for the bench charts.

Single source of truth for palette, fonts, and layout primitives.
render_charts.py and any future chart code calls apply_style() before
rendering.

Voice: technical, anti-hype, audit-ready. Slate gray for the antagonist
(rolling-summary), deep teal for the proposition (replayable). Fonts
bundled in _chart_assets/ so charts render identically on every reader's
machine.
"""
from __future__ import annotations

from pathlib import Path

import matplotlib
import matplotlib.font_manager as fm
import matplotlib.pyplot as plt

# ---- palette ----------------------------------------------------------

# Antagonist — muted, neutral, recedes. Rolling-summary lives here.
COLOR_ROLLING = "#5b6770"
# Proposition — accent. Replayable / DPM lives here. Teal reads as
# verified/audit/certificate without mimicking any single big-tech
# brand color.
COLOR_REPLAYABLE = "#1f8a8a"
# Surfaces.
COLOR_BG = "#fafafa"
COLOR_GRID = "#dcdcdc"
COLOR_TITLE = "#1a1a1a"
COLOR_AXIS = "#3a3a3a"
COLOR_MUTED = "#6a6a6a"


# ---- typography -------------------------------------------------------

_ASSETS = Path(__file__).resolve().parent / "_chart_assets"

# ProductDesign for the title (display weight). VeraMono for everything
# else — labels, numbers, source line, wordmark. Mono on data labels
# makes numerics align cleanly across grouped bars.
FONT_TITLE_PATH = _ASSETS / "ProductDesign.ttf"
FONT_BODY_PATH = _ASSETS / "VeraMono.ttf"
FONT_BODY_BOLD_PATH = _ASSETS / "VeraMoBd.ttf"


def _register_fonts() -> tuple[str, str, str]:
    """Register bundled TTFs with matplotlib's font manager and return
    the family names. Idempotent: re-registering is a no-op."""
    title_name = "FallbackSans"
    body_name = "FallbackMono"
    body_bold_name = "FallbackMono"
    for path in (FONT_TITLE_PATH, FONT_BODY_PATH, FONT_BODY_BOLD_PATH):
        if path.exists():
            fm.fontManager.addfont(str(path))
    if FONT_TITLE_PATH.exists():
        title_name = fm.FontProperties(fname=str(FONT_TITLE_PATH)).get_name()
    if FONT_BODY_PATH.exists():
        body_name = fm.FontProperties(fname=str(FONT_BODY_PATH)).get_name()
    if FONT_BODY_BOLD_PATH.exists():
        body_bold_name = fm.FontProperties(
            fname=str(FONT_BODY_BOLD_PATH)).get_name()
    return title_name, body_name, body_bold_name


def apply_style() -> dict:
    """Configure matplotlib rcParams and return a small dict of font
    family names callers can apply locally where needed."""
    matplotlib.use("Agg")
    title_family, body_family, body_bold_family = _register_fonts()

    plt.rcParams.update({
        "figure.facecolor": COLOR_BG,
        "axes.facecolor": COLOR_BG,
        "savefig.facecolor": COLOR_BG,
        "axes.edgecolor": COLOR_AXIS,
        "axes.labelcolor": COLOR_AXIS,
        "xtick.color": COLOR_AXIS,
        "ytick.color": COLOR_AXIS,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "axes.grid": True,
        "grid.color": COLOR_GRID,
        "grid.linestyle": ":",
        "grid.linewidth": 0.7,
        "axes.axisbelow": True,
        "font.family": body_family,
        "font.size": 10,
    })
    return {
        "title": title_family,
        "body": body_family,
        "body_bold": body_bold_family,
    }


# ---- layout helpers ---------------------------------------------------

def styled_title(ax, title: str, subtitle: str | None,
                 fonts: dict) -> None:
    """Left-aligned title with a teal accent bar prefix. Subtitle in
    muted gray below it."""
    ax.set_title("")  # clear any default
    fig = ax.figure
    bbox = ax.get_position()
    # Accent bar — narrow vertical rectangle on the left edge of the
    # plot area, in figure coordinates.
    bar_x = bbox.x0
    bar_y_top = bbox.y1 + 0.06
    bar_y_bot = bbox.y1 + 0.012
    fig.add_artist(plt.Line2D(
        [bar_x, bar_x], [bar_y_bot, bar_y_top],
        color=COLOR_REPLAYABLE, linewidth=3, solid_capstyle="butt",
        transform=fig.transFigure, clip_on=False))
    # Title text just to the right of the bar.
    fig.text(bar_x + 0.012, bar_y_top, title,
              fontfamily=fonts["title"], fontsize=14,
              color=COLOR_TITLE, va="top", ha="left")
    if subtitle:
        fig.text(bar_x + 0.012, bar_y_top - 0.038, subtitle,
                  fontfamily=fonts["body"], fontsize=9,
                  color=COLOR_MUTED, va="top", ha="left")


def styled_footer(fig, source: str, fonts: dict) -> None:
    """Source line bottom-left, wordmark bottom-right."""
    fig.text(0.012, 0.012, f"source · {source}",
              fontfamily=fonts["body"], fontsize=7.5,
              color=COLOR_MUTED, va="bottom", ha="left")
    fig.text(0.988, 0.012,
              "replayable agent memory · litert-dpm",
              fontfamily=fonts["body"], fontsize=7.5,
              color=COLOR_MUTED, va="bottom", ha="right")
