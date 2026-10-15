# T4.1.9 第四轮预注册协议

T4.1.8，2026-10-13（America/Chicago）；正式数据前冻结，[方法](../design/detector-fourth-round.md)与[固定目标契约](../design/fixed-target-observation.md)为语义来源。detector-only，GT仅评估，F不反馈read/migration；原三轮失败/协议/已见test保留。

## Splits与模式

正式100 disks、5000 stripes、1000000 reads、10000-request bin（100 bins）、Zipf1、三realistic/overlap/stress场景，Health-EC migration_disabled及Vanilla EC。bin-end才可用。

| split | seeds | 用途 |
|---|---|---|
| fit | 11001..11020 | label-blind location/pressure分区 |
| calibration | 12001..12020 | label-blind点/负block/common上界 |
| validation | 13001..13020 | 全gate选参或事前诊断计划 |
| test | 14001..14050 | 一次held-out研究/分片审计 |
| small engineering | fit95001/cal95002..95005/check95006 | 100000 reads/500 stripes/1000-request bin |
| full engineering | fit95011/cal95012..95015/check95016 | 正式规模工程核对 |
| logic fixtures | 95021 | 独立机制、无正式数据 |

全部互斥并排除所有旧正式/工程seeds。T4.1.8不采集任何正式split。smoke不能产生selection.json；select仅访问前三split；test只访问test并消费来源核对的冻结models/候选集，build前后核对source/binary/protocol/campaign。非空result dir拒绝零写入。

## 候选、controls与参照

24候选：peer_line/peer_local×raw/mirror/mirror_block×q.99/.999×strength0/1，h=3固定。五折/40peers/logspan≥log1.5/local20、全折median±2×1.4826MAD局部修正约束、peer_local的当前全折标准化残差MAD尺度（下限1e−6）、pressure四分位点、block划分、负侧中央median幅度余量和有限rank见方法，任一bucket/common界或中央幅度不足则insufficient。各scene/policy/reference独立模型，一个统一候选跨全部组。

validation/test均要求native、coupled、impulse、burst2、shift、shift_impulse、shift_burst2、skew、heterogeneous、correlated、bridge、shift_burst2_correlated。native/coupled/impulse沿用v3；burst2/bridge用native模型，shift及交互用coupled，skew/heterogeneous/correlated独立fit/calibration。控制定义由v4代码冻结：skew为中心化指数噪声6/sqrt(count)，heterogeneous为disk%4固定截距与斜率偏移，correlated为rho.8、innovation6的AR残差/sqrt(count)，bridge为burst2并在前bin加1/sqrt(count)，最后控制叠加shift/burst2/AR。随机扰动按seed/disk独立RNG，不读GT；数据全变换，不能仅给healthy注入。均是半合成观测压力测试，不代表真实queueing。

参照保留D12/D2/S即时、D12同h、fixed15即时/同h、frozen loglinear与constant相同calibration/q/strength。frozen与constant不适配；其校准/确认幅度规则相同。native NI、coupled压力增益、shift漂移增益与matched constant条件沿用第三轮，full差与steady差均报，不按GT改状态机。另保留v3机制对照在独立fixture中，不在旧test试新候选。

## 硬门槛与不确定性

每scene/policy/12 controls confirmed：coverage≥99%，full recall≥90%，severe≥95%，precision≥95%，healthy FPR≤0.1%，pre-first-onset健康false episodes=0，onset≥95%，lag median/p90≤3，fully-healthy clear p90≤1、无uncleared，全部校准rank足够，count OOD≤1% sufficient bins。风险native/coupled/shift/skew/heterogeneous/correlated：recall/severe/onset≥95%、lag median≤1/p90≤2。所有risk FP、operational precision/recovery、warming-up/insufficient FN保留。

Health-EC native realistic/overlap相对D12同h：配对seed steady recall差95%lower≥−2pp；stress full pooled FN减少≥50%、steady差lower>0。coupled各组相对fixed15同h FPR减少≥80%、steady差lower≥−2pp；至少一个coupled组相对matched constant FP或steady FN减少≥50%且另一项不增加，并通过绝对质量。shift相对同calibration/q/strength的frozen FPR减少≥80%、steady差lower≥−2pp；reference FPR=0不证明增益，该gate失败。其余稳健性/交互均为硬gate，不挑选通过轴。

