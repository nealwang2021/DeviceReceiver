# Windows 云端持续集成（MSVC2019 + Qt 5.15.2）

本仓库已提供 GitHub Actions 工作流：  
`/home/runner/work/DeviceReceiver/DeviceReceiver/.github/workflows/windows-msvc2019-qt5152-ci.yml`

目标：在云端尽量贴近本地生产环境（Windows + MSVC2019 + Qt 5.15.2）自动完成构建校验。

## 1. 触发方式

- Push 到以下分支会触发：
  - `main`
  - `master`
  - `develop`
  - `feature/**`
- 所有 Pull Request 会触发
- 支持手动触发（`workflow_dispatch`）

## 2. CI 环境与构建参数

- Runner：`windows-2019`
- 编译器工具链：`Visual Studio 16 2019`（x64）
- Qt：`5.15.2`，架构 `win64_msvc2019_64`
- 构建系统：CMake
- 关键 CMake 参数：
  - `-DENABLE_GRPC=OFF`
  - `-DENABLE_HDF5=OFF`
  - `-DENABLE_WASM=OFF`
  - `-DBUILD_TESTS=OFF`

说明：CI 先保证“核心桌面端可编译通过”，避免云端环境因缺失本地 vcpkg 依赖导致误报。  
如需在 CI 打开 gRPC/HDF5，可在工作流里补充对应依赖安装后将上述选项改为 `ON`。

## 3. 查看结果与日志

1. 打开 GitHub 仓库的 **Actions** 页面  
2. 进入工作流 **Windows MSVC2019 Qt5.15.2 CI**  
3. 查看每个步骤日志：
   - Install Qt
   - Configure (CMake)
   - Build
   - Upload build artifacts

## 4. 产物下载

CI 会上传构建产物（若存在）：

- `build_ci/build/release/realtime_data.exe`
- `build_ci/build/release/*.dll`

可在对应 workflow run 的 **Artifacts** 区域下载：

- `realtime_data-windows-msvc2019-qt5152`

## 5. 后续可选增强

- 增加 Debug 构建矩阵
- 增加单元测试/集成测试步骤（当 tests 目录完善后）
- 增加基于 vcpkg 缓存的 gRPC/HDF5 全功能构建任务
