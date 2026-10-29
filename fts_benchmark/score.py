#!/usr/bin/env python3
"""Score a benchmark report against the baseline, using the official formula.

  单项提升率 = (基线耗时 - 提交版本耗时) / 基线耗时
  综合提升率 = (构建平均提升率 + 分词平均提升率 + 查询平均提升率) / 3
  性能得分   = 100 * min(max(综合提升率, 0), 0.5) / 0.5
"""

import argparse
import json
import sys
from pathlib import Path

BUILD_METRICS = ["build_ik_all_sec", "build_ik_content_sec", "build_beng_en_sec"]
TOKENIZE_METRICS = ["tokenize_ik_avg_ms", "tokenize_beng_avg_ms"]
QUERY_METRICS = ["query_cn_avg_ms", "query_beng_avg_ms", "query_mixed_avg_ms",
                 "query_limit_avg_ms"]
ALL_METRICS = BUILD_METRICS + TOKENIZE_METRICS + QUERY_METRICS

CATEGORY_OF = {}
for metric in BUILD_METRICS:
    CATEGORY_OF[metric] = "build"
for metric in TOKENIZE_METRICS:
    CATEGORY_OF[metric] = "tokenize"
for metric in QUERY_METRICS:
    CATEGORY_OF[metric] = "query"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--out", default="")
    options = parser.parse_args()

    baseline = json.loads(Path(options.baseline).read_text(encoding="utf-8"))
    candidate = json.loads(Path(options.candidate).read_text(encoding="utf-8"))
    base_metrics = baseline["metrics"]
    cand_metrics = candidate["metrics"]

    missing = [m for m in ALL_METRICS if m not in base_metrics or m not in cand_metrics]
    if missing:
        print("missing metrics: %s" % missing)
        return 2

    lines = []
    lines.append("%-24s %14s %14s %10s" % ("metric", "baseline", "candidate", "improve"))
    lines.append("-" * 66)
    improvements = {}
    for metric in ALL_METRICS:
        base = float(base_metrics[metric])
        cand = float(cand_metrics[metric])
        improve = (base - cand) / base if base else 0.0
        improvements[metric] = improve
        lines.append("%-24s %14.4f %14.4f %9.2f%%"
                     % (metric, base, cand, improve * 100))

    category_avg = {}
    for category, metrics in (("build", BUILD_METRICS),
                               ("tokenize", TOKENIZE_METRICS),
                               ("query", QUERY_METRICS)):
        values = [improvements[m] for m in metrics]
        category_avg[category] = sum(values) / len(values)
        lines.append("%-24s %38s %9.2f%%" % ("  " + category + " avg", "",
                                             category_avg[category] * 100))

    composite = sum(category_avg.values()) / 3
    score = 100 * min(max(composite, 0.0), 0.5) / 0.5
    lines.append("-" * 66)
    lines.append("%-24s %38s %9.2f%%" % ("composite improvement", "", composite * 100))
    lines.append("%-24s %38s %9.2f" % ("SCORE", "", score))

    # correctness cross check: hit counts and full hit sets
    correctness = []
    base_hits = baseline.get("hits", {})
    cand_hits = candidate.get("hits", {})
    for key in sorted(set(base_hits) | set(cand_hits)):
        if base_hits.get(key) != cand_hits.get(key):
            correctness.append("hit count mismatch %s: %s != %s"
                               % (key, base_hits.get(key), cand_hits.get(key)))
    base_sets = baseline.get("hit_sets", {})
    cand_sets = candidate.get("hit_sets", {})
    for key in sorted(set(base_sets) | set(cand_sets)):
        if base_sets.get(key) != cand_sets.get(key):
            correctness.append("hit set mismatch %s" % key)
    if correctness:
        lines.append("")
        lines.append("CORRECTNESS PROBLEMS:")
        for item in correctness:
            lines.append("  " + item)
    else:
        lines.append("correctness: hit counts and hit sets identical")

    text = "\n".join(lines)
    print(text)
    if options.out:
        Path(options.out).write_text(text + "\n", encoding="utf-8")
    return 0 if not correctness else 1


if __name__ == "__main__":
    sys.exit(main())
