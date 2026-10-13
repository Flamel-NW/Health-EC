# T4.1.7 第三轮 detector validation 协议

预注册于T4.1.6，2026-10-12（America/Chicago）；在第三轮 formal fit/validation/test 产生前冻结。依据[第三轮方法](../design/detector-third-round.md)与[第二轮审计](t4_1_5_detector_validation_audit.md)。旧协议与 NO_GO 保留；本轮只做 detector 和只读诊断，F 不反馈 read/migration。

## Splits、输入与执行隔离

正式100 disks / 5000 stripes / 1000000 requests / Zipf1，三个既有realistic2%/overlap4%/stress20%场景；Health-EC migration_disabled、Vanilla EC。10000-request bin、每seed100 bins；bin-end 可用，不换算成 wall-clock。

| Split | Seeds | 用途 |
|---|---|---|
| fit | 7001..7020 | loglinear/constant location，不能使用 GT |
| calibration | 8001..8020 | 重放同伴校正后的独立 residual/common bound |
| validation | 9001..9020 | 全部必需 controls 的 gate 与选参 |
| test | 10001..10050 | 一次 held-out质量/稳健性与分片审计 |
| engineering smoke | 93001/93002/93003 | fit/calibration/check，各100000 reads/500 stripes/1000-request bin；不选参 |
| engineering full smoke | 93011/93012/93013 | 相同接口的正式规模检查，各1000000 reads/5000 stripes/10000-request bin；不选参 |

上述集合互斥，并排除旧1001..1010、2001..2010、42..71、3001..3020、4001..4020、5001..5020、6001..6050及91001..91004。任何已运行正式 test 不再作新锁参 held-out。一个统一候选跨所有场景/策略；每组自己的 location 和 calibration model。模型/同伴不消费 GT、S/D 或 utility。

`smoke → select → test`；T4.1.6只执行smoke。select 不读取/采集 test；test 在build前后验证 selection、全部模型、protocol/campaign、实际依赖源码及binary hash/size。无eligible候选按事前排序锁诊断项、validation_pass=false，held-out仅作 NO_GO诊断。smoke不能产生 selection.json 或冒充 select；非空目录拒绝、零写入。

## 候选、校准与对照

24候选：adaptation `{frozen,peer_location,peer_line}` × calibration `{raw,mirror}` × q `{.99,.999}` × confirmation bins `{2,3}`。fold=disk%5、min peers40、line log-pressure span≥log1.5、count≥20；有限 calibration rank、current健康清除、gap/缺样本清除与 common_change 见方法，事前固定。

必需 controls（validation/test 均使用）：native、coupled、impulse、burst2、shift。前四及shift定义沿用第二轮；burst2只用native fitted/calibrated model，shift只用coupled model，不针对新漂移重新训练。所有变换仅作用于 observation，旧 scores/actions 不随变换重算。锁参 test 另外必需检查 `shift_impulse` / `shift_burst2` 交互，使用coupled model、系数4→6并叠加对应20ms扰动。它们独立检验适配与确认是否一起有效，不能挑选通过的单轴控制。

Baselines：fixed15ms即时与同h连续确认；native另报D12/D2/S即时及D12同h确认；constant family、frozen adaptation、相同 calibration/q/h 的matched constant；loglinear/frozen、同calibration/q/h 的适配消融。所有即时参考完整保留，不把 matched确认后的指标写成原baseline。所有基线也保留warming-up与insufficient，不按GT修改状态机。

输出风险/确认两套 pooled和逐seed metrics、事件/时序、完整启动 FN、去除各真实positive episode前h−1 bins的steady recall（仅辅助）、episode≤h的漏检；所有positive仍进入主recall。partial recovery独立分层。共享 correction/common-change、各折usable peers/pressure span、训练count范围及OOD、校准总数/negative fraction/rank/upper、健康上下侧残差分位和偏斜分别输出。

## 必需 gates 与公平比较

工程/数据：preflight环境适格、Release/CTest/workflow tests、原runner完整CSV回归、schema/full disk×bin/唯一键/finite、schedule与issued read/bytes守恒、全部来源hash/size、split与源版本锁。失败不得授予研究资格。

每scene/policy/必需control的confirmed要求：coverage≥99%、全positive recall≥90%、severe recall≥95%、precision≥95%、healthy FPR≤0.1%、pre-first-onset健康false event episodes=0、onset detection≥95%、confirmed bin-end lag median/p90≤3、fully-healthy clear p90≤1且无uncleared interval、校准rank足够、count OOD≤1% sufficient bins（OOD计入所有指标）。所有缺样本positive计FN，warming-up不删。

