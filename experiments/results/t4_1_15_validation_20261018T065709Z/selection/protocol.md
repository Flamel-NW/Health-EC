# T4.1.15 第七轮预注册协议

T4.1.14准备中，2026-10-17（America/Chicago）。以[方法](../design/detector-seventh-round.md)及最终工程readiness封存的实际source/binary/protocol ZIP为准；未完成readiness前不授正式执行资格。本轮不预先沿用任何旧锁。

## 新拆分与候选

三场景realistic_2pct/overlap_4pct/stress_20pct、health_ec（migration disabled）和vanilla_ec；正式100 disks/5000 stripes/100万reads/100×10000-request bins。GT仅验证，F不反馈read/migration。

| split | seeds | 资格 |
|---|---|---|
| fit | 23001..23020 | 独立中心/折外历史尺度 |
| calibration | 24001..24020 | 独立点/整路径校准 |
| validation | 25001..25020 | 全24候选/全部硬组选择 |
| test | 26001..26050 | 一次锁定held-out |
| small工程 | fit101001..101002 / cal101003..101006 / check101007..101008 | 500stripes/10万reads/100×1000；不正式选择 |
| full工程 | fit101011..101012 / cal101013..101016 / check101017..101018 | 正式规模，仅工程 |
| 人工机制 | fit102001..102002 / cal102003..102006 / check102007..102008 | 独立生成；不正式选择 |

所有新splits互斥并排除前六轮全部已见正式/工程/人工seeds。实际执行前枚举旧protocol与artifact seed身份形成排除清单；发现冲突即停止，不静默换seed。正式四split在T4.1.14不采集。

24候选按方法给定itertools顺序；全部15个旧controls保留为硬组，cold_history移除disk0全部fit历史，shift/burst/bridge使用native或coupled预注册参照。完整新机制只作独立诊断，不自动加入候选或删掉困难硬组。

## 保留的绝对与风险门槛

每组coverage≥99%、full recall≥90%、mild steady≥95%、severe≥95%、precision≥95%、healthy FPR≤.1%、pre-first-onset false episodes=0、onset≥95%、lag median/p90≤3、fully-healthy clear p90≤1且无uncleared、全部rank足够、count OOD≤1%。完整mild、full/startup/steady、恢复、1/2/3/5-bin及balanced额外FN均保留；不存在的episode记未测。

risk在native/coupled/shift/skew/heterogeneous/correlated/history_shift/long_memory下recall/severe/onset≥95%、lag median≤1/p90≤2。风险FP和未恢复照常报告，不将suspected用于动作。

## 有效直接参照与不确定性

native Health-EC对D12同h的steady配对seed95%lower≥−2pp；stress要求full FN减少≥50%且steady lower>0。coupled对fixed15同h FPR减少≥80%、steady lower≥−2pp；至少一coupled组相对有效直接raw constant的FP或steady FN减少≥50%，另一不增加且绝对质量通过。shift对有效直接raw frozen FPR减少≥80%、steady lower≥−2pp。

raw constant/frozen直接参照用本轮独立fit/calibration，同q、同3-bin确认；须全部calibration sufficient且coverage≥99%。FPR=0不可声称减少，不能以无效参照授予收益。另列disk_history/contrast_offset/contrast_curve×mirror_path/balanced_path、fixed15 instant/h3、D12 instant/h3与S作为机制参照。

独立seed配对bootstrap10000次，RNG20261017，percentile95%；pooled计数与seed CI并列，不重复计算control/disk/bin为独立样本。工程只有2个check seeds，仅验证统计代码，不授研究收益。

## 选择、诊断与一次test

eligible须所有绝对/比较/稳健性门槛及有效constant收益通过。按最差precision降序、最大FPR升序、最差severe降序、最大lag p90升序、ID排名；锁定唯一优胜者。无eligible时仅锁定预注册诊断集合 **[1,7,9,11]**：offset/current/mirror/.995，offset/history_floor/mirror/.995，offset/history_floor/positive/.995，offset/history_floor/trimmed/.995；不依validation或test改变集合。完整curve消融结果来自validation，不补入test。

selection/models/protocol/source/binary与collection seal核验后，一次性按selection身份认领test；test仅消费原锁，不重选/重拟合/补候选。无eligible仅诊断且保持NO_GO，不降目标、不自动T4.2。50test seeds保留disabled/default shard与fixed-target、previous-bin动作可用关联。

## 生产、执行与恢复契约

使用maintenance API的显式身份/字段/预期集合校验、同字节解析及hash；source必需集合与依赖闭包双向核验，实际运行binary进入ZIP。每条命令成功并schema通过后立即以确切argv、SHA256/size和时间产生只创建一次的fsync回执；完整预期集合复验后封存collection，再封存assessment。

OS文件锁唯一执行者；任务按选择→锁定→一次test→审计→交接组织为一个实际pueue作业，无Codex，无自定义status.json，无自动重试。失败保留原现场。显式暂停只在已持久阶段边界；恢复须新执行记录并复核既存阶段命令/结果身份。未锁select只能明确授权后在新目录消费已生产封存collection重算；selection/models已锁时禁止该分支；已消费未封存test不得自动重跑。

每阶段前空间检查：工程≥4GiB、select≥8GiB、test≥12GiB；workers默认2，最多4。工程为压缩诊断与完整源码保留预留8GiB、select预留3GiB、test含gzip预留5GiB，以本轮smoke实测更新；不足停止由用户处理，不清理历史。来源、生产raw与gzip原文字节、阶段/科学回执及失败均保留。

T4.1.14完成条件：方法与协议、完整维护回归/独立机制/预测因果与模型不变性、小/全规模smoke、全部raw/schema/schedule/receipt/source ZIP/gzip核验、统计与计数逻辑及失败分析、正式splits未消费和可核验工作流交接。工程PASS不等于研究资格。T4.1.15须另获用户执行指令；第五轮缺原生产签名的限制保留。