工程schedule可达性：realistic/stress健康前缀条件的full recall上限恰为90%，overlap为94.074%；前两者在当前h=3下没有额外steady FN余量。保留这一强压力条件，不以健康前缀误报获得启动优势，不把gate可达性等同于可稳健满足。small/full engineering只有4cal seeds，mirror_block/.999可能因rank不足而insufficient；formal20seed仍必须实际核验rank，不能由样本量预测授予PASS。

共同steady GT区间排除各positive episode最初h−1 bins；全positive仍计full recall/FN。short1/2/3/5bin的可辨前缀/风险/确认独立报告；三个连续确认对长度L的健康前缀条件上限max(0,L−2)/L，L<3不确认，L=5上限60%。本正式schedule的全recall可达性由工程fixture实际计算；不因上限诊断放宽gates。零pre-onset是有限campaign硬要求，无观测可辨信息时不能承诺普遍零误报；保留同前缀反例。

Bootstrap以seed为单位，10000次、RNG20261012、percentile95%，各disk/bin/control不当独立样本；输出pooled/seed median/min/max、paired full/steady intervals，无多重比较保证。按pressure、global/local peer污染、端点误差、健康偏斜及残差lag1/3相关性分层，只在阈值冻结后用GT解释。

## 选择与失败诊断预注册

eligible必须全部组gate和constant改进通过；合格排名沿用最差precision降序、最大FPR升序、最差severe recall降序、最大lag p90升序、ID。无eligible不再让静默候选因precision主导诊断：预先冻结三个机制诊断候选ID0（line/raw/.99/strength0）、ID10（line/mirror_block/.999/strength0）、ID23（local/mirror_block/.999/strength1）。它们分别检查污染、block与局部+幅度；即使validation失败也一次锁定三者，validation_pass=false，仅诊断，不成为部署参数。不据test结果替换，test评估全部三者并保留全部失败。具体ID由itertools的代码顺序和fixture核对。

## 固定目标收益与进入资格

test另取50seeds×三scene×disabled/default shard_diagnostic与fixed_target_trace；保留D≤w/evidence分区、动作数与≥5访问资格差；default盘级trace关联previous-bin F，不门控旧动作。past固定target和decision_read只能来自past；future不重新选target；source_changed/target_conflict/no_future_reads为unavailable。signed负/零gain和τ15/30完整保留，预算1/4/16单替换和及available分母明确；joint_gain_available=false，online_utility_claim=false。joint/真实布局与migration I/O反馈另行设计。

工程/数据/来源、识别/比较/稳健性、分片因果观测分别授予资格。只有eligible锁、全部held-outgates、来源与固定目标观测审计通过才`ELIGIBLE_FOR_T4.2_DESIGN`，仅许可独立设计，不自动执行T4.2；否则`NO_GO_DIAGNOSE`，按失败机制安排后续研究，不收敛弱claim。

## 资源与产物

入口WORKFLOW_ROOT：`python3 -B scripts/validate_disk_detector_v4.py --mode smoke|select|test`，full smoke用`--engineering-scale full`，test必须selection-dir。默认独立t4_1_8_detector_method_smoke[_full]/t4_1_9_detector_selection/t4_1_9_detector_test。workers1..4默认2。检查smoke≥0.5GiB/select≥2GiB/test≥4GiB，阶段前复查；空间不足立即停止报告由用户处理，不清理历史，阈值不是容量保证。

保留实际commands/preflight/build-and-test/run-status、protocol/method/contract与全部依赖source/binary ZIP、models/group/seed/gates/时序/pressure/peer/校准/失败诊断、raw/schedules/分片和固定目标raw及gzip原文hash。所有工程执行版本/失败独立封存；smoke只能报告工程准备，未执行formal。select/test入口由合成fixture验证后供T4.1.9使用。