第三轮把 confirmed median≤2 调整为≤3，是针对相同两bin前缀可辨性而预先声明的多尺度检测契约，不是第二轮改门槛后重新判PASS。必须同时满足 native/coupled/shift 的suspected风险recall≥95%、severe recall≥95%、onset≥95%、lag median≤1/p90≤2；完整报告风险nuisance FP与short-episode FN。风险不能许可动作，h=3不等于保证快速确认。额外 short-event fixture 对1/2/3/5 bins逐一报告风险/确认与lag，1/2 bins无法确认是能力边界，不能写作无条件fail-slow检测保证。

公平NI：Health-EC native realistic/overlap相对同h D12-confirmed的配对seed confirmed recall差95%lower≥−2pp；stress相对同h D12-confirmed pooled FN减少≥50%且配对lower>0。即时D12差仍报告但不用于不同持续性契约的NI。

coupled每scene/policy相对同h fixed15-confirmed：healthy FPR减少≥80%，配对recall差95%lower≥−2pp；reference FPR=0则没有已展示的压力增益，该gate失败。至少一个coupled组对matched constant减少FP或FN≥50%且另一项不增加，并满足绝对质量。shift每组相对同q/h loglinear/frozen FPR减少≥80%，配对recall差lower≥−2pp；消融无FP则漂移增益gate失败。burst2/shift与test交互是硬性资格，不降为诊断脚注。

Bootstrap单位seed，10000重复、RNG20261012、percentile95%；同seed的所有盘/bin/control不拆分。保留pooled、逐seedmedian/min/max与paired interval，不声明多重比较的统计保证。

选择先过滤全部硬门槛，再按最差组precision降序、最大healthy FPR升序、最差severe recall降序、最大confirmed lag p90升序、candidate ID升序。无eligible用同一排名锁诊断；smoke只评估所有候选，不选择，不创建正式锁。

## 分片效用与因果审计

test另采集50 seeds×三场景×disabled/default被动shard trace及default盘级migration observations。D≤w、阈值/evidence/合法目标/实际动作分区、旧≥5访问eligibility与动作数差、负/零counterfactual、τ15/30 tail变化均保留。新test default的当bin风险/确认只作事后关联；previous-bin风险/确认才作为动作时可用覆盖，F不门控D动作。

past window t排序与next window t+1的single-substitution utility检查：排序不读取未来窗口/GT，预算1/4/16，按past gain降序与shard ID打平；不纳入past gain≤0或缺合法目标。future逐source/shard只有位置一致且完整目标观测时可核对，其余单列unavailable，不记零收益。future收益、τ15/30、reads与动作成本代理分别记录，label仅事后分层。该gain仍是反事实诊断，不是可部署在线score或联合benefit；用first-k反例核验不可加性。

对同伴各折的慢/恢复污染、全体共同延迟与pressure关系改变做独立假设审计；common_change保留，但不将相对detector无事件解释成全体健康。真正layout/BW/decoder/目标竞争及migration I/O对read latency的反馈仍需独立T4.2设计，不能由本轮single-substitution推断。

## 资格、资源与 artifacts

只有selection资格、test全部质量/比较/交互、数据来源及分片因果审计通过，才输出 `ELIGIBLE_FOR_T4.2_DESIGN`，且仅限预注册场景/同伴可比性假设；否则 `NO_GO_DIAGNOSE`。此资格只允许设计下一独立实验，不自动集成或执行T4.2。

WORKFLOW_ROOT入口：`python3 -B scripts/validate_disk_detector_v3.py --mode smoke|select|test`；test必需`--selection-dir ../Health-EC/experiments/results/t4_1_7_detector_selection`。默认新目录为`t4_1_6_detector_method_smoke`、`t4_1_7_detector_selection`、`t4_1_7_detector_test`；全规模工程检查使用`--mode smoke --engineering-scale full`，写独立`t4_1_6_detector_method_smoke_full`。`--workers 1..4`控制独立采集，默认2，不改变指标。select/test模式代码由工程fixture核验，本任务不调用。

保留commands/preflight/build-and-test/run-status/validation/provenance、protocol/campaign/source snapshot、private source/binary ZIP、所有raw traces/schedules、models、风险/确认group/seed/pressure/peer/时序、比较/候选判定、锁参与分片未来/预算表。CSV只对本次新采集文件作已验证无损gzip，保留原文hash/size；旧raw绝不改写。空间下限smoke0.25GiB/select1.5GiB/test3GiB；运行前重新检查，资源不足时停止而不清理历史。输出增大时执行者重新评估预留，阈值不是容量保证。
