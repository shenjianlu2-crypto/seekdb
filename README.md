# MiniVectorDB 运行说明与设计记录

2026 OceanBase 数据库大赛赛题一（MiniVectorDB）实现。单机、单进程、单连接，通过标准输入/输出
以 JSONL 协议接收 SQL 并返回结果，不监听网络端口。

## 1. 语言、版本与依赖

- 语言与标准：C++17，`g++ 13.3.0`（Ubuntu 24.04 自带）。
- 依赖：仅 C++ 标准库与 POSIX 文件接口（`<fcntl.h>`、`fdatasync`）。无第三方库，不使用
  SQLite/RocksDB 等现成存储引擎，也不调用 Faiss/VSAG/HNSWlib 等向量索引库；JSON 读写与
  SQL 解析均为本仓库内自实现（`src/json.cpp`、`src/sql.cpp`）。
- 目标平台：Linux x86_64。

## 2. 构建

```bash
make CXX=g++ CXXFLAGS="-O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter"
```

产物为单个可执行文件 `minivectordb`。容器构建：

```bash
docker build -t minivectordb:latest .
```

Dockerfile 在镜像内从源码编译（构建阶段安装 `g++ make libc6-dev`），不预置二进制，
运行期不联网。

## 3. 启动与参数

容器以前台方式直接运行数据库程序，标准输入保持打开：

```bash
docker run --rm -i \
  -v minivectordb-data:/data \
  --cpus=2 --memory=2g \
  minivectordb:latest
```

入口等价于 `ENTRYPOINT ["/app/minivectordb"]` + `CMD ["--data-dir", "/data"]`。

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--data-dir` | `/data` | 数据文件 `data.db`、预写日志 `wal.log` 与排序临时文件的唯一存放目录 |
| `--mode` | `exact` | 进程默认检索模式，随后每条请求里的 `search_mode` 可覆盖 |
| `--cache-mb` | `32` | 缓冲池上限（MiB），超出后按 LRU 逐出脏页 |

根文件系统以 `--read-only` 挂载运行，所有写入只发生在 `/data`。日志与调试信息只写 stderr。

## 4. 通信协议

与 `STDIO_PROTOCOL.md` 一致：

1. 启动完成后立即主动输出一行 `{"type":"ready"}` 并 flush，然后才读取第一条请求；每次重启都会重新输出。
2. 请求逐行读取：`{"id":N,"op":"execute","sql":"...","search_mode":"exact|ann"}`、`{"id":N,"op":"shutdown"}`。
   SQL 内部的分号不会被用作报文边界。
3. 响应逐行返回并 flush：
   - 查询：`{"id":N,"ok":true,"columns":[...],"rows":[[...]]}`
   - 写入：`{"id":N,"ok":true,"affected_rows":N}`
   - DDL 与事务控制：`{"id":N,"ok":true}`
   - 失败：`{"id":N,"ok":false,"error":{"code":"<非空字符串>","message":"..."}}`
4. `shutdown` 先回滚未提交事务，再回 `{"id":N,"ok":true}`，flush 后以退出码 0 结束，之后不再向 stdout 写任何字节。
5. 类型保真：BIGINT 输出 JSON 整数（超过 2^53 的值保持精确），字符串为 JSON 字符串，向量为一维数组，
   距离为带小数点的 JSON 浮点数。

## 5. 支持的 SQL 范围

| 类别 | 支持 |
| --- | --- |
| 建表 | `CREATE TABLE t (col BIGINT PRIMARY KEY, col VARCHAR(n), col VECTOR(n))`，一个 BIGINT 主键，每表至多一个向量列 |
| 写入 | `INSERT`（含多行 `VALUES`）、`UPDATE`、`DELETE`，可省略列名按位置写入 |
| 查询 | 列投影、`SELECT *`、`WHERE` 比较运算 `= < <= > >=` 与 `AND` 连接、`ORDER BY`（可多列、`ASC/DESC`、可按 `L2_DISTANCE` 的别名）、`LIMIT` |
| 向量 | `L2_DISTANCE(col, '[...]')` 返回欧氏距离（非平方），向量按 float32 存储；支持先过滤再取 TopK，距离并列按主键升序 |
| 索引 | `CREATE VECTOR INDEX index_name ON table_name(vector_column)`；自研 IVF-Flat，增删改后索引保持一致 |
| 事务 | `BEGIN` / `COMMIT` / `ROLLBACK`，事务内可读到自己的修改，普通查询与向量检索同时可见 |
| 检索模式 | `search_mode` 显式区分 `exact` 与 `ann`；即使已建近似索引，`exact` 仍返回精确结果 |

按题面约定不支持、并且一律返回错误而不静默改写的构造：`NULL` 与 `IS NULL`、`JOIN`、聚合函数与
`GROUP BY`、子查询、`ALTER TABLE`、外键、嵌套事务、主键更新；以及 `OR`、`!=`/`<>`、`IN`、
`BETWEEN`、`LIKE`、`DISTINCT`、`OFFSET`、`DROP`、`TRUNCATE`、`SHOW`、算术表达式。

### 错误码

公开题包只冻结了 `duplicate_key` 与 `unsupported_sql` 两个语义名，其余映射尚未发布；实现按
"`error.code` 必须是与语义同名、非空字符串"处理，取值如下。

| code | 触发条件 |
| --- | --- |
| `syntax_error` | 无法解析的语句 |
| `unsupported_sql` | 语法可识别但题目不支持（`ALTER TABLE`、主键更新、`OR` 等） |
| `duplicate_key` | 主键重复 |
| `dimension_mismatch` | 向量维度与列定义或查询向量不一致 |
| `invalid_vector` | 向量字面量非法：非数值元素、NaN、Infinity、float32 溢出、括号缺失 |
| `type_mismatch` | 值类型与列类型不符（如向 BIGINT 列写入字符串） |
| `value_too_long` | VARCHAR 超过按 Unicode 字符数计的长度 |
| `table_not_found` / `column_not_found` / `row_not_found` | 对象不存在 |
| `table_exists` | 重复建表 |
| `transaction_error` | 事务状态非法（如已处于事务中再次 `BEGIN`） |
| `internal_error` | 兜底：非预期异常，同时把详情写 stderr |

## 6. 从空数据目录开始的操作示例

以下每一行是一个 JSONL 报文，`<` 表示发往 stdin，`>` 表示从 stdout 读回。

```text
启动：docker run --rm -i -v mvd-data:/data --cpus=2 --memory=2g minivectordb:latest
> {"type":"ready"}

