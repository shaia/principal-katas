"""Generate every figure and animation used by ../your-profiler-said-dispatch.md.

    py -3 make_figures.py

All numbers live in MEASUREMENTS below and are copied verbatim from the run that
cpp/solution.md quotes, so the post, the solution and the figures cannot drift
apart. Re-measure once, change one dict, and all three follow.

That run: clang 21.1.7 targeting x86_64-pc-windows-msvc, Release, on an Intel
i9-13980HX, thread pinned to CPU 0, second invocation of the binary. Reproduce
with `cd cpp/bin && ./solution.exe`.

Figures use an explicit light background rather than a transparent one: a
transparent PNG with dark text is invisible in GitHub's dark theme, which is the
most common way a committed chart silently fails.
"""

from __future__ import annotations

import pathlib

import matplotlib

matplotlib.use("Agg")

import matplotlib.animation as animation
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import FancyArrowPatch, Rectangle

HERE = pathlib.Path(__file__).parent

# --------------------------------------------------------------------------
# The measurements. Everything below reads from here and nowhere else.
# --------------------------------------------------------------------------

MEASUREMENTS = {
    # The rate the brief names, as a per-packet budget on one core.
    "budget_ns": 333.3,
    # Phase 0.
    "loop_floor_ns": 1.76,
    # Phase 2 — uniform traffic, sweeping the handler count.
    "sweep": {
        "handlers": [10, 25, 50, 100],
        "scan_ns": [36.6, 71.2, 119.2, 142.8],
        "table_ns": [36.2, 60.8, 71.9, 67.8],
        "probes": [5.7, 13.9, 27.2, 52.3],
    },
    # Phase 3 — 100 handlers, four traffic distributions.
    # (scan_unordered, scan_ordered, scan_mtf, table_compact, probes_unordered, probes_ordered)
    "dist": {
        "uniform": (138.5, 137.8, 199.2, 68.5, 52.3, 51.0),
        "zipf": (147.0, 82.5, 126.3, 62.1, 67.6, 18.7),
        "95/5": (128.2, 41.4, 48.4, 35.9, 88.2, 5.7),
        "worst": (135.1, 30.9, 30.9, 27.6, 100.0, 3.0),
    },
    # Phase 4 — selection held constant. (hot, mixed, mixed_tiny, icache_delta)
    "mech": {
        "direct-call": (26.26, 26.36, 0.91, 0.10),
        "virtual": (27.57, 61.26, 11.15, 33.69),
        "erasure": (27.38, 60.41, 9.48, 33.03),
        "variant": (27.72, 64.54, 11.89, 36.82),
    },
    "mech_floor_ns": 0.91,  # direct-call mixed-tiny: the cost of no dispatch at all
    # Phase 5 — .text bytes.
    "size": {
        "empty": 3478,
        "none": 183574,
        "virtual": 184278,
        "erasure": 170534,
        "variant": 180198,
    },
    "handler_set_bytes": 180096,
    "l1i_bytes": 32 * 1024,
    # Phase 6.
    "boundary": {"internal": 7.1, "per_packet": 10.1, "batched": 10.2},
}

# --------------------------------------------------------------------------
# House style
# --------------------------------------------------------------------------

BG = "#ffffff"
INK = "#1c1c1c"
MUTED = "#6b6b6b"
GRID = "#e2e2e2"

SCAN = "#c1443c"      # the problem
SORT = "#d99000"      # the one-line fix
TABLE = "#2c6e8f"     # the rewrite
CONTROL = "#8a8a8a"   # controls and floors
ACCENT = "#4a7c59"

plt.rcParams.update(
    {
        "figure.facecolor": BG,
        "axes.facecolor": BG,
        "savefig.facecolor": BG,
        "text.color": INK,
        "axes.labelcolor": INK,
        "axes.edgecolor": MUTED,
        "xtick.color": INK,
        "ytick.color": INK,
        "font.size": 10,
        "axes.titlesize": 12,
        "axes.titleweight": "bold",
        "axes.spines.top": False,
        "axes.spines.right": False,
        "grid.color": GRID,
        "figure.dpi": 200,
    }
)


def save(fig, name: str) -> None:
    path = HERE / name
    fig.savefig(path, bbox_inches="tight", facecolor=BG)
    plt.close(fig)
    print(f"  {name:38} {path.stat().st_size / 1024:8.1f} KiB")


