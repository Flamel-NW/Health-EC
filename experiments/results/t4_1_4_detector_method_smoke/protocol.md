# T4.1.5 第二轮 detector validation 协议

预注册：2026-10-12，T4.1.4。在第二轮 validation/test 产生前固定；原[首轮协议](t4_1_3_detector_validation_protocol.md)不变。[方法](../design/detector-second-round.md)承接首轮机制诊断。仅 disk detector、passive diagnostics，无 F→read/migration 集成。

## 采集与隔离

正式 framework 保持 100 disks / 5000 stripes / 1000000 requests / Zipf=1，realistic2%、overlap4%、stress20%；Health-EC migration disabled 和 Vanilla EC 分别观测。bin=10000 requests，100 bins/run。request-index bin 结束后可用，不解释成 wall-clock 秒。

| Split | Seeds | 用途 |
|---|---|---|
| fit | 3001..3020 | location 模型 |
| calibration | 4001..4020 | 独立 upper bound，不清洗 |
| validation | 5001..5020 | gate、选择；不再修改候选 |
| test | 6001..6050 | 一次 held-out 审计 |
| engineering smoke | 91001..91003 | fit/calibration/check 各一个；100000 reads、500 stripes、1000-request bin，仅接口/算例检查 |

新集合互斥，且不同于首轮2001..2010、1001..1010、42..71；smoke 不用于选择，不消耗 formal test。每个 scene/policy/control 独立拟合，上述 seeds 是重复单位；control 与 policy 在同 seed 上配对。GT 不用于模型或候选决策，只用于独立评估。

执行按 `smoke → select → test` 分阶段；select 不采集/读取 test，test 必须验证 selection、models、campaign、protocol 和源码的 SHA256/size，且候选已锁定。无合格候选仍锁诊断项，test 只能作为预先声明的 NO_GO 诊断。test 后变更须新协议、新 seeds、新结果目录。不得编辑当前 campaign 重新选择。

## 候选与对照

24项：family `{constant,loglinear}` × calibration `{raw,mirror}` × quantile `{.99,.999,.9999}` × event `{guard,consecutive}`。event/risk、pressure 分桶拟合、rank 不足与 OOD 语义见方法。一个统一候选用于全部 scene/policy/control；每组自己的 fitted model。native/coupled/impulse 是必需组，burst2/shift 是锁参后的稳健性诊断组。

Baselines：fixed mean15ms、相同 calibration/q/event 的 constant、同上界下首轮三 bin/2-of-3历史 event。native 另报 S>0.03、D>12、D>2。D2 只改盘级评估阈值，不冒称 alpha_D=0.10 策略。不能把 semi-synthetic control 的旧 D 当公平 baseline。

上界输出 calibration total/lower counts、lower fraction、rank、bounds、拟合清洗数和 pressure range。validation/test 事后测 healthy exceedance rate，按训练压力四分位分层，bootstrap seed variability；mirror 另报 upper/lower residual tail 不对称。q 不是误报承诺，不使用 bin 独立性置信区间。

## 固定 gates

硬性资格：preflight 适格、Release build/CTest/workflow tests、原 runner 输出回归、schema/唯一键/full disk×bin coverage、finite、schedule核对、issued-read/bytes守恒、所有实际输入来源 hash、split 隔离、非空目录拒绝复用。缺项不能进入研究 gate。

native/coupled/impulse 的每个 scene/policy 组均须：

1. sufficient coverage≥99%、positive recall≥90%、severe recall≥95%、precision≥95%、healthy disk-bin FPR≤0.1%。insufficient positive计FN；warming-up保留，不隐藏。
2. 首次 slowdown 之前 false event episode=0；始终健康盘全部窗口同样计入。
3. onset detection rate≥95%，bin-end lag median≤2、p90≤3；未检测 episode 是失败，不能只报成功 episode。
4. fully-healthy clear p90≤1 bin，未 clear单列 failure；partial recovery 单列 operational precision/event rate，不当 fully healthy，也不当迁移正例。
5. calibration rank 足够、OOD 不超过 sufficient 样本的1%；OOD仍保留输出并计入 recall/FP，不能静默删除。

