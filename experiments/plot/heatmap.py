"""Shared K×L speedup heatmap pair used by Figures 10 and 11."""
import numpy as np
import matplotlib.pyplot as plt

from style import label


def speedup_pivot(df, metric, numerator_tree, denominator_tree):
    """speedup = time(numerator_tree) / time(denominator_tree), L on rows (desc), K on columns."""
    num = df[df["tree_type"] == numerator_tree][["K", "L", metric]].rename(columns={metric: "num"})
    den = df[df["tree_type"] == denominator_tree][["K", "L", metric]].rename(columns={metric: "den"})
    m = num.merge(den, on=["K", "L"])
    m["speedup"] = m["num"] / m["den"]
    p = m.pivot(index="L", columns="K", values="speedup").sort_index(axis=1).sort_index(ascending=False)
    if 0 in p.index and 0 in p.columns:  # K=0,L=0 is the only fully sorted cell; blank the rest of row/col 0
        v = p.loc[0, 0]
        p.loc[:, 0] = np.nan
        p.loc[0, :] = np.nan
        p.loc[0, 0] = v
    return p


def draw(ax, pivot, title, vmin, vmax, font_size):
    data = pivot.values.astype(float)
    cmap = plt.get_cmap("viridis").copy()
    cmap.set_bad("white")
    im = ax.imshow(data, cmap=cmap, vmin=vmin, vmax=vmax, aspect="equal")
    for i in range(data.shape[0] + 1):
        ax.axhline(i - 0.5, color="white", linewidth=0.5)
    for j in range(data.shape[1] + 1):
        ax.axvline(j - 0.5, color="white", linewidth=0.5)
    for i in range(data.shape[0]):
        for j in range(data.shape[1]):
            v = data[i, j]
            if np.isfinite(v):
                b = (v - vmin) / (vmax - vmin) if vmax > vmin else 0.5
                ax.text(j, i, f"{v:.2f}{label('times')}", ha="center", va="center", fontsize=font_size,
                        color="black" if b > 0.5 else "white")
    ax.set_xticks(range(len(pivot.columns)))
    ax.set_xticklabels([int(c) for c in pivot.columns])
    ax.set_yticks(range(len(pivot.index)))
    ax.set_yticklabels([int(r) for r in pivot.index])
    ax.set_title(title, fontweight="bold", pad=24)
    ax.set_xlabel(f"K ({label('pct')})")
    return im


def heatmap_pair(df, numerator_tree, denominator_tree, title_a, title_b, font_size=30, pad=0.01):
    pi = speedup_pivot(df, "avg_insert_ns", numerator_tree, denominator_tree)
    pq = speedup_pivot(df, "avg_query_ns", numerator_tree, denominator_tree)
    vmin = min(np.nanmin(pi.values), np.nanmin(pq.values))
    vmax = max(np.nanmax(pi.values), np.nanmax(pq.values))
    fig, axes = plt.subplots(1, 2, figsize=(26, 10))
    fig.subplots_adjust(wspace=0.08)
    draw(axes[0], pi, title_a, vmin, vmax, font_size)
    axes[0].set_ylabel(f"L ({label('pct')})")
    im = draw(axes[1], pq, title_b, vmin, vmax, font_size)
    cb = fig.colorbar(im, ax=axes.tolist(), shrink=0.8, pad=pad)
    cb.ax.tick_params(labelsize=font_size)
    print("insert speedup:\n" + pi.round(2).to_string())
    print("query speedup:\n" + pq.round(2).to_string())
    return fig
