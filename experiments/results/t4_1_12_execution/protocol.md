# T4.1.13 第六轮预注册协议

T4.1.12准备，2026-10-15（America/Chicago）。语义见[方法](../design/detector-sixth-round.md)。工程期间允许修订；最终readiness的协议/source/binary ZIP构成正式数据前冻结。本任务不执行formal。

## 拆分与候选

三场景/两策略沿用第五轮，正式规模100 disks、5000 stripes、100万reads、10000-request bin、100bins。

| split | seeds | 资格 |
|---|---|---|
| fit | 19001..19020 | 新、label-blind历史 |
| calibration | 20001..20020 | 新、独立点/路径校准 |
| validation | 21001..21020 | 新、全部硬gate选参 |
| test | 22001..22050 | 新、一次锁参held-out |
| small工程 | 99001 fit / 99002..99005 cal / 99006 check | 10万reads、500stripes、1000-request bin |
| full工程 | 99011 fit / 99012..99015 cal / 99016 check | 正式规模，仅工程 |
| 人工机制 | 99021..99024 | 独立生成，不读取正式数据 |

排除前五轮全部正式/工程/人工 seeds，split互斥；工程数据不获得held-out资格。24候选依 `adaptation=(disk_history,contrast_offset,contrast_curve)` × `calibration=(mirror_path,balanced_path)` × q=(.99,.995) × strength=(0,1) 的itertools顺序，h=3。

第五轮15 controls全部保留为硬组；各控制对全部状态变换，不读GT。cold_history仍移除disk0全部fit历史。未知盘/漂移不足明确失败，不降覆盖要求。新独立人工矩阵补齐压力/阶段混杂、共同残差投影、负侧尾、条件污染、右偏/AR、桥接与1/2/3/5bin，不以人工结果代替formal。

## 门槛与统计

继承第五轮全部绝对/比较/稳健性门槛：每组coverage≥99%、full recall≥90%、mild steady≥95%、severe≥95%、precision≥95%、healthy FPR≤.1%、pre-first-onset false episodes=0、onset≥95%、lag median/p90≤3、fully-healthy clear p90≤1且无uncleared、全部rank足够、count OOD≤1%。完整mild、startup、steady、短episode、risk FP仍报告；额外报告balanced拒绝与额外FN。风险native/coupled/shift/skew/heterogeneous/correlated/history_shift/long_memory：recall/severe/onset≥95%、lag median≤1/p90≤2。

native Health-EC对D12同h的steady配对seed95%lower≥−2pp；stress要求full FN减少≥50%且steady lower>0。coupled对fixed15同h FPR减少≥80%、steady lower≥−2pp；至少一coupled组相对有效直接raw constant的FP或steady FN减少≥50%，另一不增加且绝对质量通过。shift对有效直接raw frozen FPR减少≥80%、steady lower≥−2pp。参照全部sufficient、coverage≥99%；参照FPR=0不能声称降低；无效参照不能授予收益。

独立seed bootstrap10000次、RNG20261015、percentile95%，seed配对；disk/bin/control不作独立样本。pooled计数与seed CI并列。median/路径rank不赋予理论保证。

## 锁定及一次test

eligible须全部硬gate及有效constant收益通过；排名按最差precision降序、最大FPR升序、最差severe降序、最大lag p90升序、ID。无eligible时一次锁诊断集合 **2/10/22**，对应旧历史/对照offset/对照curve+balanced，均q.995/strength0。仅检验机制，不是默认/部署参数，也不是沿用第五轮2/18/22。test只消费eligible单项或预注册集合，不重选、重拟合、补其他候选。

select只读取fit/cal/validation；test只读test和封存锁，构建前后核对model/protocol/source/binary。smoke不创建selection。无eligible保持研究NO_GO，不自动T4.2。test保留50seed×三场景×disabled/default分片及固定目标观测、默认migration previous-bin关联，F不反馈动作。

## 生产回执、互斥与重启

唯一执行者使用OS文件锁，异常退出由内核释放；同一campaign的并发select/test/恢复拒绝。各阶段状态、PID、commands/log及终态持久保存。生产raw每条命令成功且schema验证后立即生成只创建一次的JSON回执（路径白名单、SHA256/size、完成时间、命令），fsync文件与目录；只有全部预期raw及schema/schedule回执齐全并复验才封存collection。缺少任一生产回执即拒绝恢复，不用恢复hash补出签名。机器/存储失效仍有持久性边界，不宣称密码学可信执行。

未封存的select评估允许明确恢复到新目录：先核对完整collection seal、全部生产回执、实际source/binary/protocol ZIP，再复制并重算尚未锁定评估；原失败目录保留，不重采。若selection/models锁已出现则拒绝该恢复分支；封存select后的test续跑必须核对原锁，绝不重拟合。已消费但未封存test不自动重跑，须保留现场并由用户按原协议明确处置。阶段边界暂停不启动下一阶段；resume入口必须显式模式，不把重启当授权。

## 资源与完成条件

workers默认2、最多4；工程每阶段至少4GiB余量，select至少8GiB，test至少12GiB，stage前复查；预计raw/select<2GiB、test含全部gzip/来源/审计<4GiB，以工程实测核对。容量不足停止由用户处理，不删除历史。全部新raw/schema/schedule、生产hash、完整模型/输出、source/binary/protocol ZIP与gzip原文hash保留。

T4.1.12须完成：方法与协议、实现/完整回归、独立逻辑和因果fixtures、恢复/互斥/暂停契约验证、小/全规模smoke、全部来源/raw/gzip/readiness核验及失败交接；formal四split均未运行。T4.1.13必须另获用户执行指令。工程PASS不等于研究识别合格；不改变第五轮生产签名缺失限制、不降低原研究目标。