建表
< {"id":1,"op":"execute","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), content VARCHAR(1024), embedding VECTOR(4));","search_mode":"exact"}
> {"id":1,"ok":true}

多行写入（自动提交）
< {"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (1,'database','事务与恢复','[1, 0, 0, 0]'),(2,'database','向量索引','[0, 1, 0, 0]'),(3,'travel','旅行攻略','[0, 0, 1, 0]');","search_mode":"exact"}
> {"id":2,"ok":true,"affected_rows":3}

普通查询
< {"id":3,"op":"execute","sql":"SELECT id, content FROM docs WHERE category = 'database' ORDER BY id;","search_mode":"exact"}
> {"id":3,"ok":true,"columns":["id","content"],"rows":[[1,"事务与恢复"],[2,"向量索引"]]}

向量 TopK（先过滤后取 K，距离为欧氏距离）
< {"id":4,"op":"execute","sql":"SELECT id, L2_DISTANCE(embedding, '[0.9, 0.1, 0, 0]') AS distance FROM docs WHERE category = 'database' ORDER BY distance LIMIT 2;","search_mode":"exact"}
> {"id":4,"ok":true,"columns":["id","distance"],"rows":[[1,0.141421374149721],[2,1.272792188223375]]}

建近似索引并切换 ann 模式
< {"id":5,"op":"execute","sql":"CREATE VECTOR INDEX vidx ON docs (embedding);","search_mode":"exact"}
> {"id":5,"ok":true}
< {"id":6,"op":"execute","sql":"SELECT id, L2_DISTANCE(embedding, '[0.9, 0.1, 0, 0]') AS distance FROM docs ORDER BY distance LIMIT 2;","search_mode":"ann"}
> {"id":6,"ok":true,"columns":["id","distance"],"rows":[[1,0.141421374149721],[2,1.272792188223375]]}