# --------------------------------------------------------------------------
# fig1 — the scan is linear in probes; the table is not flat either
# --------------------------------------------------------------------------


def fig1_scan_vs_table() -> None:
    s = MEASUREMENTS["sweep"]
    x = s["handlers"]

    fig, ax = plt.subplots(figsize=(7.2, 4.0))
    ax.plot(x, s["scan_ns"], "o-", color=SCAN, lw=2.2, ms=7, label="linear scan (the brief's loop)")
    ax.plot(x, s["table_ns"], "o-", color=TABLE, lw=2.2, ms=7, label="key-indexed table")

    ax.set_xlabel("handlers registered")
    ax.set_ylabel("ns per packet")
    ax.set_xticks(x)
    ax.set_xlim(1, 112)   # room for the leftmost probe label, which otherwise clips
    ax.set_ylim(0, 172)
    ax.grid(axis="y", lw=0.8)
    ax.set_axisbelow(True)

    # Probes, as annotations rather than a second axis: a twin axis here invites
    # the reader to compare two different units by eye, which is exactly the
    # mistake the chart exists to prevent.
    for xi, yi, p in zip(x, s["scan_ns"], s["probes"]):
        ax.annotate(
            f"{p:.1f} probes",
            (xi, yi),
            textcoords="offset points",
            xytext=(0, 11),
            ha="center",
            fontsize=8.5,
            color=SCAN,
        )

    ax.annotate(
        "the table's cost is not flat either:\nO(1) selection, but 100 handler\nbodies no longer fit in L1i",
        xy=(100, 67.8),
        xytext=(58, 20),
        fontsize=8.5,
        color=MUTED,
        arrowprops=dict(arrowstyle="->", color=MUTED, lw=1),
    )

    ax.legend(frameon=False, loc="upper left")
    ax.set_title("Cost is linear in probes, not in the dispatch mechanism", loc="left")
    fig.text(
        0.005,
        -0.04,
        "uniform traffic, 100 generated handlers of 1800 B each.  "
        f"budget = {MEASUREMENTS['budget_ns']:.0f} ns/packet at 3 M packets/s.",
        fontsize=8,
        color=MUTED,
    )
    save(fig, "fig1-scan-vs-table.png")


# --------------------------------------------------------------------------
# fig2 — the headline: who captures the available win
# --------------------------------------------------------------------------


def fig2_where_the_win_comes_from() -> None:
    d = MEASUREMENTS["dist"]
    order = ["uniform", "zipf", "95/5", "worst"]
    labels = {
        "uniform": "uniform\n(no skew to exploit)",
        "zipf": "zipf s=1.1\n(realistic)",
        "95/5": "95/5\n(realistic, skewed)",
        "worst": "worst case\n(hot handler last)",
    }

    fig, ax = plt.subplots(figsize=(7.8, 4.2))
    y = np.arange(len(order))[::-1]

    for i, key in enumerate(order):
        unord, ordered, _mtf, table, _pu, _po = d[key]
        available = unord - table
        by_sort = unord - ordered
        by_table = ordered - table
        yy = y[i]

        ax.barh(yy, by_sort, height=0.55, color=SORT, edgecolor=BG)
        ax.barh(yy, by_table, height=0.55, left=by_sort, color=TABLE, edgecolor=BG)

        if by_sort / available > 0.06:
            ax.text(
                by_sort / 2,
                yy,
                f"{100 * by_sort / available:.0f}%",
                ha="center",
                va="center",
                color="white",
                fontweight="bold",
                fontsize=10,
            )
        if by_table / available > 0.06:
            ax.text(
                by_sort + by_table / 2,
                yy,
                f"{100 * by_table / available:.0f}%",
                ha="center",
                va="center",
                color="white",
                fontweight="bold",
                fontsize=10,
            )
        ax.text(
            available + 2,
            yy,
            f"{available:.0f} ns available",
            va="center",
            fontsize=8.5,
            color=MUTED,
        )

    ax.set_yticks(y)
    ax.set_yticklabels([labels[k] for k in order], fontsize=9)
    ax.set_xlabel("ns per packet recovered, out of the total available")
    ax.set_xlim(0, 142)
    ax.grid(axis="x", lw=0.8)
    ax.set_axisbelow(True)

    handles = [
        Rectangle((0, 0), 1, 1, color=SORT),
        Rectangle((0, 0), 1, 1, color=TABLE),
    ]
    # Below the axis, not inside it: at 'worst' the bars run the full width and
    # an in-axes legend lands on top of the row it is explaining.
    ax.legend(
        handles,
        ["captured by sorting the list (one line)", "added by the table (the rewrite)"],
        frameon=False,
        loc="upper center",
        bbox_to_anchor=(0.5, -0.20),
        ncol=2,
        fontsize=9,
    )
    ax.set_title("The more skewed the traffic, the less the rewrite buys", loc="left")
    fig.text(
        0.005,
        -0.14,
        "100 handlers. 'sorting' is given the true frequencies of the very stream it is measured on — "
        "better than any real system could manage.",
        fontsize=8,
        color=MUTED,
    )
    save(fig, "fig2-where-the-win-comes-from.png")


