"""Shared matplotlib style for the paper figures.

Every plot.py does:
    from style import apply, label, save
    apply(font_size=15)
    ...
    save(fig, out_path)

LaTeX text rendering (Libertine, as in the paper) is used when available:
    QUART_USETEX=1|0   force on / off (default: auto-detect `latex` + libertine.sty)
    QUART_PDFCROP=0    skip the optional `pdfcrop` pass after saving
"""
import os
import shutil
import subprocess

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def _usetex_available():
    env = os.environ.get("QUART_USETEX", "auto").lower()
    if env in ("1", "true", "yes"):
        return True
    if env in ("0", "false", "no"):
        return False
    if not shutil.which("latex") or not shutil.which("kpsewhich"):
        return False
    try:
        r = subprocess.run(["kpsewhich", "libertine.sty"], capture_output=True, text=True)
        return r.returncode == 0 and r.stdout.strip() != ""
    except OSError:
        return False


USETEX = _usetex_available()


def apply(font_size=15):
    plt.rcParams["font.family"] = "serif"
    plt.rcParams["font.serif"] = ["Linux Libertine O", "Linux Libertine", "Libertine", "DejaVu Serif"]
    plt.rcParams["text.usetex"] = USETEX
    if USETEX:
        plt.rcParams["text.latex.preamble"] = r"\usepackage{libertine}"
    plt.rcParams.update({
        "font.size": font_size, "axes.titlesize": font_size, "axes.labelsize": font_size,
        "xtick.labelsize": font_size, "ytick.labelsize": font_size, "legend.fontsize": font_size,
    })


# (LaTeX form, plain form)
_LABELS = {
    "ART": ("ART", "ART"),
    "QuART_tail": (r"QuART-\emph{tail}", "QuART-tail"),
    "QuART_lil": (r"QuART-$\ell\mkern1mu i\mkern1mu\ell$", "QuART-lil"),
    "QuART_stail": (r"QuART-\emph{stail}", "QuART-stail"),
    "tail": (r"\emph{tail}", "tail"),
    "lil": (r"$\ell\mkern1mu i\mkern1mu\ell$", "lil"),
    "stail": (r"\emph{stail}", "stail"),
    "B+-tree": (r"B$^+$-tree", "B+-tree"),
    "QuIT": ("QuIT", "QuIT"),
    "QuIT_4k": ("QuIT (4k)", "QuIT (4k)"),
    "RAX": (r"\emph{rax}", "rax"),
    "QuART-rax": (r"QuART-\emph{rax}", "QuART-rax"),
    "pct": (r"\%", "%"),
    "times": (r"$\times$", "x"),
    "hash": (r"\#", "#"),
}


def label(key):
    tex, plain = _LABELS.get(key, (key, key))
    return tex if USETEX else plain


def save(fig, path):
    path = os.path.abspath(path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fig.savefig(path, bbox_inches="tight")
    plt.close(fig)
    if os.environ.get("QUART_PDFCROP", "1") != "0" and shutil.which("pdfcrop") and path.endswith(".pdf"):
        subprocess.run(["pdfcrop", path, path], check=False, capture_output=True)
    print(f"wrote {path}")
