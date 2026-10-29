# 赛题四：全文索引调优 —— 优化报告

- 项目：`oceanbase/seekdb`（分支 `2026-competition-fts`，基线提交 `9cd99bfc`）
- 优化提交：`ac4fdbc4` perf(fts): speed up fulltext index build and IK tokenization
- 本机环境：Windows 11 + MSVC/Bazel 编译，seekdb 单实例 `cpu_count=8 / memory_limit=8G`，同机 A/B 对比

---

## 1. 基准复现

官方 CI 使用的 `tools/benchmark/fts_large_bench*` 未随公开仓库发布，因此按赛题描述复刻了等价基准（脚本位于 `ftsbench/`）：

| 项目 | 官方描述 | 本地复现 |
|---|---|---|
| 数据规模 | 20,000 篇中英文文档 | 14,000 篇中文 + 6,000 篇英文（确定性种子） |
| 导入 | 每批 500 行 | 每批 500 行 INSERT（实测 5.8 万行/秒） |
| 索引构建 | IK(标题+正文)、IK(正文)、BENG(标题+正文) | 同左，各自独立计时 |
| 分词 | 每组 3,000 次，3 组，预热 30 次 | 同左（1 条语句内完成 3,000 次 TOKENIZE 调用） |
| 查询 | 每条每组 200 次，3 组，预热 30 次 | 同左（中文/英文/中英混合/带 LIMIT 各一组） |
| 计分 | 三类等权 → `100 × min(max(综合提升率,0),0.5)/0.5` | 同左（`ftsbench/score.py`） |

辅助观测项一并采集：`SELECT 1` 单次往返开销（用于判断协议噪声）、原始导入速度、每条查询的命中数量与**完整命中行集合**（用于正确性核对）。

---

## 2. 瓶颈定位（依据与实测数据）

### 2.1 索引构建：单 worker 跑满整个建索引过程

在 `CREATE FULLTEXT INDEX` 期间采样进程 CPU 时间：

| 阶段 | 墙钟 | 进程 CPU | 等效并行度 |
|---|---|---|---|
| 原始版本 | 8.92 s | 18.27 s | **2.05 核 / 8 核** |

根因链条：

1. `ObDDLResolver::calc_ddl_parallelism()`（`src/sql/resolver/ddl/ob_ddl_resolver.cpp:7750`）在 `_enable_parallel_ddl` 未开启时把 DDL 并行度置为 **1**（日志实测 `create ddl task(param={... parallelism:1 ...})`）。
2. `ObFtsIndexBuilderUtil::decide_parallelism()`（`src/sql/resolver/ddl/ob_fts_index_builder_util.cpp:1034`）对 FTS 辅助表再执行 `MAX(original_parallelism / 3, 1)`，把已经为 1 的并行度保留为 1。
3. 一个全文索引会派生三张辅助表 `fts_doc_rowkey` / `fts_index_aux` / `fts_doc_word`，它们**依次**构建，因此每张表都在只有 1 个 worker 的情况下重建 140 万行。

实测并行度扫描（`PARALLEL(n)` hint，其余条件不变）：

| 并行度 | 1（实际回落到 16） | 2 | 4 | 8 | 16 |
|---|---|---|---|---|---|
| 建索引耗时 | 13.75 s | **7.28 s** | 7.38 s | 7.37 s | 13.86 s |

结论：2~4 个 worker 是拐点，再多只增加归并开销。

### 2.2 分词：逐字符重复解码 + 处理器重复取字符

`ObFTCharUtil::do_classify()` 原实现对每个字符依次调用 `is_alpha / is_arabic / is_chinese / is_other_cjk`，**每个函数各自调用一次 `decode_unicode()`**，一个汉字最多被解码 4 次；随后 4 个 segmenter（letter / quantifier / cjk / surrogate）又各自通过 `ctx.current_char()` / `ctx.current_char_type()` 重新取一遍当前字符与类型。

按字符种类实测单字符成本（固定开销 12.1 µs/次已扣除）：

| 输入 | 每字符耗时 | 说明 |
|---|---|---|
| 300 个汉字 | **370 ns** | 1 字 = 1 token，含词典匹配 |
| 300 个 ASCII | 167 ns | 1 词 ≈ 0.1 token，不查词典 |
| 12 种不同汉字 | 346 ns | 词典映射表全部命中 L1 |
| 500 种不同汉字 | 572 ns | 词典映射表随机访问，缓存缺失 |

### 2.3 查询：ORDER BY 场景下全量回表 + 全量排序

`EXPLAIN` 显示 `ORDER BY id LIMIT 10` 走 `TOP-N SORT → TEXT RETRIEVAL SCAN(is_index_back=true)`，实测：

| 语句 | 耗时 |
|---|---|
| `COUNT(*) WHERE MATCH(...)`（3700 命中） | 0.85 ms |
| `id ... LIMIT 10`（无 ORDER BY，limit 下推） | 1.31 ms |
| `id ... ORDER BY id LIMIT 10`（3690 命中） | **10.50 ms** |
| `id ... ORDER BY id LIMIT 100` | 10.66 ms |

