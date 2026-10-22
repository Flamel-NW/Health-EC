# 第九轮工程机制诊断

所有比较共享独立check seed和输入；2个seed的区间只核验管道，不授研究资格。
完整失败、短段、risk FP/恢复及确认额外FN分别保留，未观测不是通过。

| 阶段 | 机制 | FP平均差值范围 | steady FN平均差值范围 |
|---|---|---|---|
| mechanisms | conditional_distribution | -1199.5 .. 2129 | -10 .. 475 |
| mechanisms | matched_eighth_min3 | 0 .. 0 | 0 .. 0 |
| mechanisms | nonlinear_shape_min3 | -118.5 .. 0.5 | -18 .. 381 |
| mechanisms | nonlinear_shape_order2 | -320 .. 1 | -19 .. 381 |
| mechanisms | nonlinear_shape_positive_order2 | -92 .. 3.5 | -19.5 .. 21.5 |
| mechanisms | order_conditional_evidence | -93 .. 50 | -9 .. 5 |
| mechanisms | order_current_evidence | 0 .. 2430.5 | -32 .. 1 |
| mechanisms | order_vs_capped3 | -1211 .. 49.5 | -42 .. 64 |
| mechanisms | positive_path | -2.5 .. 234 | -6.5 .. 0.5 |
| mechanisms | quantile_order_vs_mad_min3 | -1199.5 .. 2036 | -18 .. 442 |
| mechanisms | quantile_vs_lower_scale | -1199.5 .. 2127 | -10 .. 633 |
| small | conditional_distribution | -8 .. 273 | 25.5 .. 445.5 |
| small | matched_eighth_min3 | 0 .. 0 | 0 .. 0 |
| small | nonlinear_shape_min3 | -103.5 .. 4 | 5.5 .. 186 |
| small | nonlinear_shape_order2 | -156 .. 128.5 | 8 .. 203.5 |
| small | nonlinear_shape_positive_order2 | -0.5 .. 530.5 | -2 .. 2 |
| small | order_conditional_evidence | -0.5 .. 323.5 | -3 .. 1 |
| small | order_current_evidence | -0.5 .. 43 | -20 .. 12 |
| small | order_vs_capped3 | -3.5 .. 43.5 | -443 .. 1 |
| small | positive_path | -253.5 .. 186 | -3.5 .. 3.5 |
| small | quantile_order_vs_mad_min3 | -4.5 .. 583.5 | 13.5 .. 405 |
| small | quantile_vs_lower_scale | -9 .. 264.5 | 19.5 .. 433 |
| full | conditional_distribution | -18.5 .. 469 | 54 .. 540 |
| full | matched_eighth_min3 | 0 .. 0 | 0 .. 0 |
| full | nonlinear_shape_min3 | -99 .. 3 | 22 .. 277 |
| full | nonlinear_shape_order2 | -437 .. 5 | 16 .. 311 |
| full | nonlinear_shape_positive_order2 | 0 .. 780 | 0 .. 3 |
| full | order_conditional_evidence | -1 .. 341.5 | -4.5 .. 0 |
| full | order_current_evidence | -2 .. 51 | -10.5 .. 70.5 |
| full | order_vs_capped3 | -4.5 .. 50.5 | -540 .. 1 |
| full | positive_path | -665.5 .. 73.5 | 0 .. 5 |
| full | quantile_order_vs_mad_min3 | -5.5 .. 795 | 36 .. 540 |
| full | quantile_vs_lower_scale | -105.5 .. 456.5 | 52.5 .. 540 |

负差表示错误较少；跨组范围不是总体效应。条件分位数映射、镜像和健康历史均有假设，不能借工程PASS声称解决识别。
当前点锚定和次序证据不保证排除双尖峰；健康桥接、右偏、长相关与漂移仍按完整gates判断。
1/2-bin确认启动不可达与额外steady FN分别报告；短段人工样本不授正式短段资格。
固定目标负seed、partial/unavailable和重叠原因保留；相同past可有不同future，single非可加，不证明线上收益。