# --------------------------------------------------------------------------
# fig3 — the mechanism, and the thing that costs more than it
# --------------------------------------------------------------------------


def fig3_mechanism() -> None:
    m = MEASUREMENTS["mech"]
    names = ["direct-call", "virtual", "erasure", "variant"]
    floor = MEASUREMENTS["mech_floor_ns"]

    fig, (axa, axb) = plt.subplots(1, 2, figsize=(10.2, 4.2), gridspec_kw={"width_ratios": [1.35, 1]})

    # Panel A — one resident handler vs 100 interleaved.
    x = np.arange(len(names))
    w = 0.36
    hot = [m[n][0] for n in names]
    mixed = [m[n][1] for n in names]

    axa.bar(x - w / 2, hot, w, color=CONTROL, label="hot: 1 handler, resident")
    axa.bar(x + w / 2, mixed, w, color=SCAN, label="mixed: 100 handlers, 176 KiB")

    for i, n in enumerate(names):
        delta = m[n][3]
        axa.annotate(
            f"+{delta:.1f}",
            xy=(x[i] + w / 2, mixed[i]),
            textcoords="offset points",
            xytext=(0, 4),
            ha="center",
            fontsize=9,
            fontweight="bold",
            color=SCAN if delta > 1 else CONTROL,
        )

    axa.set_xticks(x)
    axa.set_xticklabels(names, fontsize=9)
    axa.set_ylabel("ns per packet")
    axa.set_ylim(0, 92)
    axa.grid(axis="y", lw=0.8)
    axa.set_axisbelow(True)
    axa.legend(frameon=False, fontsize=9, loc="upper right")
    axa.set_title("The instruction cache, measured as a time delta", loc="left", fontsize=11)
    # The control note goes under the panel rather than inside it: every region
    # of the axes is occupied by a bar, a delta label or the legend, and an
    # arrow drawn across them was less legible than a caption.
    axa.text(
        0.0,
        -0.20,
        "direct-call is the control — it always reaches the same handler, so it has\n"
        "no i-cache cost to pay, and its +0.1 is what an honest zero looks like.",
        transform=axa.transAxes,
        fontsize=8,
        color=MUTED,
        va="top",
    )

    # Panel B — the mechanism alone.
    costs = [max(m[n][2] - floor, 0.0) for n in names]
    colors = [CONTROL, TABLE, ACCENT, SORT]
    axb.bar(names, costs, color=colors, width=0.6)
    for i, c in enumerate(costs):
        axb.text(i, c + 0.25, f"{c:.2f}", ha="center", fontsize=9, fontweight="bold")
    axb.set_ylabel("ns per packet")
    axb.set_ylim(0, 17)
    axb.grid(axis="y", lw=0.8)
    axb.set_axisbelow(True)
    axb.set_xticks(range(len(names)))
    axb.set_xticklabels(names, fontsize=9)
    axb.set_title("The dispatch mechanism alone", loc="left", fontsize=11)
    axb.text(
        0.0,
        -0.20,
        "tiny handlers, floor subtracted. 3 % of the 333 ns budget —\nand a third of what interleaving 100 handlers costs.",
        transform=axb.transAxes,
        fontsize=8,
        color=MUTED,
        va="top",
    )

    fig.suptitle(
        "What the four options compete over, and what costs three times more",
        x=0.005,
        ha="left",
        fontsize=12.5,
        fontweight="bold",
    )
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    save(fig, "fig3-mechanism.png")


