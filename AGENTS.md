# AGENTS.md

## Cursor Cloud specific instructions

### Project Overview

This is a Qt 5.15 C++ real-time data monitoring desktop application (DeviceReceiver) with Python gRPC mock servers. See `README.md` for full documentation.

### Primary Development Environment (Owner's Local)

- **OS:** Windows 10/11
- **Compiler:** MSVC 2019 x64 (Visual Studio 2019, v14.2+)
- **Qt:** 5.15.2 msvc2019_64
- **Package manager:** vcpkg (gRPC, HDF5, protobuf v6.x, and transitive deps)
- **Build:** `build_cmake.bat` (CMake, full features including gRPC + HDF5)

Cloud agents must not introduce changes that break the Windows MSVC 2019 + Qt 5.15 build. The checked-in proto generated files (`proto/generated/`) and `CMakeLists.txt` Windows linking logic target this toolchain.

### Building on Linux (Cloud Agent Environment)

The project was designed for Windows (MSVC + vcpkg), but builds on Linux with GCC and system Qt5 packages.

**CMake configure & build:**
```bash
mkdir -p build_linux && cd build_linux
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ -DENABLE_GRPC=OFF -DENABLE_HDF5=OFF -DENABLE_WASM=OFF -DBUILD_TESTS=OFF
cmake --build . --config Release -j$(nproc)
```

Binary output: `build_linux/build/release/realtime_data`

**Important:** gRPC C++ support is disabled on Linux because the checked-in proto generated files (`proto/generated/`) target protobuf v6.x (Windows vcpkg), which is incompatible with the Ubuntu system protobuf v3.21.x. The app still works in mock-data mode and with the Python gRPC test servers.

### Running the Application

```bash
export DISPLAY=:1
QT_QPA_PLATFORM=xcb ./build_linux/build/release/realtime_data
```

On first launch it creates `config.ini` next to the executable. The app defaults to gRPC backend; since gRPC C++ is not compiled in, enable "启用设备 gRPC 本地模拟" (mock data checkbox) in the left device panel, then click "连接" (Connect) to stream simulated data.

### Python gRPC Test Servers

The Python test servers work independently of the C++ gRPC build:

```bash
# Device data service (port 50051)
python3 grpc_test_server.py --port 50051

# Stage service (port 50052) — requires generated stage_pb2 stubs
python3 stage_grpc_test_server.py --port 50052
```

If `stage_pb2` is missing, regenerate stubs:
```bash
python3 -m grpc_tools.protoc -Iproto --python_out=proto/generated_py --grpc_python_out=proto/generated_py proto/stage.proto
```

### Gotchas

- The default C++ compiler on the Cloud VM is Clang 18, which fails to link due to missing `-lstdc++`. Always pass `-DCMAKE_CXX_COMPILER=g++` to CMake.
- Port binding on `0.0.0.0` may fail for some ports in the VM. Use `--host 127.0.0.1` or try a different port if binding fails.
- The `tests/` directory referenced in `CMakeLists.txt` (`BUILD_TESTS=ON`) does not exist in the repo. Always pass `-DBUILD_TESTS=OFF`.
- QCustomPlot deprecation warnings (`HighQualityAntialiasing`) are from the vendored library and can be ignored.
- Build artifacts go to `build_linux/` on Linux (vs `build_cmake/` or `build/` on Windows).
