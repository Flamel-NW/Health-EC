# T4.1.17 第八轮预注册协议

T4.1.16准备中，2026-10-18（America/Chicago）。以[方法](../design/detector-eighth-round.md)及准备readiness实际封存source/binary/protocol ZIP为准；未完成readiness、未获单独指令不执行正式采集。

## 拆分与范围

三场景realistic_2pct/overlap_4pct/stress_20pct × health_ec（migration disabled）/vanilla_ec；正式100 disks/5000 stripes/100万reads/100×10000-request bins。GT仅验证，F不反馈read/migration。

| split | seeds | 用途 |
|---|---|---|
| fit | 27001..27020 | 独立中心与折外负侧尺度 |
| calibration | 28001..28020 | 点界及100-bin整路径界 |
| validation | 29001..29020 | 全32候选/全部90硬组选择 |
| test | 30001..30050 | 原锁一次held-out test |
| small工程 | fit105001..105002 / cal105003..105006 / check105007..105008 | 500 stripes/10万reads/100×1000 |
| full工程 | fit105011..105012 / cal105013..105016 / check105017..105018 | 正式规模，仅工程 |
| 人工机制 | fit106001..106002 / cal106003..106006 / check106007..106008 | 独立人工输入 |
| first-k/契约 | 106009 / 107001 | 独立契约fixtures |

全部split互斥，运行前枚举前七轮protocol/campaign/artifact及本地历史seed并保留来源与排除清单；冲突即停止，不静默换seed。T4.1.16不采四个formal splits。历史test和本轮工程输入不授held-out资格。

## 候选、参照与诊断

方法规定32候选；15个旧controls全部为硬组，不移除cold_history/shift/bridge/long_memory。cold_history移除disk0全部fit历史，shift消费coupled拟合模型，burst/bridge消费native模型，其余保持原control参照；不按check重新拟合。

全部基线重新使用本轮独立fit/calibration。直接raw constant/frozen、D12与fixed15均保留同h3公平参照；instant另报启动成本；第七轮四算法及第六轮mirror/balanced仅为匹配机制参照。

eligible须全部90组绝对/比较/稳健性gates通过且至少一coupled组有效constant收益通过。按最差precision降序、最大FPR升序、最差severe降序、最大lag p90升序、ID排名，锁唯一优胜者。无eligible仅消费预注册诊断集合 **[1,5,9,13,15]**，分别为offset/current/mirror_min3、offset/current/mirror_capped3、offset/lower_history/mirror_min3、offset/lower_history/mirror_capped3、offset/lower_history/positive_capped3（均q=.995）；不由validation/test修改集合，不补curve或其他候选。

## 完整门槛与统计

原目标保持：每组coverage≥99%、full recall≥90%、mild steady≥95%、severe≥95%、precision≥95%、healthy FPR≤.1%、pre-first-onset false episodes=0、onset≥95%、lag median/p90≤3、fully-healthy clear p90≤1且无uncleared、全部rank足够、count OOD≤1%。不存在的1/2/3/5-bin段明确NOT_OBSERVED，不能记为通过。risk FP与未恢复照常报告。

risk在native/coupled/shift/skew/heterogeneous/correlated/history_shift/long_memory下recall/severe/onset≥95%、lag median≤1/p90≤2。保留完整mild、risk/confirmed、full/startup/steady和确认额外FN，人工短段不替代正式短段资格。

native Health-EC对D12同h steady配对seed95%lower≥−2pp；stress另要求full FN减少≥50%且steady lower>0。coupled对fixed15同h FPR减少≥80%、steady lower≥−2pp；至少一coupled组相对有效直接raw constant的FP或steady FN减少≥50%，另一不增加且绝对质量通过。shift对有效raw frozen FPR减少≥80%、steady lower≥−2pp。直接raw参照须同q、全部calibration sufficient、coverage≥99%；零FPR分母不声称减少。

独立seed配对bootstrap10000次，RNG20261017，percentile95%；pooled计数与seed CI并列，不把control/disk/bin当独立seed。2个工程check seeds仅验证计算管道。

## 执行、封存与恢复

正式阶段按独立fit/calibration→validation→选择/模型锁→一次test→完整审计/交接组织为一个实际pueue作业；T4.1.16只运行工程脚本。无Codex、自动重试、旧test重拟合/重锁/补候选或自定义status.json。

每条生产命令用确切argv、schema、同字节SHA256/size和时间生成一次性fsync回执；显式预期集合复验后先封存collection，再封存assessment。source必需集合及依赖闭包、实际binary/protocol ZIP、raw/gzip、模型、计数独立复算、原锁和唯一执行者OS锁全部核验。

空间不足停止由用户处理，不清理历史；工程≥4GiB/select≥8GiB/test≥12GiB，工程完整诊断/源码预留8GiB、select3GiB/test5GiB，按本轮smoke实测更新资源计划。workers=2，最多4；preflight检查CPU负载/可用内存/版本并记录。

显式暂停只在持久化阶段边界；恢复须新执行记录并复核既存命令/身份。未锁select的封存collection仅在另获恢复指令后在新目录复算；已锁selection/models不得走未锁分支，已消费未封存test不得自动重跑。所有失败保留，运行状态/输出/退出码由pueue管理。

固定目标完整signed负seed、partial/unavailable及重叠原因、previous-bin信号可用时序分别审计；single不证明joint/P99/layout/queueing/I/O或线上收益。工程/数据/来源与研究资格分别报告，前七轮失败及第五轮原生产签名缺口保留，不自动T4.2。
