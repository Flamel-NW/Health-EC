# Health-EC 历史清理记录

- 本地dev/main及origin跟踪引用已清理，移除3720历史路径；保留文件树经55个提交逐一核对。
- 6578份实验文件留在原路径，未移动/删除；此前全量SHA-256核验通过，本次回退只删除额外治理文件。
- [唯一数据清单](data_manifest.csv)记录原路径、大小与SHA-256；[提交映射](history_cleanup_commit_map.csv)用于解释原实验日志中的旧revision。
- 清理前历史与旧数据版本仍保存在本仓库被忽略的`.local-data/git-history-cleanup/20261012T234621-0500/`；历史路径清单为同目录`historical_paths.csv`。
- 不保留policy、守卫/测试、独立校验工具或清理/同步脚本。通过`.gitignore`过滤数据，新增文件仅按明确路径添加。
- 本地dev=`65e7c4d5cf9a9cd2f0d9cbeae85354e30bf9ced3`，main=`884434866e52460c7a06924491b7b22f6f687cdb`；2026-10-13已核对用户完成的远端同步，dev/main与本地一致，无标签；后续归档使用普通push。