替换首轮不可达的 +10pp gate：仅主 Health-EC native，realistic/overlap 的配对 seed recall差 F−D12，其95% bootstrap lower bound≥−0.02；stress pooled FN 至少减少50%，且配对 recall差95% lower bound>0。三场景仍同时满足绝对 gate，不因 baseline 弱而放宽。

耦合场景需证明问题相关性：每 scene/policy 对 fixed15ms 的 healthy FPR 降低≥80%，并且配对 recall差95% lower bound≥−0.02。若 fixed15ms FPR=0，没有已展示的压力归一化增益，本 gate 不通过；不据此包装 LvT 价值。与 matched constant 报告 paired recall/FPR，至少一个耦合组需在 absolute-quality 合格的同时减少 FN 或 FP≥50%且另一项不变差。

Bootstrap 单位 seed；10000次、RNG seed=20261012，percentile95% interval。同 seed 所有 disks/bins/controls保持同一重复，不 bootstrap bins。20个validation/50个test seeds；每组报告 pooled、逐seed、median/min/max及paired interval。不宣称多重比较后的统计保证；只验证一个已锁候选。

选择：先过滤全部 gates，再按最差组 precision降序、最大 healthy FPR升序、最差 severe recall降序、最大 onset p90升序，最终候选ID打破平局。无合格项用同一排名锁诊断项并写 `validation_pass=false`，不改门槛。烟测不作正式选择。

## 分片诊断与预算

锁参后按 test seeds 另采集 Health-EC disabled/default 的 `shard_diagnostic`，对 native 保留 D上限、proactive evidence、越门槛、合法目标和冻结策略的一次读 utility。旧 migration-positive≥5 accesses和收益加权口径分别报告，不冒称新的 utility 口径是旧 recall。

按热度桶、disk/event、状态和 τ=15/30 分层：不可达（w≤12）、可达但未过、过门槛但无更健康合法目标、候选数量与实际动作；default无top-k budget，所以不设置虚构预算阻塞。反事实无目标/负收益/零收益单列。离线按 per-window utility 排序画预算1/4/16曲线，只是诊断上界；不向控制路径反馈。相同 window 的 F 是事后关联，因果关联用前一 bin；当前输出为 schedule-window聚合，若不能重建bin对应，明确 unavailable，不能当可用信号。

burst2/shift 在 candidate 锁定后固定评估；如出现precision<95%或FPR>0.1%，标 `ROBUSTNESS_GAP`，不能自动 PASS_TO_T4.2；先确认机制及下一轮外部场景/适配方法。不能将其降为脚注后继续部署。

工程与研究状态分离。只有 validation/test 全部必需 gates通过、关联资格完整、稳健性缺口已处理且分片诊断解释可核对，才给出 `ELIGIBLE_FOR_T4.2_DESIGN`；这只允许制定独立 D-only 投影实验，不宣称迁移收益或执行集成。其余 `NO_GO_DIAGNOSE`，保留原失败，下一轮改进方法与证据。

## 入口与 artifacts

WORKFLOW_ROOT：

```bash
PYTHONDONTWRITEBYTECODE=1 python3 scripts/validate_disk_detector_v2.py --mode smoke
PYTHONDONTWRITEBYTECODE=1 python3 scripts/validate_disk_detector_v2.py --mode select
PYTHONDONTWRITEBYTECODE=1 python3 scripts/validate_disk_detector_v2.py --mode test --selection-dir ../Health-EC/experiments/results/t4_1_5_detector_selection
```

默认目录分别 `t4_1_4_detector_method_smoke`、`t4_1_5_detector_selection`、`t4_1_5_detector_test`；拒绝非空目录。T4.1.4只执行smoke，不启动正式select/test。

保留protocol/campaign/source snapshots、commands/preflight/build-and-test/run-status/validation/provenance；raw disk trace与schedule、全部location/calibration models、group/seed/pressure metrics、selection及test episodes/timing；native shard diagnostics/derived partitions/budget curves。控制观测由原始hash和变换版本可复现，不重复复制全量 CSV。选择阶段只登记 fit/calibration/validation 输入；测试阶段只能消费已验证的锁定文件。
