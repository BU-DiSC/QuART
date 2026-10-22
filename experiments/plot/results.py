"""Locate and load an experiment's result CSV.

    from results import add_args, locate, load, mops
    args = add_args(argparse.ArgumentParser(), EXP_DIR, "quart_vs_art.csv", "quart_vs_art.pdf").parse_args()
    df = load(locate(EXP_DIR, "quart_vs_art.csv", args.results))

Resolution order for the CSV:
  1. --results PATH (a CSV file, or a directory containing the CSV)
  2. QUART_RESULTS=<run name>  →  <exp>/results/<run name>/<csv>  ("paper" = the paper's data)
  3. <exp>/results/latest/<csv>, the default run.sh output
  4. <exp>/results/paper/<csv>  (the data behind the paper's figure)
"""
import os
import sys

import pandas as pd

FIGURES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "figures")


def add_args(parser, exp_dir, csv_name, out_name):
    parser.add_argument("--results", default=None,
                        help=f"CSV file or results directory (default: results/latest, else results/paper/{csv_name})")
    parser.add_argument("--out", default=os.path.join(FIGURES_DIR, out_name),
                        help=f"output file (default: experiments/figures/{out_name})")
    return parser


def locate(exp_dir, csv_name, cli=None):
    if cli:
        return cli if os.path.isfile(cli) else os.path.join(cli, csv_name)
    paper = os.path.join(exp_dir, "results", "paper", csv_name)
    run = os.environ.get("QUART_RESULTS", "latest")
    if run == "paper":
        return paper
    named = os.path.join(exp_dir, "results", run, csv_name)
    return named if os.path.exists(named) else paper


def load(path, numeric=()):
    """Read the CSV; rows with an ERROR cell (failed runs) are dropped with a warning."""
    if not os.path.exists(path):
        sys.exit(f"error: no results at {path} (run the experiment's run.sh first)")
    df = pd.read_csv(path, dtype=str)
    bad = df.apply(lambda r: r.astype(str).str.contains("ERROR").any(), axis=1)
    if bad.any():
        print(f"warning: dropping {int(bad.sum())} ERROR row(s) from {path}", file=sys.stderr)
        df = df[~bad]
    text_cols = {"workload", "tree_type", "tree", "insertion_method", "panel", "verify", "bulk_path", "sorted",
                 "grouped", "config"}
    for c in df.columns:
        if c in numeric or c not in text_cols:
            try:
                df[c] = pd.to_numeric(df[c])
            except (ValueError, TypeError):
                pass
    print(f"data: {path}")
    return df


def mops(ops, ns):
    """Throughput in million operations per second from an op count and a total time in ns."""
    return ops / ns * 1e3