# --------------------------------------------------------------------------
# fig4 — everything against the budget it has to fit inside
# --------------------------------------------------------------------------


def fig4_budget() -> None:
    budget = MEASUREMENTS["budget_ns"]
    d = MEASUREMENTS["dist"]

    rows = [
        ("scan, uniform traffic", d["uniform"][0], SCAN),
        ("scan, 95/5 traffic", d["95/5"][0], SCAN),
        ("scan sorted by frequency, 95/5", d["95/5"][1], SORT),
        ("table, 95/5 traffic", d["95/5"][3], TABLE),
    ]

    fig, ax = plt.subplots(figsize=(8.0, 3.4))
    y = np.arange(len(rows))[::-1]

    for i, (label, val, color) in enumerate(rows):
        ax.barh(y[i], budget, height=0.6, color="#f0f0f0", edgecolor=GRID)
        ax.barh(y[i], val, height=0.6, color=color)
        ax.text(
            val + 5,
            y[i],
            f"{val:.1f} ns   ({100 * val / budget:.0f}% of budget)",
            va="center",
            fontsize=9,
            color=INK,
        )
        _ = label

    ax.set_yticks(y)
    ax.set_yticklabels([r[0] for r in rows], fontsize=9)
    ax.set_xlim(0, budget * 1.02)
    # Room below the last bar for the budget label, which collides with the
    # title if placed at the top.
    ax.set_ylim(-1.0, len(rows) - 0.4)
    ax.set_xlabel("ns per packet")
    ax.axvline(budget, color=INK, lw=1.2, ls="--")
    # Below the bars, not above them: at the top it collides with the title.
    ax.annotate(
        f"{budget:.0f} ns — one core at 3 M packets/s",
        xy=(budget, -0.68),
        xytext=(-6, 0),
        textcoords="offset points",
        ha="right",
        va="center",
        fontsize=8.5,
        color=INK,
    )
    ax.grid(axis="x", lw=0.8)
    ax.set_axisbelow(True)
    ax.set_title("Everything, against the budget it has to fit inside", loc="left")
    save(fig, "fig4-budget.png")


# --------------------------------------------------------------------------
# anim1 — walking the list versus indexing the table
# --------------------------------------------------------------------------


def anim1_scan_vs_table() -> None:
    n_shown = 24          # handlers drawn; the real set is 100
    target = 17           # the one that matches
    hold = 8              # frames held on the final state

    fig, (axl, axr) = plt.subplots(1, 2, figsize=(7.6, 3.4))
    for ax in (axl, axr):
        ax.set_xlim(-0.5, n_shown + 0.5)
        ax.set_ylim(-1.6, 2.4)
        ax.axis("off")

    def draw_row(ax, y=0.0):
        boxes = []
        for i in range(n_shown):
            r = Rectangle((i, y), 0.82, 0.82, facecolor="#eaeaea", edgecolor="#cfcfcf", lw=0.6)
            ax.add_patch(r)
            boxes.append(r)
        return boxes

    lboxes = draw_row(axl)
    rboxes = draw_row(axr)
    for b in (lboxes[target], rboxes[target]):
        b.set_edgecolor(ACCENT)
        b.set_linewidth(1.6)

    axl.set_title("the brief's loop:  ask each in turn", fontsize=10, loc="left", color=SCAN)
    axr.set_title("the table:  one index", fontsize=10, loc="left", color=TABLE)

    ltxt = axl.text(0, 1.5, "", fontsize=10, color=INK)
    rtxt = axr.text(0, 1.5, "", fontsize=10, color=INK)
    lfoot = axl.text(0, -1.25, "", fontsize=9, color=MUTED)
    rfoot = axr.text(0, -1.25, "", fontsize=9, color=MUTED)

    arrow = FancyArrowPatch(
        (target + 0.4, 1.35), (target + 0.4, 0.92), arrowstyle="-|>", mutation_scale=13,
        color=TABLE, lw=2,
    )
    axr.add_patch(arrow)
    arrow.set_visible(False)

    frames = target + 1 + hold

    def update(f):
        step = min(f, target)
        for i, b in enumerate(lboxes):
            if i < step:
                b.set_facecolor("#f6d9d7")          # probed, did not match
            elif i == step:
                b.set_facecolor(SCAN if step < target else ACCENT)
            else:
                b.set_facecolor("#eaeaea")
        ltxt.set_text(f"matches() calls: {step + 1}")
        lfoot.set_text(
            "52.3 per packet at 100 handlers\n142.8 ns — 43 % of the budget"
            if f >= target
            else ""
        )

        if f >= 1:
            arrow.set_visible(True)
            rboxes[target].set_facecolor(ACCENT)
            rtxt.set_text("matches() calls: 0")
            rfoot.set_text(
                "1 load, 1 call\n67.8 ns — the same handlers, found by index" if f >= target else ""
            )
        return lboxes + rboxes + [ltxt, rtxt, lfoot, rfoot, arrow]

    anim = animation.FuncAnimation(fig, update, frames=frames, interval=110, blit=False)
    path = HERE / "anim1-scan-vs-table.gif"
    anim.save(path, writer=animation.PillowWriter(fps=9), dpi=100, savefig_kwargs={"facecolor": BG})
    plt.close(fig)
    print(f"  {'anim1-scan-vs-table.gif':38} {path.stat().st_size / 1024:8.1f} KiB")