`LIMIT` 从 10 改成 100 耗时不变，说明排序阶段处理的是**全部命中行**（逐行回表 + 全量排序），`LIMIT` 只在最后才生效。查询耗时与命中数近似线性（0.85 ms@3700 → 3.09 ms@12771，约 0.25 µs/命中）。

---

## 3. 所做修改

### 3.1 构建并行度（`src/sql/resolver/ddl/ob_fts_index_builder_util.cpp`）

```cpp
case share::schema::ObIndexType::INDEX_TYPE_DOC_ID_ROWKEY_LOCAL:
case share::schema::ObIndexType::INDEX_TYPE_FTS_INDEX_LOCAL:
case share::schema::ObIndexType::INDEX_TYPE_FTS_DOC_WORD_LOCAL: {
  // 三张辅助表依次构建，可以让当前这张表用满 CPU；上限 4，因为再多的
  // worker 只带来归并开销。
  int64_t parallelism = original_parallelism;
  if (parallelism <= 1) {
    const int64_t cpu_count = config::cpu_count() > 0 ? config::cpu_count() : common::get_cpu_num();
    parallelism = MIN(cpu_count, static_cast<int64_t>(MAX_FTS_BUILD_PARALLELISM));
  }
  decided_parallelism = MAX(parallelism, 1L);
  break;
}
```

保留 `original_parallelism > 1` 时的原值，因此显式 `PARALLEL(n)` hint 与 `_enable_parallel_ddl` 的语义不变。

### 3.2 IK 分词：字符只解码一次

- `src/storage/fts/ik/ob_ik_char_util.h`
  - `do_classify()` 改为**解码一次**，随后用码点判定字符类别；新增 `decode_char()` 以及按码点判定的 `check_cn_number / check_num_connector / check_letter_connector / is_ignore_single_cjk` 重载（字符串版本保留，语义不变）。
  - `classify_first_char()` 新增一个带 `ob_wc_t &unicode` 的重载，分类的同时带回码点。
- `src/storage/fts/ik/ob_ik_processor.h/.cpp`
  - `TokenizeContext` 缓存当前字符的码点 `next_char_unicode_`；`ObIIKProcessor::process()` 改为接收已取好的 `ch / char_len / type / unicode`，4 个 segmenter 不再各自回查上下文。
- `ob_ik_letter_processor` / `ob_ik_quantifier_processor`：连接符、中文数字判定改用码点版本，少一次解码。
- `ob_ik_cjk_processor` / `ob_ik_surrogate_processor`：仅调整 `do_process` 签名。

### 3.3 IK 词典区间查找（`src/storage/fts/dict/ob_ft_range_dict.*`）

`find_first_char_range()` 原先对每个 CJK 字符线性扫描全部区间，每区 2 次 `ObCharset::strcmp`。现在先命中「上一个匹配到的区间」（同一文档的相邻字符几乎总落在同一区间），未命中再回退到原线性扫描。结果完全等价。

---

## 4. 优化前后对比

两轮独立 A/B（每轮均为「原始版本重新编译 → 跑基准 → 切回优化版本重新编译 → 跑基准」，同一台机器）：

### A/B 第 1 轮（机器负载低，`SELECT 1` 往返 0.084 ms）

| 指标 | 基线 | 优化后 | 提升 |
|---|---|---|---|
| build_ik_all_sec | 9.5029 | 7.8818 | **17.06%** |
| build_ik_content_sec | 8.9894 | 7.3775 | **17.93%** |
| build_beng_en_sec | 2.6371 | 2.2314 | **15.38%** |
| tokenize_ik_avg_ms | 0.0554 | 0.0512 | 7.53% |
| tokenize_beng_avg_ms | 0.0125 | 0.0129 | −3.75%（噪声） |
| query_cn_avg_ms | 1.8179 | 1.7478 | 3.86% |
| query_beng_avg_ms | 1.0160 | 0.9262 | 8.84% |
| query_mixed_avg_ms | 2.0290 | 1.9960 | 1.63% |
| query_limit_avg_ms | 10.5032 | 10.4217 | 0.78% |
| **构建均值** | | | **16.79%** |
| **分词均值** | | | **1.89%** |
| **查询均值** | | | **3.78%** |
| **综合提升率 / 得分** | | | **7.49% / 14.97** |

### A/B 第 2 轮（机器有约 24% 外部负载，`SELECT 1` 往返 0.126 ms，绝对值整体偏高）

