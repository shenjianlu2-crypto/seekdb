#!/usr/bin/env python3
"""Generate the FTS benchmark corpus: 20,000 mixed Chinese/English documents.

Row layout (fixed, so the baseline and the optimized build score identical data):

  id <= 14000   Chinese dominant documents  (title and content in Chinese,
                with English technical terms mixed in)
  id >  14000   English dominant documents  (title and content in English)

The output is a TSV file that `bench.py` streams into the table with batched
multi-row INSERTs of 500 rows each, matching the official benchmark's
"每批导入 500 行" behaviour.
"""

import argparse
import json
import random
from pathlib import Path

ZH_ROWS = 14000

CN_SUBJECTS = [
    "海洋数据库", "全文索引", "查询优化", "向量化检索", "混合检索", "事务内核",
    "存储引擎", "日志服务", "分布式事务", "租户资源隔离", "列式存储", "行式存储",
    "并行执行", "内存管理", "垃圾回收", "网络协议", "数据压缩", "备份恢复",
    "智能运维", "向量检索", "文档分析", "分词器", "倒排索引", "词典加载",
    "索引构建", "增量合并", "外部排序", "结果合并", "查询下推", "代价估算",
    "数据字典", "日志流", "合并策略", "内存池", "并发控制", "负载均衡",
]

CN_VERBS = [
    "显著提升了", "持续优化了", "有效降低了", "稳定增强了", "显著改进了",
    "大幅压缩了", "彻底修复了", "完整实现了", "高效支持了", "平滑升级了",
    "持续加速了", "明显减少了", "稳定保障了", "全面优化了", "细致打磨了",
]

CN_OBJECTS = [
    "写入吞吐", "查询延迟", "索引构建耗时", "内存占用", "磁盘IO开销",
    "分词耗时", "倒排链访问效率", "排序归并效率", "结果集合并效率",
    "批量导入速度", "并发查询能力", "冷启动时间", "资源利用率", "稳定性",
    "可观测性", "容错能力", "资源回收效率", "锁竞争程度", "任务调度效率",
    "数据分发效率", "流水线衔接效率", "临时对象分配开销",
]

CN_TAILS = [
    "在生产环境中的表现", "在混合负载下的表现", "在海量数据下的表现",
    "在多租户场景下的表现", "在高并发场景下的表现", "在长时间压测中的表现",
    "在冷热数据混合场景下的表现", "在跨机房部署下的表现",
    "在索引结构不断膨胀时的表现", "在批量导入与查询交替时的表现",
]

CN_CONNECTORS = [
    "与此同时", "此外", "因此", "然而", "进一步地", "值得注意的是",
    "从工程实践来看", "经过多轮迭代", "在最新的版本中", "根据线上观测",
    "结合内核源码分析", "结合火焰图定位",
]

CN_TECH = ["OceanBase", "seekdb", "MySQL", "TokuDB", "Lucene", "Elasticsearch", "Gin", "Kafka", "Redis"]

EN_SUBJECTS = [
    "OceanBase", "full text search", "vector database", "hybrid retrieval",
    "transactional engine", "storage layer", "log service", "query optimizer",
    "columnar storage", "memory manager", "parallel execution", "index builder",
    "tokenizer", "inverted index", "dictionary loader", "result merger",
    "partition manager", "compaction scheduler", "raft consensus", "sql parser",
    "disk io pipeline", "block cache", "row cache", "query rewriter",
]

EN_VERBS = [
    "significantly improves", "consistently reduces", "dramatically improves",
    "effectively eliminates", "reliably accelerates", "substantially lowers",
    "consistently delivers", "measurably shortens", "robustly handles",
    "steadily increases",
]

EN_OBJECTS = [
    "the indexing build time", "the query latency", "the tokenization throughput",
    "the memory footprint", "the disk io overhead", "the inverted list access cost",
    "the merge sort pipeline", "the batch import throughput", "the cold start time",
    "the concurrency scalability", "the resource utilization", "the overall stability",
    "the intermediate spill traffic", "the result aggregation cost",
]

EN_TAILS = [
    "under a mixed read write workload", "on a production sized dataset",
    "in a multi tenant deployment", "during a long running stress test",
    "with a cold and hot data mix", "across multiple availability zones",
    "when the inverted index grows beyond memory",
]

EN_CONNECTORS = [
    "Meanwhile", "In addition", "As a result", "However", "Furthermore",
    "It is worth noting that", "After several iterations", "In the latest release",
    "Based on production telemetry", "From an engineering perspective",
]


def cn_sentence(rng):
    text = (rng.choice(CN_SUBJECTS) + rng.choice(CN_VERBS)
            + rng.choice(CN_OBJECTS) + rng.choice(CN_TAILS))
    if rng.random() < 0.5:
        text = rng.choice(CN_CONNECTORS) + "，" + text
    if rng.random() < 0.3:
        text = text.replace("。", "，" + rng.choice(CN_TECH) + " 平台同样受益。", 1)
    return text + "。"


def en_sentence(rng):
    text = " ".join([rng.choice(EN_SUBJECTS), rng.choice(EN_VERBS),
                     rng.choice(EN_OBJECTS), rng.choice(EN_TAILS)])
    if rng.random() < 0.5:
        text = rng.choice(EN_CONNECTORS) + ", " + text
    return text + "."


def make_doc(rng, index):
    chinese = index <= ZH_ROWS
    if chinese:
        title = " ".join(rng.sample(CN_SUBJECTS, 2)) + " " + rng.choice(CN_OBJECTS)
        n_cn, n_en = rng.randint(5, 13), rng.randint(0, 3)
    else:
        title = " ".join(rng.sample(EN_SUBJECTS, 2)) + " " + rng.choice(EN_OBJECTS)
        n_cn, n_en = rng.randint(0, 3), rng.randint(5, 13)

    chunks = [cn_sentence(rng) for _ in range(n_cn)]
    chunks += [en_sentence(rng) for _ in range(n_en)]
    rng.shuffle(chunks)
    content = " ".join(chunks)
    return title, content


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rows", type=int, default=20000)
    parser.add_argument("--seed", type=int, default=20261029)
    parser.add_argument("--out", default="data/docs.tsv")
    options = parser.parse_args()

    rng = random.Random(options.seed)
    out_path = Path(options.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", encoding="utf-8", newline="\n") as handle:
        for index in range(1, options.rows + 1):
            title, content = make_doc(rng, index)
            title = title.replace("\t", " ").replace("\n", " ")
            content = content.replace("\t", " ").replace("\n", " ")
            handle.write("%d\t%s\t%s\n" % (index, title, content))
    meta = {"rows": options.rows, "seed": options.seed, "zh_rows": ZH_ROWS,
            "file": str(out_path)}
    Path("data/corpus_meta.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
    print("generated %d docs -> %s" % (options.rows, out_path))


if __name__ == "__main__":
    main()