# --------------------------------------------------------------------------
# anim2 — sorting the list by frequency, and what it does to probes
# --------------------------------------------------------------------------


def anim2_frequency_ordering() -> None:
    rng = np.random.default_rng(7)
    n = 40
    # Zipf-ish weights assigned to a SHUFFLED set of positions: registration
    # order uncorrelated with traffic, which is the realistic case and the one
    # the first version of the benchmark got wrong.
    weights = 1.0 / np.power(np.arange(1, n + 1), 1.1)
    weights /= weights.sum()
    perm = rng.permutation(n)
    start = np.empty(n)
    start[perm] = weights

    target_order = np.argsort(-start)
    steps = 26
    probes_from, probes_to = 88.2, 5.7

    fig, ax = plt.subplots(figsize=(7.6, 2.9))
    ax.set_xlim(-0.5, n + 0.5)
    ax.set_ylim(-1.9, 2.0)
    ax.axis("off")

    cmap = plt.get_cmap("YlOrRd")
    norm = plt.Normalize(0, start.max())

    boxes = []
    for i in range(n):
        r = Rectangle((i, 0), 0.86, 0.9, facecolor=cmap(norm(start[i])), edgecolor="#d5d5d5", lw=0.5)
        ax.add_patch(r)
        boxes.append(r)

    ax.text(0, 1.45, "handler list, shaded by share of traffic", fontsize=10, color=INK)
    counter = ax.text(0, -0.85, "", fontsize=11, color=INK, fontweight="bold")
    note = ax.text(0, -1.62, "", fontsize=8.5, color=MUTED)

    def update(f):
        t = min(f / (steps - 1), 1.0)
        # Interpolate each box toward its sorted position.
        for rank, idx in enumerate(target_order):
            x0 = idx
            x1 = rank
            boxes[idx].set_x(x0 + (x1 - x0) * t)
        probes = probes_from + (probes_to - probes_from) * t
        counter.set_text(f"probes per packet: {probes:5.1f}")
        if t >= 1.0:
            note.set_text(
                "one std::sort by observed frequency.  at 95/5 traffic this captures 94 % of\n"
                "everything the rewrite could have won — and the table adds the remaining 6 %."
            )
        else:
            note.set_text("")
        return boxes + [counter, note]

    anim = animation.FuncAnimation(
        fig, update, frames=steps + 10, interval=110, blit=False
    )
    path = HERE / "anim2-frequency-ordering.gif"
    anim.save(path, writer=animation.PillowWriter(fps=9), dpi=100, savefig_kwargs={"facecolor": BG})
    plt.close(fig)
    print(f"  {'anim2-frequency-ordering.gif':38} {path.stat().st_size / 1024:8.1f} KiB")


# --------------------------------------------------------------------------
# anim3 — why 100 handlers cost more than one, even with O(1) selection
# --------------------------------------------------------------------------


