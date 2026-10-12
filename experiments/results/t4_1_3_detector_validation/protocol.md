# T4.1.3 离线 Detector Validation Protocol

制定日期：2026-10-12。首次结果产生前固定以下拆分、候选和门槛；修改须另开实验，保留失败结果。

## 目的与边界

验证 [mapping](../design/perseus-health-ec-mapping.md) 的 read-side `F_d(t)`，只新增观测输出和离线 detector。控制逻辑、S/D 更新和迁移动作不变。检测输入仅含盘 ID、窗口、实际 issued read count 和完整模拟读延迟均值；schedule、bypass eligibility、S/D 与迁移只进入评估。

现有 T3 没有 per-disk latency trace，不能从 stripe P99 或 migration trace 反推。新采集固定 100 disks / 5000 stripes / 1000000 reads / Zipf 1.0，场景为 realistic 2%、overlap 4%、stress 20%。主观测为 Health-EC `migration_disabled`（保留 read bypass，避免迁移消除观测）；Vanilla EC 为独立的 policy sensitivity 对照。每个实际 issued data/parity read 计一次，未 issued parity 不计。模拟中的完整延迟可见，尚未模拟取消读的 censoring。

模拟器没有 queueing 或 latency-pressure 耦合。因此本实验能验证盘级识别和风险时序，不能验证生产 LvT workload normalization；常数模型必须与 LvT 模型直接比较。kernel 已从历史 7.0.0-1005 升为 7.0.0-1013，本 campaign 独立记录环境并重新构建、CTest、static 回归，不将新数据混入旧 T3 表。

## 窗口、拆分与模型

- Detector observation bin = 10000 stripe requests，100 bins/run；schedule 仍为原 20 个窗口，每个对应 5 bins。按 request index 计时，无 wall-clock 秒含义。decision 在 bin 完成后可用。
- 完整 peer pool 为同 scene、同 observation policy；训练/校准不能跨 scene 或 policy 混合。训练 seeds `2001..2010`，验证 `1001..1010`，测试 `42..71`，集合互斥。测试只在候选锁定后评估。
- 训练使用所有训练期样本，不通过 schedule 挑选健康盘或健康窗口。清洗：对 latency mean 使用 median + 3 * scaled MAD 单侧剔除，再拟合；清洗只服务回归，不直接产生检测标签。保留清洗数、模型系数和 pressure 范围。
- 两类 normal model：robust constant、OLS `a+b*log(read_count)`。upper bound 为 `max(0.001, expected + q(residual*sqrt(count))/sqrt(count))`，quantile 使用 nearest-rank。样本不足 `read_count<20` 标 insufficient。
- 候选网格：model `{constant,loglinear}` × quantile `{0.95,0.99,0.999}` × persistence bins `{3,5}` × rho `{0.6,0.8}`，共24项。`SR=L/UB`，`F=persistence*median(max(0,SR-1))`，event 按 mapping 同时检查 persistence 和 median SR。
- 需要连续、完整且足样本的 persistence 窗口；开头或缺测不填零、不压缩时间。pressure 超出训练范围单列 OOD 但保留预测，不将其静默删除。
- 同一候选配置用于所有 scene/policy，各组分别拟合。验证选择：先检查下方全部组门槛，合格者按最低组 recall 降序、healthy FP 总数升序、onset lag p90 升序、优先 constant、较短 persistence、较高 quantile 排序。无合格者仍锁定诊断最优项，明确 NO_GO，不在测试集调参。

## 指标与进入 T4.2 门槛

全部 confusion counts 以 disk-bin 为单位，mild/severe 为正；recovery 为部分退化独立分层（其 profile 均值13.5ms高于healthy9ms），不当作完全健康 FP。另给出将 recovery 计负时的 operational precision。insufficient 同时报告，正例 insufficient 计入 FN；healthy FPR 分母包含全部 healthy bins。

每 scene/policy 同时报告 pooled counts、per-seed 指标和跨 seed median / min / max；窗口不是独立重复，不使用 bin 级独立性假设的置信区间。

硬性数据门槛：build/CTest/static regression、schema、唯一键、完整盘×bin覆盖、count conservation、有限值、schedule 核对与 SHA256/size provenance 均 PASS。

验证及测试各组研究门槛：

1. coverage ≥99%，positive recall ≥90%，severe recall ≥95%，precision（排除recovery）≥95%。
2. healthy disk-bin FPR ≤0.1%；每盘首次 slowdown 之前 false event episode 数为0（始终健康盘也包含）。
3. 每个连续 mild/severe episode 的 onset detection rate ≥95%，bin-end lag median ≤2 bins、p90 ≤3 bins（onset bin 的检测 lag=1）。
4. 完全转 healthy 后，event 清除的 p90 ≤3 bins；恢复期 event rate 单列，不要求 partial recovery 立即清除。
5. 主观测的 F event recall 比同 trace 的 `max_D>12` 盘级基线至少提高10个百分点。S/D聚合只是识别比较，不是迁移收益证明。

通过表示值得开展独立 T4.2 D-only integration；不执行 T4.2。失败用于机制诊断和新一轮验证，不降低门槛。

## 比较与解释

- Baselines：window mean >15ms（fixed threshold）、同样 persistence 的 robust constant、盘上任意 shard 的 bin-end `S>0.03`、`D>12`、`D>2`。D>2 仅作相同 alpha_D 下的诊断阈值，不冒充 T3.6 alpha_D=0.10 策略。
- 保留实际 observable parity-win、proactive reads、S/D above-threshold counts、max S/D；按 disk-bin 报告 F 与 S/D、parity-win overlap。bypass eligible 使用已有 ground truth validation 口径，字段单独标明，仅用于关联。
- 事件质量：连续 predicted episode 的 start/end、duration、median SR/F、healthy/recovery/slow bins；onset lag、FN episodes、恢复清除延迟分别输出。
- 候选锁定后另采集默认 Health-EC migration 的30个测试seeds（共90个trace），只作关联和观测反馈诊断；使用同scene的 migration_disabled 训练模型，不再调参。报告实际迁移 source disk 的同bin F（事后关联）及前一bin F（当时可用信号），并分开 TP/FP、bypass、迁移数和延迟。此项不加入选择gate，也不证明迁移收益。旧T3 migration trace无逐文件hash记录，不能直接作为本轮适格来源。
- 零 FP 仅为本场景观测；20% stress、同分布seed测试与固定placement不代表生产外部有效性。

## Artifacts 与复现

PROJECT_ROOT：`experiments/results/t4_1_3_detector_validation/`（默认，拒绝非空目录）。WORKFLOW_ROOT：`python3 scripts/validate_disk_detector.py`。

保留 protocol snapshot、源码/工作区 diff、commands、preflight与适格判断、build-and-test、static回归、schedule traces、分组 raw disk traces、models、candidate validation metrics、测试 detector traces、seed/group summaries、event/timing/association tables、selection、validation、run-status及provenance。失败目录保留；追加运行必须使用新目录。

主采集300个trace/3000000行；默认迁移诊断90个trace/900000行。训练/验证/测试主trace在选择前可落盘，但离线分析严格按拆分读取，测试数据不参与模型、边界或候选选择。

专项代码验收覆盖：issued-read conservation、观测输出不改变模拟结果、非法 bin组合、无GT模型输入、持续窗口缺测、episode边界、来源篡改与非空目录保护。
