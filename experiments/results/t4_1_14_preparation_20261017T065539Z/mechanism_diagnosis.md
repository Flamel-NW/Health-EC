# 第七轮工程机制诊断

以下比较共享新check seed和观测；差值负值表示错误较少。2个工程seed的区间只验证计算管道，不授正式研究收益。

完整效果见mechanism_effects.csv，全部失败和支持/rank见各组metrics/models及压缩逐bin诊断。

| 阶段 | 机制 | FP平均差值范围 | steady FN平均差值范围 |
|---|---|---|---|
| mechanisms | contemporaneous_history | -47 .. 15.5 | -432.5 .. 1099 |
| mechanisms | amplitude_balance | -2 .. 0 | 0 .. 17 |
| mechanisms | historical_scale_floor | -2.5 .. 6.5 | -78 .. 225 |
| mechanisms | positive_path | -22 .. 2.5 | -19.5 .. 514.5 |
| mechanisms | path_trimming | 0 .. 115.5 | -574.5 .. 0 |
| small | contemporaneous_history | -76 .. 0.5 | -131 .. 82 |
| small | amplitude_balance | 0 .. 0 | 0 .. 44.5 |
| small | historical_scale_floor | -1 .. 7 | -88.5 .. 98.5 |
| small | positive_path | -7 .. 0 | 18.5 .. 369.5 |
| small | path_trimming | 0 .. 70.5 | -380.5 .. -20 |
| full | contemporaneous_history | -247 .. 0.5 | -265.5 .. 68 |
| full | amplitude_balance | 0 .. 0 | 0 .. 35.5 |
| full | historical_scale_floor | -55 .. 8.5 | -64 .. 182 |
| full | positive_path | -8.5 .. 0 | 33 .. 540 |
| full | path_trimming | 0 .. 101.5 | -540 .. -32 |

所有逐组差异保留；跨组最小/最大只作定位，不是独立样本或总体效应。
持续盘型与持续退化、未知盘及健康历史污染仍需结合独立双世界/支持阶梯判定；共同退化不能依赖相对同伴检测自动识别。
正尾筛除可能筛去真实健康长相关尾，直接正尾可能吸收慢盘污染；不将工程PASS解释为解决这些问题。
h3启动界不解释额外steady FN；短episode、risk FP及uncleared保留在原始时序。
固定目标signed负seed和partial/unavailable单列，single不等于joint或线上收益。
