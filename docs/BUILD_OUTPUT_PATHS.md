# 编译产物路径（避免「改了代码却像没生效」）

本仓库有 **两套** 构建方式，生成的 `realtime_data.exe` **不在同一目录**。请确认你运行的是 **刚编译的那一份**。

| 构建方式 | 典型输出路径（相对仓库根目录） |
|----------|--------------------------------|
| **CMake**（`build_cmake.bat`） | `build_cmake\build\release\realtime_data.exe`（Debug 则为 `...\debug\...`） |
| **qmake**（`build_and_run.bat`） | `build\release\realtime_data.exe`（Debug 则为 `build\debug\...`） |

**建议**

1. 在资源管理器中查看 exe **修改时间**，应等于你最近一次编译时间。
2. 若不确定，可先 **彻底重编**：  
   - CMake：`build_cmake.bat -Clean` 后再 `build_cmake.bat`  
   - 或删除对应 `build` / `build_cmake` 目录后重新配置编译。
3. 桌面快捷方式、开始菜单若指向旧路径，会一直打开旧程序。