| 指标 | 基线 | 优化后 | 提升 |
|---|---|---|---|
| build_ik_all_sec | 11.0440 | 9.2067 | 16.64% |
| build_ik_content_sec | 10.5208 | 8.7970 | 16.38% |
| build_beng_en_sec | 3.0458 | 2.5429 | 16.51% |
| tokenize_ik_avg_ms | 0.0619 | 0.0615 | 0.66% |
| tokenize_beng_avg_ms | 0.0152 | 0.0159 | −4.67%（噪声） |
| query_cn_avg_ms | 2.1690 | 2.1244 | 2.06% |
| query_beng_avg_ms | 1.3340 | 1.1929 | 10.58% |
| query_mixed_avg_ms | 2.4252 | 2.3571 | 2.81% |
| query_limit_avg_ms | 12.4039 | 11.8625 | 4.36% |
| **综合提升率 / 得分** | | | **6.49% / 12.97** |

**结论：构建类提升稳定在 16%~18%；分词类提升 0%~7%（受外部负载影响明显）；查询类 2%~5%。**

---

## 5. 正确性验证

1. **分词结果逐字节比对**：`ftsbench/verify_tokenize.py` 对 128 篇语料文本 + 18 组手工边界用例（大写、空格、全角空格、中英文混排、数字、重复串、错误字符等），分别用 `ik / beng / space / ngram` 四种解析器调用 `TOKENIZE()`，加上 6 组 `MATCH` 的**完整命中行集合与相关性分数**，共 590 行输出。
   - 原始版本与优化版本的两份 dump **逐字节完全一致**。
2. **基准内置核对**：两轮 A/B 中 12 组查询的命中数量与命中行集合在基线与优化版本之间完全一致（`score.py` 的 correctness 检查通过）。
3. **未改变语义**：本次修改只涉及「解码次数」「参数传递方式」「区间查找顺序」「DDL 并行度」，没有触碰分词规则、词典内容、索引表结构与查询算子。

---

## 6. 过程中发现的一个既有缺陷（未修改，仅记录）

IK 分词器在某些输入上会切出**半个多字节字符**：词典命中的 token 边界可能落在 UTF-8 字符中间，导致 `TOKENIZE()` 返回非法 UTF-8，服务端报 `ERROR 1210 (Invalid argument)`，且错误包里带着坏字节会让 MySQL 协议连接失步（客户端报 `Packet sequence number wrong`）。

- 最小复现：语料第 7 篇中文文本的前 152 个字符即触发；`SUBSTR(content,1,512)` 正常、`SUBSTR(content,1,513)` 失败。
- 该缺陷在**原始版本中同样存在**，本次优化未改变其行为（基线/优化两版 dump 中该用例的报错完全一致）。
- 因为改动它会改变索引内容与 `MATCH` 命中结果，与赛题「保持分词结果及 MATCH 查询语义正确」冲突，本次未做修改，仅在基准中避开会触发该缺陷的输入。
- 建议后续单独立项：定位 `ObFTRangeDict::match_with_hit` 命中终点落在字符中间的边界计算。

---

## 7. 后续可挖掘的优化点（本次未实施，附定位）

| 方向 | 定位 | 预期收益 |
|---|---|---|
| 构建期二次分词 | `fts_index_aux` 与 `fts_doc_word` 两张辅助表各对主表做了一次完整分词（`src/sql/das/ob_das_domain_utils.cpp:276`），二者仅列序不同 | 消除一次分词 ≈ 构建 10%+ |
| 构建期写放大 | 3.4M 辅助行落盘占据构建主要时间，且并行扩展性差（4 核后不再变快），瓶颈在存储写入 | 需改索引表结构，风险高 |
| 查询 top-N 下推 | `ORDER BY id LIMIT n` 时 TOP-N 排序仍需回表全部命中行；`src/sql/engine/table/ob_table_scan_op.cpp:1115` 因「挂了 pushdown 算子」而禁掉了 LIMIT 下推 | 该类查询可从 10.5 ms 降到 ~1.3 ms |
| 分词词典访问 | `ObArrayHashMap::find()`（`src/storage/fts/dict/ob_ft_dat_dict.cpp:302`）每字符一次哈希表查找 + 取模，随机访问 3.6MB 映射表；可加线程局部 (字符→码点) 记忆化 | 中文分词再降 10%~20% |
| 分词单 token 成本 | 实测每 token 约 226 ns：仲裁器链管理、`ObFTWordMap` 双次哈希、`casedown` 分配+拷贝+大小写转换、JSON 节点分配各占一部分 | 逐项可再压 |
| 词典初始化 | 每篇文档/每条查询都重建 IK 词典区间对象（每区间一次 KV cache 查找 + 分配） | 固定开销约占分词 24% |

---

## 8. 复现方式

```bash
# 编译
bash ftsbench/build_seekdb.sh            # 内部执行 build.py build ... seekdb
# 起一个干净实例
bash ftsbench/start_seekdb.sh            # 全新数据目录，端口 2881
python ftsbench/wait_ready.py
# 跑基准
python ftsbench/bench.py --out reports/xxx.json --label xxx
# 计分（基线 vs 候选）
python ftsbench/score.py --baseline reports/base.json --candidate reports/xxx.json
# 正确性逐字节比对
python ftsbench/verify_tokenize.py reports/verify_xxx.txt
diff reports/verify_base.txt reports/verify_xxx.txt
```