事务：更新与删除后回滚，内容、可见集合与向量检索同时回到事务前状态
< {"id":7,"op":"execute","sql":"BEGIN;","search_mode":"exact"}
> {"id":7,"ok":true}
< {"id":8,"op":"execute","sql":"UPDATE docs SET embedding = '[0.8, 0.2, 0, 0]' WHERE id = 1;","search_mode":"exact"}
> {"id":8,"ok":true,"affected_rows":1}
< {"id":9,"op":"execute","sql":"DELETE FROM docs WHERE id = 2;","search_mode":"exact"}
> {"id":9,"ok":true,"affected_rows":1}
< {"id":10,"op":"execute","sql":"ROLLBACK;","search_mode":"exact"}
> {"id":10,"ok":true}

提交后重启仍然可见（`--restart` 由驱动用相同数据目录重启进程完成）
< {"id":11,"op":"execute","sql":"BEGIN;","search_mode":"exact"}
> {"id":11,"ok":true}
< {"id":12,"op":"execute","sql":"INSERT INTO docs VALUES (4,'database','持久化','[1, 1, 0, 0]');","search_mode":"exact"}
> {"id":12,"ok":true,"affected_rows":1}
< {"id":13,"op":"execute","sql":"COMMIT;","search_mode":"exact"}
> {"id":13,"ok":true}
< {"id":14,"op":"shutdown"}
> {"id":14,"ok":true}
（进程退出码 0；用同一 `/data` 重新启动后先输出 `{"type":"ready"}`，`SELECT id FROM docs ORDER BY id` 返回含主键 4 的四条记录）
```

## 7. 设计与实现要点

### 7.1 存储布局

- `data.db` 为 8 KiB 定长页的堆文件，页头含页类型与槽位目录；记录按槽位追加，删除只清槽位标记，
  空闲空间可被后续插入复用。每表维护堆页链表（`heap_hint`）与槽位计数。
- 主键索引是磁盘上的 B+ 树：内部节点存键与子页号，叶子节点存键与 `(页号, 槽位)` 定位符，
  叶节点串联便于范围遍历；单节点上限 500 个键，分裂与合并都按页写日志。
- 表目录（列定义、堆首/末尾页、B+ 树根页、索引状态）序列化在目录页中，启动时随恢复一起重建。

### 7.2 缓冲池

固定上限的 LRU 缓冲池（默认 32 MiB），按需换页；脏页在写日志时被记为"本语句改写"，
失败时回滚为语句开始时的旧镜像。数据量超过缓冲池不会失败：扫描、排序、恢复都只按页推进，
因此内存占用与数据规模无关。

### 7.3 WAL 与恢复

- 采用 no-steal 事务模型：事务内的页修改先在内存中缓冲，提交时把被改页的**后像**连同一条
  `COMMIT` 记录一次性追加到 `wal.log`，`fdatasync` 成功后才向客户端发送提交响应——
  即"确认已提交"不依赖正常关闭。
- 恢复只重做（redo-only）：按序扫描日志，已见 `COMMIT` 的记录把后像写回 `data.db`；
  没有 `COMMIT` 的部分（进程被强杀、或自动提交语句写到一半）整条丢弃。因此
  **强杀已提交不丢、未提交不部分生效、发出提交请求但未收到响应的允许整体生效或整体不生效**。
- 日志尾部可能因写入过程被强杀而出现残缺记录：读日志按长度前缀校验，遇到不完整记录即停止，
  故"恢复途中再次被终止"后重启仍能收敛到同一状态。
- 恢复完成后（含索引重建）才输出 `{"type":"ready"}`，重建成本计入启动时间。

### 7.4 语句原子性

页级 undo 只能恢复磁盘镜像，表目录还在内存里，因此每个原子单元同时快照内存目录：
自动提交语句失败 → 回滚语句页改动 + 恢复目录快照 + 重建索引；显式事务内语句失败 → 只撤销该语句，
之前成功的语句仍可提交或回滚；`ROLLBACK` → 回到 `BEGIN` 时的目录快照。索引（IVF 倒排）在这些路径上
一并重建，避免失败语句的行残留在检索结果里。

### 7.5 向量检索

- 存储：`VECTOR(n)` 定长 float32，写入前逐元素检查有限性、float32 范围与维度，非法值拒绝。
- 距离：`L2_DISTANCE` 先按存储后的 float32 值计算平方和再开方，返回欧氏距离；查询侧维度不符报错。
- `exact`：全表扫描 + 保留 K 条的最小堆，比较键为 `(距离, 主键升序)`；带过滤时先在满足 `WHERE` 的
  集合内取 TopK。
- `ann`：自研 IVF-Flat。`k = clamp(round(sqrt(N)), 16, 256)` 个聚类中心，25 轮 k-means 初始化，
  倒排链表存 `(主键, 原始 float32 向量)`；查询探测中心数 `probes = max(1, ceil(0.9k))`，
  候选按 `(距离, 主键)` 归并。探测比例取 0.9 是为了在未见过的随机种子上稳定满足
  无过滤与有过滤两组平均 Recall@10 ≥ 0.95（实测 0.997，见 §7.7）。
- 索引维护：`CREATE VECTOR INDEX` 从记录重建；插入/更新/删除同步维护倒排链与中心归属，
  更新向量时先摘旧再插新，删除立即从结果集消失；重启后索引按持久化记录重建。

### 7.6 排序与执行

- `ORDER BY` 带 `LIMIT` 时走堆选择，内存 O(K)；无 `LIMIT` 的大结果集走外部归并排序
  （按行分块写入 `/data` 下临时文件再归并），排序键并列时统一追加主键升序。
- 字符串比较按 Unicode 码点顺序，`VARCHAR(n)` 长度按 Unicode 字符数计算。

### 7.7 验证记录（本地）

以下均在 WSL Ubuntu 24.04 + Docker 上，用组织方随题包发布的 `protocol_check/run.py` 驱动执行。

| 项目 | 结果 |
| --- | --- |
| 握手检查（镜像模式，只读根文件系统、`--cap-drop=ALL`、`--network=none`、`--cpus=2`、`--memory=2g`、卷挂载 `/data`） | `handshake: PASS` |
| 公开 20 个用例 / 95 条 SQL，`--command` 模式 | 20/20 PASS，`result.json` `passed: true` |
| 公开 20 个用例，`--image` Docker 模式 | 20/20 PASS，`passed: true` |
| 强制终止与重复恢复 | 已提交 200 行 SIGKILL 后保留；未提交行不出现；单条 8000 行插入被强杀后全有或全无；恢复途中反复强杀后行数一致、写入与向量检索正常 |
| 关闭语义 | `shutdown` 响应后退出码 0，未提交事务被回滚 |
| 非法输入 | NaN / Infinity / -Inf / float32 溢出 / 维度不符 / 空向量 / 非数值元素 / 向向量列写整数一律 `ok:false` 且不留部分数据 |
| 语句原子性 | 多行插入中间一行主键重复或维度不符 → 整条语句 0 行生效；显式事务内失败语句可继续 `COMMIT` 或 `ROLLBACK` |
| 超出内存限额 | `--memory=512m` 下装载并查询远大于限额的数据（含全表外部归并排序、精确 TopK、建 IVF 索引、强杀后恢复），无 OOM，`ann` 与 `exact` 结果一致 |
| 性能与召回 | 见 §8 |

## 8. 性能与召回实测

测量口径按题面 3.3 节：10,000 条记录、128 维向量、每批 100 条写入并提交、1,000 条 K=10 向量 TopK 查询，
其中一半带普通列等值过滤；容器参数 `--cpus=2 --memory=2g --memory-swap=2g --read-only --cap-drop=ALL --network=none`，
数据在命名卷挂载的 `/data`。T 为平台侧墙钟，B 取该容器 cgroup `io.stat` 的 `rbytes+wbytes` 增量，
M 取 `memory.peak`（分阶段清零后重测）。

**提交配置固定使用 `exact`。** 同一进程内先跑完 1,000 条 `exact` 取真值、紧接着跑 1,000 条 `ann`
的配对实测（消除本机其他容器的负载差异）：

| 指标 | exact | ann |
| --- | ---: | ---: |
| 批量写入 T | 2.15 s | 1.49 s |
| 索引准备 T | 不需要 | 1.97 s（另计） |
| 1,000 条查询 T | 13.78 s | 24.14 s |
| 平均单查询 | 13.78 ms | 24.14 ms |
| Recall@10（无过滤） | 1.0000（定义即真值） | 0.9972 |
| Recall@10（有过滤） | 1.0000（定义即真值） | 0.9970 |
| 全部查询合计 Recall@10 | — | 0.9971 |

两组 `ann` 召回均高于题面要求的 0.95，重复 ID 数为 0；但在 10,000×128 这个规模下整表只有约 5 MB，
精确扫描本身就是顺序读的带宽型工作，`ann` 反而要多付建索引与遍历倒排链的开销，因此不选它。

另一次以 cgroup 采样为准的运行（同机负载较重，绝对值偏大，仅用于取 B、M）：

| 阶段 | T | B（磁盘 I/O） | M（峰值内存） |
| --- | ---: | ---: | ---: |
| 建表 + 100 批写入并提交 | 4.40 s | 8.3 MiB | 22.6 MiB |
| 1,000 条 TopK 查询 | 15.54 s | 0.0 MiB | 22.6 MiB |

查询阶段 B=0 说明数据与索引在装载阶段已进入页缓存/缓冲池，测量窗口内不再产生物理读；
写入阶段 8.3 MiB 包含数据页、WAL 与 `fsync`。整个容器峰值内存 22.6 MiB，远低于 2 GiB 限额。

超出内存限额的场景单独实测（题面 2.3 要求不能依赖全量载入内存）：`--memory=512m` 下装载
150,000 行（每行 600 字节 `VARCHAR` + `VECTOR(16)`，`/data` 实际占用 209 MB，为限额的 41%），
耗时 79.1 s，容器内存读数 42 MiB / 512 MiB；随后主键点查、`ORDER BY id DESC`、对非主键列做
150,000 行全量外部归并排序、精确 TopK、建 IVF 索引、`ann` 与 `exact` 结果比对、强制终止后重启恢复
共 8 项断言全部通过，未被 OOM 杀死。

> 本机在测量期间同时运行着其他容器，同一负载两次实测的墙钟耗时相差约 2 倍，
> 因此表中 T 只用于同机相对比较，不用于预测平台名次；正式数字以平台三轮测量的中位数为准。

## 9. 已知限制

- 单机单连接，无并发控制；同一时刻至多一个显式事务，不支持嵌套事务。
- 只实现题面子集：`NULL`、`JOIN`、聚合、子查询、`ALTER TABLE`、外键、主键更新按约定返回错误；
  `OR`、`!=`、`IN`、`BETWEEN`、`LIKE`、`OFFSET`、`DROP`、`TRUNCATE` 等未列入必须支持范围的语法同样返回错误。
- 删除记录只标记槽位，不主动收缩堆页，不做文件级压缩；`data.db` 会保留回收空间供后续插入复用。
- 错误码字符串在公开配置中未冻结，实现按语义同名返回；若正式映射另有规定，只需替换 `src/common.h`
  中的常量表，不影响引擎行为。
- `ann` 使用 IVF-Flat（暴力扫描倒排链，非图索引），召回优先：探测比例固定 0.9，
  在极小表上退化为全量扫描，因此结果与 `exact` 一致。
- 性能数据为本地 WSL + Docker 实测，平台硬件与缓存策略可能不同；未使用任何第三方向量索引库。