def anim3_icache() -> None:
    """Animates the experiment fig3 panel A measures, rather than a cache model.

    The first version drew a single row with a resident set and a refetch
    counter. It was legible but made the weaker point: what matters is not that
    a cache evicts things, it is that the SAME dispatch code costs 27 ns when
    one handler is hot and 61 ns when a hundred are interleaved. Showing both
    arms side by side is the measurement, not an illustration of it.
    """
    per_body = MEASUREMENTS["handler_set_bytes"] // 100
    capacity = MEASUREMENTS["l1i_bytes"] // per_body   # ~18 bodies fit in L1i
    n = 40
    rng = np.random.default_rng(11)
    w = 1.0 / np.arange(1, n + 1) ** 0.7
    calls = rng.choice(n, size=30, p=w / w.sum())
    m = MEASUREMENTS["mech"]["virtual"]

    fig, ax = plt.subplots(figsize=(7.8, 3.5))
    ax.set_xlim(-0.5, n + 0.5)
    ax.set_ylim(-1.5, 4.3)
    ax.axis("off")

    def row(y, label):
        ax.text(0, y + 1.05, label, fontsize=9.5, color=INK)
        bs = []
        for i in range(n):
            r = Rectangle((i, y), 0.86, 0.78, facecolor="#ededed", edgecolor="#d8d8d8", lw=0.5)
            ax.add_patch(r)
            bs.append(r)
        return bs

    # round(), not //: 180096 B is 175.9 KiB, and flooring it to 175 would
    # contradict the 176 KiB the solution quotes for the same number.
    set_kib = round(MEASUREMENTS["handler_set_bytes"] / 1024)
    hot_boxes = row(2.5, "hot:  every packet reaches the same handler")
    mix_boxes = row(0.35, f"mixed:  100 handlers, {set_kib} KiB of code, L1i holds ~{capacity}")

    # Right-aligned on the row's own label line, clear of the boxes.
    hot_txt = ax.text(n + 0.4, 3.55, "", fontsize=9.5, color=INK, ha="right")
    mix_txt = ax.text(n + 0.4, 1.40, "", fontsize=9.5, color=INK, ha="right")
    note = ax.text(0, -1.15, "", fontsize=8.5, color=MUTED)

    resident: list[int] = []
    misses = 0

    def update(f):
        nonlocal misses, resident
        if f == 0:
            resident, misses = [], 0

        if f > 0:
            h = int(calls[(f - 1) % len(calls)])
            if h in resident:
                resident.remove(h)
            else:
                misses += 1
            resident.append(h)
            if len(resident) > capacity:
                resident.pop(0)

            for i, b in enumerate(hot_boxes):
                b.set_facecolor(ACCENT if i == 0 else "#ededed")
            for i, b in enumerate(mix_boxes):
                if i == h:
                    b.set_facecolor(ACCENT)
                elif i in resident:
                    b.set_facecolor("#cfe3d5")
                else:
                    b.set_facecolor("#ededed")

            hot_txt.set_text(f"refetched: 0        {m[0]:.1f} ns/packet")
            mix_txt.set_text(f"refetched: {misses:<3d}      {m[1]:.1f} ns/packet")

        if f >= len(calls):
            note.set_text(
                f"Same selection, same mechanism, same handler bodies. What differs is how many of them the\n"
                f"front end has to keep fetching, and whether it can guess the call's target: {m[3]:.1f} ns/packet\n"
                "together, three times what the dispatch mechanism itself costs."
            )
        return hot_boxes + mix_boxes + [hot_txt, mix_txt, note]

    anim = animation.FuncAnimation(fig, update, frames=len(calls) + 8, interval=140, blit=False)
    path = HERE / "anim3-icache.gif"
    anim.save(path, writer=animation.PillowWriter(fps=7), dpi=100, savefig_kwargs={"facecolor": BG})
    plt.close(fig)
    print(f"  {'anim3-icache.gif':38} {path.stat().st_size / 1024:8.1f} KiB")


def main() -> None:
    print("figures:")
    fig1_scan_vs_table()
    fig2_where_the_win_comes_from()
    fig3_mechanism()
    fig4_budget()
    print("animations:")
    anim1_scan_vs_table()
    anim2_frequency_ordering()
    anim3_icache()


if __name__ == "__main__":
    main()
