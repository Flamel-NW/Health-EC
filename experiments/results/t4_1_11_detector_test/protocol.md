# T4.1.11 第五轮预注册协议

T4.1.10准备，2026-10-14（America/Chicago）。[方法](../design/detector-fifth-round.md)为语义来源。本文件在工程验证期间可以修订；最终readiness锁与源码ZIP才构成formal前冻结。T4.1.10不采formal数据。

## 拆分、控制与候选

100 disks、5000 stripes、100万reads、10000-request bin、100bins，三场景和Health-EC disabled/Vanilla沿用第四轮。

| split | seeds | 资格 |
|---|---|---|
| fit | 15001..15020 | 未采集，label-blind历史 |
| calibration | 16001..16020 | 未采集，独立点/路径校准 |
| validation | 17001..17020 | 未采集，全部gate选参 |
| test | 18001..18050 | 未采集，一次锁参held-out |
| small工程 | 97001 fit，97002..97005 cal，97006 check | 10万reads/500stripes/1000-request bin |
| full工程 | 97011 fit，97012..97015 cal，97016 check | 正式规模，仅工程 |
| 独立逻辑 | 97021..97024 | 人工数据，不读旧test |

排除此前全部正式/工程seed；工程前缀不能混入formal。24候选按pooled/disk_history×raw/mirror/mirror_path×q(.99,.995)×strength(0,1)固定itertools顺序，h=3。12个第四轮控制全部保留；新增history_shift（异构盘型在中点改变）、cold_history（fit移除disk0的全部历史、check仍完整）、long_memory（rho.95健康噪声）作为硬稳健性组；cold_history预期暴露不足，不能通过静默获得资格。控制不读取GT，变换所有状态。继承控制定义以v4冻结代码为准，新增变换以v5代码为准。

## Gates与比较

每scene/policy/control确认：coverage≥99%、full recall≥90%、mild steady≥95%、severe≥95%、precision≥95%、healthy FPR≤0.1%、pre-first-onset健康false episodes=0、onset≥95%、lag median/p90≤3、fully-healthy clear p90≤1且无uncleared、rank全部足够、count OOD≤1%。新增mild steady gate明确强化轻度识别；完整mild含不可避免启动FN，不能对短relapse设置超过h3信息上限的目标；不删除原full/startup损失。风险native/coupled/shift/skew/heterogeneous/correlated/history_shift/long_memory：recall/severe/onset≥95%、lag median≤1/p90≤2；风险FP/精度完整输出。

Health-EC native对D12同h steady差的配对seed95%lower≥−2pp；stress另要求full FN减少≥50%且steady lower>0。coupled对fixed15同h FPR减少≥80%、steady lower≥−2pp；至少一coupled组对有效直接constant FP或steady FN减少≥50%、另一不增加且本候选绝对质量通过。shift对有效直接frozen FPR减少≥80%、steady lower≥−2pp。参照必须模型全部sufficient且coverage≥99%；参照FPR=0不证明减少，比较gate失败。输出参照可用性及原因，不借无效参照授予收益。

bootstrap以独立seed为单位，10000次、RNG20261014、percentile95%；跨disk/bin/control不作独立样本，路径校准rank不赋予独立性或分布无关保证。全部seed/pooled/full/steady/mild/severe/短episode与控制失败保留。

## 选择、诊断及一次test

eligible必须全部组硬gate与有效constant改进通过；排名沿用最差precision降序、最大FPR升序、最差severe降序、最大lag p90升序、ID。无eligible时预注册一次诊断集合**2/18/22**：pooled/raw/.995/0、disk_history/mirror/.995/0、disk_history/mirror_path/.995/0；分别检验污染/盘条件/路径校准。不是沿用第四轮锁，不成为部署默认。test只消费eligible单项或此集合，不在test换参、补候选或拟合。

select只访问fit/calibration/validation，test只访问test与冻结models；smoke不创建selection。锁包含campaign、模型、protocol、method、实际source/binary/依赖、gates与统计代码。test构建前后核对来源；非空目录零写入拒绝。T4.1.10工程准备PASS不等于识别合格。

## 效用、资源和保留

test保留50seed×三scene×disabled/default分片及固定目标诊断、默认迁移previous-bin关联，仍不反馈F。固定目标past身份/决策read、future signed、partial/unavailable和seed负和审计；新风险分层只用past字段，future只用于结果。single与joint/layout/queueing/I/O/线上效用严格区分。

workers默认2、最多4。smoke至少0.5GiB、select至少2GiB、test至少4GiB，阶段前复查；预计select约0.8GiB/test约1.2GiB加源码/审计余量，以实际工程占用校正。容量不足停止报告、由用户处理，不删除历史或自动恢复旧raw。新raw、全部产物、commands/preflight/build/tests、源码/binary/protocol ZIP及gzip原文hash均保留。

T4.1.10完成条件：独立机制/因果fixtures、小/全规模smoke、完整回归、raw/gzip/source锁复验及readiness报告。formal四split均不得存在。T4.1.11须额外用户指令才执行。只有eligible锁且held-out全部gate/来源/效用观测审计通过才允许T4.2独立设计，不自动实施T4.2。
