# 第八轮工程机制诊断

所有比较共享独立check seed和输入；2个seed的区间只核验管道，不授研究资格。
完整失败、短段、risk FP/恢复及确认额外FN分别保留，未观测不是通过。

| 阶段 | 机制 | FP平均差值范围 | steady FN平均差值范围 |
|---|---|---|---|
| mechanisms | bounded_current_evidence | -3 .. 35.5 | -64.5 .. 42 |
| mechanisms | bounded_lower_evidence | -4 .. 24.5 | -57.5 .. 42 |
| mechanisms | lower_history_scale | -12 .. 110.5 | -197.5 .. 2.5 |
| mechanisms | lower_vs_mad_floor | -16 .. 110.5 | -351 .. 1.5 |
| mechanisms | matched_seventh_min3 | 0 .. 0 | 0 .. 0 |
| mechanisms | positive_path | -131.5 .. 0 | 0 .. 659 |
| small | bounded_current_evidence | -1 .. 5 | -2.5 .. 409.5 |
| small | bounded_lower_evidence | -1 .. 14 | -11 .. 384.5 |
| small | lower_history_scale | 0 .. 16.5 | -122.5 .. 44 |
| small | lower_vs_mad_floor | -3.5 .. 16.5 | -59.5 .. 0 |
| small | matched_seventh_min3 | 0 .. 0 | 0 .. 0 |
| small | positive_path | -27.5 .. 0 | 0 .. 396 |
| full | bounded_current_evidence | 0 .. 3.5 | -4 .. 540 |
| full | bounded_lower_evidence | -11 .. 16 | 0 .. 540 |
| full | lower_history_scale | -0.5 .. 48 | -44 .. 63.5 |
| full | lower_vs_mad_floor | -1 .. 96 | -180 .. 15.5 |
| full | matched_seventh_min3 | 0 .. 0 | 0 .. 0 |
| full | positive_path | -85 .. 0 | -3.5 .. 540 |

负差表示错误较少；跨组范围不是总体效应。负侧尺度、镜像和健康历史均有假设，不能借工程PASS声称解决识别。
当前点锚定及有界证据只控制补偿方式；健康桥接、右偏、长相关与漂移仍按完整gates判断。
1/2-bin确认启动不可达与额外steady FN分别报告；短段人工样本不授正式短段资格。
固定目标负seed、partial/unavailable和重叠原因保留；相同past可有不同future，single非可加，不证明线上收益。
