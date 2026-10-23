# 固定路径测试目录更新

手序版 Windows x64 预发行包完成 Release 构建、聚焦测试、归档完整性和哈希校验后，把包内容同步到 `D:\Maimai\MiaCode_HandOrder_Test` 根目录。操作者固定双击该目录的 `MiaCode.exe`；不要从 `app/` 内直接启动。此目录是用户明确指定的本地测试安装，不是正式版、release 或 dev 安装目录。

从 `source/` 执行：

```powershell
& .\scripts\build\sync-hand-order-test-install.ps1 -PackageDir ..\dist\<已验证的手序包目录>
```

脚本只接受本工作区 `dist/` 下带 `StickingPackage.marker`、启动器和干净 Git 身份的包；测试程序正在运行时拒绝替换。先在工作区 `exports/` 复制并校验，再把旧测试目录整体移入 `exports/hand_order_test_install_backups/`，最后将新目录移到固定路径。若新目录移动失败，脚本尝试恢复旧目录。备份不会自动删除。

固定路径建立后，后续同步会保留旧根目录的 `runtime/`、`exports/` 和 `logs/`，以便持续测试。首次从 v0.2.1 的版本号子目录改为固定根目录时，不自动迁移旧 `runtime/`；旧目录和其中的测试数据完整留在备份中。不得从正式版复制偏好、恢复内容或真实谱面。

发布工作以 GitHub 附件核验和本地测试目录同步均完成为结束条件。同步后检查固定路径的启动器、包内 README 构建身份及 `app/MiaCode.exe` 哈希；主 GUI 操作仍由用户手动验收。
