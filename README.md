# MiniVectorDB

2026 OceanBase 数据库大赛加赛赛题一：支持 SQL、持久化存储与事务的轻量向量数据库。

## 语言与构建

- 语言：C++17，无第三方依赖（存储、查询执行、向量索引、事务核心均自行实现）。
- 编译器：g++ 13（Ubuntu 24.04 容器内构建）。
- 本地构建：`make`
- 镜像构建：`docker build -t minivectordb:latest .`

## 启动

```bash
docker run --rm -i -v minivectordb-data:/data --cpus=2 --memory=2g minivectordb:latest
```

或直接运行：`./minivectordb --data-dir /data [--mode exact|ann] [--cache-mb 32]`

- 前台运行，stdin 接收 SQL（以 `;` 结束一条语句，支持多行），stdout 返回结果，stderr 输出日志。
- 启动并完成恢复与索引重建后输出 `READY`。
- 数据、日志、索引、临时文件全部位于 `--data-dir`（默认 `/data`）。
- 正常退出：发送 `exit;` 或关闭 stdin；进程回滚未提交事务并落盘。
- 精确/近似模式：`--mode` 或 `SET vector_mode = 'ann';` / `SET vector_mode = 'exact';`，默认 exact。

## 输出约定

- DDL/DML/事务控制成功：`SUCCESS`；失败：`FAILURE`（错误详情写 stderr）。
- 查询：首行列名，列间 ` | `；随后每行一条记录；空结果只输出列名。
- 浮点保留两位小数去多余 0；向量输出 `[a, b, c]`；字符串不带引号。

## SQL 支持范围

- `CREATE TABLE`：BIGINT、VARCHAR(n)、VECTOR(n)；一个 BIGINT 主键；每表最多一个向量列。
- `INSERT`（含多行）、`SELECT`（投影、`*`、`L2_DISTANCE`、WHERE 比较与 AND、ORDER BY、LIMIT）、`UPDATE`、`DELETE`。
- `BEGIN` / `COMMIT` / `ROLLBACK`；事务内读到自己的修改；语句失败原子回滚该语句。
- `CREATE VECTOR INDEX name ON table(col)`：自研 IVF-Flat（k-means 质心 + 倒排 postings），ann 模式生效；增删改正确维护；恢复时从持久化记录重建，重建完成前不输出 READY。
- 不支持：NULL、JOIN、聚合、子查询、ALTER、外键、嵌套事务、主键更新、OR；返回 FAILURE。
- VARCHAR 长度按 Unicode 码点计；字符串按码点序比较；向量 float32，拒绝 NaN/Infinity/维度不符/溢出。
- TopK 在 WHERE 过滤后的集合内计算；距离并列按主键升序；普通 ORDER BY 并列亦按主键升序。

## 存储与恢复设计

- 单文件页式存储（8KB 页）：slotted 堆页存记录，磁盘 B+ 树存主键索引，目录页链表存表定义。
- LRU 缓冲池，内存上限可配（默认 32MB），超内存数据流式扫描，不全量载入。
- 事务：no-steal，活跃事务脏页不写回；COMMIT 先写 WAL（页后像 + 提交记录）并 fsync 后才响应；ROLLBACK 用事务前像恢复。
- 恢复：重放 WAL 中已提交事务的页后像，未提交尾部丢弃；重放幂等，恢复途中被杀可重复恢复；完成后截断 WAL。
- 检查点：正常关闭或 WAL 过大时把已提交页写回数据文件并 fsync 后截断 WAL。

## 操作示例

```sql
CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), content VARCHAR(1024), embedding VECTOR(4));
INSERT INTO docs VALUES (1, 'database', '事务与恢复', '[1, 0, 0, 0]'), (2, 'database', '向量索引', '[0, 1, 0, 0]');
SELECT id, content, L2_DISTANCE(embedding, '[0.9, 0.1, 0, 0]') AS distance FROM docs WHERE category = 'database' ORDER BY distance LIMIT 2;
BEGIN;
UPDATE docs SET embedding = '[0.8, 0.2, 0, 0]' WHERE id = 1;
ROLLBACK;
exit;
```

重启后复用同一数据目录，已提交数据不丢。

## 本地自测

`python3 tests/run_tests.py`（功能、事务、重启、kill -9 恢复、超内存、ann 召回）。
`python3 tests/bench.py ann`（10k×128 维、1000 TopK，对照 exact 真值校验 Recall@10≥0.95，输出耗时与 I/O、峰值内存）。

## 已知限制

- 单进程单客户端、同一时刻一个显式事务（题目约定）。
- 删除不回收堆页空间（空间放大可接受，主键索引与检索正确性不受影响）。
- ann 召回依赖 probes 参数（默认 lists/2），本地实测见测试输出。