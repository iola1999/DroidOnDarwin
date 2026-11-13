# 决策记录: SDK first-stage 阶段清理

状态: implemented

## 问题

[SDK36 原始 first-stage 两页阶段](../architecture/2026-11-13-SDK36原始first-stage启动.md)已交付，SDK 工作树、大小写敏感 APFS 中的工作树、展开源码和构建输出继续占用本机存储。阶段清单已保存必要来源、实际结果、iOS 对象身份、复现条件和四份持久候选，清理需要保留这些文件及其他会话的工作树。

## 决策

2026-11-13 在主线 `3707b035c788c91f3faadeee0216c93526f26ad2` 核对两个待移除工作树均已合入且没有未提交修改。重新扫描使用状态后，CASE 工作树没有打开文件，SDK 工作树只有 sparseimage 的正常挂载句柄。实际 APFS UUID 为 `A979B14D-7327-41C4-B5AA-3EEFB774C8B1`，与阶段记录及 hdiutil 的镜像路径关联一致。

先通过 `git worktree remove` 移除 CASE 工作树，再卸载固定卷、删除该轮 sparseimage，重新确认 SDK 目录没有打开文件后，通过 `git worktree remove` 移除 SDK 工作树。全部操作成功，未使用 force；其他工作树列表保持原值。

清理前的目录统计、UUID 和执行记录位于 `work/cleanup-2026-11-13/sdk-first-stage-cleanup-ready-v2.json`、`sdk-first-stage-cleanup-v1.jsonl` 和 `sdk-first-stage-cleanup-v1.json`。这些记录保存在主线工作区。SDK 目录的清理前统计包含其挂载的 CASE 目录，两个目录的字节数不能相加计算释放空间。

清理后重新核对 1066 份保留文件的大小和 SHA-256，全部成功。四盘保持交付清单中的最终字节，原共享 `system.img` 和 `ramdisk.img` 的 device、inode、大小、mtime 和 ctime 保持。清单 `work/sdk-first-stage-delivery-inventory-v1/inventory-v3.json` 的 SHA-256 仍为 `583734098c333ddb1f64cffc36f1be90c5d044cdeb4b5a6a5dd8760b0e5bc89e`。本次未重新挂载数据盘、回放 journal 或查询数据库。

## 曾考虑的替代方案

保留展开源码、全部对象和两个工作树供下一阶段继续使用。这些产物已有固定来源和复现条件，保留会同时增加阶段间的构建输入和清理范围。下一阶段使用独立加密候选及重新生成的源码身份。

## 影响

阶段源码和结果已合入并推送，原 tar、四盘、结构化结果、必要日志与重建材料继续保留。完整 Android boot 仍由[原 vold 与 default-key](../../proposed/architecture/2026-11-13-SDK36原始vold与default-key.md)推进；清理成功只确认本阶段的产物保留和目录移除。
