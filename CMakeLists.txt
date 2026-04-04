cmake_minimum_required(VERSION 3.16)
project(DeviceReceiver VERSION 1.0.1 LANGUAGES CXX)

# 可选功能：默认启用gRPC和HDF5支持
option(ENABLE_GRPC "Enable gRPC client support" ON)
option(ENABLE_HDF5 "Enable HDF5 export support" ON)
option(ENABLE_WASM "Enable WebAssembly build" OFF)

# 设置默认Qt路径（如果未通过CMAKE_PREFIX_PATH指定）
if(NOT DEFINED CMAKE_PREFIX_PATH AND NOT DEFINED QT5_DIR)
    # 尝试自动检测Qt安装
    if(EXISTS "C:/Qt/5.15.2/msvc2019_64")
        set(CMAKE_PREFIX_PATH "C:/Qt/5.15.2/msvc2019_64" CACHE PATH "Qt安装路径")
    elseif(EXISTS "C:/Qt/5.15.2/msvc2022_64")
        set(CMAKE_PREFIX_PATH "C:/Qt/5.15.2/msvc2022_64" CACHE PATH "Qt安装路径")
    elseif(EXISTS "C:/Qt/5.15.2/mingw81_64")
        set(CMAKE_PREFIX_PATH "C:/Qt/5.15.2/mingw81_64" CACHE PATH "Qt安装路径")
    endif()
endif()

# 查找Qt库（Qt 5.15）
find_package(Qt5 5.15 REQUIRED COMPONENTS Core Gui Widgets Network PrintSupport)
if(NOT ENABLE_WASM)
    find_package(Qt5 5.15 REQUIRED COMPONENTS SerialPort OpenGL)
else()
    find_package(Qt5 5.15 REQUIRED COMPONENTS Network)
endif()

# 设置C++标准（与.pro文件一致）
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 设置输出目录（与.pro和build_and_run.bat保持一致）
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/build/debug)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/build/release)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/build/debug)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/build/release)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/build/debug)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/build/release)

# Qt自动处理
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# 自动检测vcpkg路径（与build_and_run.bat保持一致）
if(NOT DEFINED VCPKG_ROOT)
    if(EXISTS "D:/vcpkg/vcpkg.exe")
        set(VCPKG_ROOT "D:/vcpkg")
    elseif(EXISTS "C:/vcpkg/vcpkg.exe")
        set(VCPKG_ROOT "C:/vcpkg")
    endif()
endif()

if(VCPKG_ROOT)
    set(HDF5_ROOT "${VCPKG_ROOT}/installed/x64-windows" CACHE PATH "HDF5 root directory")
    set(GRPC_ROOT "${VCPKG_ROOT}/installed/x64-windows" CACHE PATH "gRPC root directory")
endif()

# 源文件列表（从realtime_data.pro复制）
set(SOURCE_FILES
    main.cpp
    DataCacheManager.cpp
    DataExporter.cpp
    SerialReceiver.cpp
    GrpcReceiverBackend.cpp
    StageReceiverBackend.cpp
    PlotWindow.cpp
    PlotWindowBase.cpp
    HeatMapPlotWindow.cpp
    ArrayPlotWindow.cpp
    PulsedDecayPlotWindow.cpp
    InspectionPlotWindow.cpp
    DataProcessor.cpp
    ApplicationController.cpp
    AppConfig.cpp
    PlotDataHub.cpp
    StagePoseLatch.cpp
    PlotWindowManager.cpp
    MainWindow.cpp
)

# 头文件列表
set(HEADER_FILES
    FrameData.h
    GrpcEndpointUtils.h
    DataCacheManager.h
    DataExporter.h
    IReceiverBackend.h
    SerialReceiver.h
    GrpcReceiverBackend.h
    StageReceiverBackend.h
    PlotWindow.h
    PlotWindowBase.h
    HeatMapPlotWindow.h
    ArrayPlotWindow.h
    PulsedDecayPlotWindow.h
    InspectionPlotWindow.h
    DataProcessor.h
    ApplicationController.h
    AppConfig.h
    PlotDataHub.h
    StagePoseLatch.h
    PlotWindowManager.h
    MainWindow.h
)

# UI文件
set(UI_FILES
    MainWindow.ui
)

# 资源文件
set(RESOURCE_FILES
    realtime_data.qrc
)

# 合并所有源文件
set(ALL_SOURCES ${SOURCE_FILES} ${HEADER_FILES} ${UI_FILES} ${RESOURCE_FILES})

# 添加可执行文件
add_executable(realtime_data ${ALL_SOURCES})

# 设置目标属性（保留控制台窗口，与.pro的CONFIG+=console一致）
set_target_properties(realtime_data PROPERTIES
    WIN32_EXECUTABLE FALSE
    MACOSX_BUNDLE FALSE
)

# 链接Qt库
target_link_libraries(realtime_data
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Network
    Qt5::PrintSupport
)

if(NOT ENABLE_WASM)
    target_link_libraries(realtime_data Qt5::SerialPort Qt5::OpenGL)
endif()

# 包含目录
target_include_directories(realtime_data PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 第三方库：3rd_qcustomplot（复制.pri文件逻辑）
target_include_directories(realtime_data PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd_qcustomplot/3rd_qcustomplot
)

if(NOT ENABLE_WASM)
    # 非WASM环境：启用OpenGL和freeglut
    find_package(OpenGL REQUIRED)
    target_link_libraries(realtime_data OpenGL::GL OpenGL::GLU)
    
    # 添加freeglut库
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/3rd_qcustomplot/3rd_qcustomplot/freeglut/lib/x64/freeglut.lib")
        target_link_libraries(realtime_data "${CMAKE_CURRENT_SOURCE_DIR}/3rd_qcustomplot/3rd_qcustomplot/freeglut/lib/x64/freeglut.lib")
        target_include_directories(realtime_data PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/3rd_qcustomplot/3rd_qcustomplot/freeglut/include
        )
    endif()
    
    # 设置QCUSTOMPLOT_USE_OPENGL定义
    target_compile_definitions(realtime_data PRIVATE QCUSTOMPLOT_USE_OPENGL)
endif()

# 根据Qt版本选择qcustomplot版本（复制.pri逻辑）
# Qt5及以上使用v2.1版本（默认）
target_include_directories(realtime_data PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd_qcustomplot/3rd_qcustomplot/v2_1
)
target_sources(realtime_data PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd_qcustomplot/3rd_qcustomplot/v2_1/qcustomplot.cpp
)

# 添加smoothcurve类
target_sources(realtime_data PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/3rd_qcustomplot/3rd_qcustomplot/smoothcurve.cpp
)

# 编译定义（与.pri文件保持一致）
target_compile_definitions(realtime_data PRIVATE
    qcustomplot_v2_1
    qcustomplot
    QT_USE_UTF8STRING
)

if(WIN32)
    target_compile_definitions(realtime_data PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()

# HDF5支持
if(ENABLE_HDF5)
    if(DEFINED HDF5_ROOT)
        if(EXISTS "${HDF5_ROOT}/include/hdf5.h")
            target_include_directories(realtime_data PRIVATE ${HDF5_ROOT}/include)
            target_link_directories(realtime_data PRIVATE ${HDF5_ROOT}/lib)
            target_link_libraries(realtime_data hdf5)
            target_compile_definitions(realtime_data PRIVATE HAS_HDF5)
            message(STATUS "HDF5支持已启用: ${HDF5_ROOT}")
        else()
            message(WARNING "HDF5_ROOT无效，禁用HDF5支持")
            set(ENABLE_HDF5 OFF)
        endif()
    else()
        message(WARNING "未设置HDF5_ROOT，禁用HDF5支持")
        set(ENABLE_HDF5 OFF)
    endif()
endif()

# gRPC支持
if(ENABLE_GRPC)
    if(DEFINED GRPC_ROOT)
        if(EXISTS "${GRPC_ROOT}/include/grpcpp/grpcpp.h")
            # 包含proto生成文件目录
            target_include_directories(realtime_data PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/proto/generated
                ${GRPC_ROOT}/include
            )
            
            # 添加proto生成源文件
            target_sources(realtime_data PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/proto/generated/device.pb.cc
                ${CMAKE_CURRENT_SOURCE_DIR}/proto/generated/device.grpc.pb.cc
                ${CMAKE_CURRENT_SOURCE_DIR}/proto/generated/stage.pb.cc
                ${CMAKE_CURRENT_SOURCE_DIR}/proto/generated/stage.grpc.pb.cc
            )
            
            # 链接gRPC库（Windows MSVC）
            if(WIN32)
                target_link_directories(realtime_data PRIVATE ${GRPC_ROOT}/lib)
                target_link_libraries(realtime_data
                    grpc++
                    grpc
                    gpr
                    upb_base_lib
                    upb_hash_lib
                    upb_json_lib
                    upb_lex_lib
                    upb_mem_lib
                    upb_message_lib
                    upb_mini_descriptor_lib
                    upb_mini_table_lib
                    upb_reflection_lib
                    upb_textformat_lib
                    upb_wire_lib
                    libupb
                    address_sorting
                    re2
                    utf8_range
                    utf8_validity
                    abseil_dll
                    absl_flags_commandlineflag
                    absl_flags_commandlineflag_internal
                    absl_flags_config
                    absl_flags_internal
                    absl_flags_marshalling
                    absl_flags_parse
                    absl_flags_private_handle_accessor
                    absl_flags_program_name
                    absl_flags_reflection
                    absl_flags_usage
                    absl_flags_usage_internal
                    cares
                    zlib
                    libprotobuf
                    libssl
                    libcrypto
                    ws2_32
                    crypt32
                )
            elseif(UNIX AND NOT APPLE)
                # Linux
                target_link_libraries(realtime_data
                    grpc++
                    grpc
                    protobuf
                    pthread
                    dl
                )
            elseif(APPLE)
                # macOS
                target_link_libraries(realtime_data
                    grpc++
                    grpc
                    protobuf
                    pthread
                )
            endif()
            
            target_compile_definitions(realtime_data PRIVATE HAS_GRPC)
            message(STATUS "gRPC支持已启用: ${GRPC_ROOT}")
        else()
            message(WARNING "GRPC_ROOT无效，禁用gRPC支持")
            set(ENABLE_GRPC OFF)
        endif()
    else()
        message(WARNING "未设置GRPC_ROOT，禁用gRPC支持")
        set(ENABLE_GRPC OFF)
    endif()
endif()

# WASM特定设置
if(ENABLE_WASM)
    target_compile_definitions(realtime_data PRIVATE QT_COMPILE_FOR_WASM)
    # WASM需要更大的初始内存
    set_target_properties(realtime_data PROPERTIES
        LINK_FLAGS "-s TOTAL_MEMORY=33554432"
    )
endif()

# 编译选项
if(MSVC)
    # MSVC：UTF-8编码，多线程编译，警告级别W3
    target_compile_options(realtime_data PRIVATE
        /utf-8
        /MP
        /W3
        /wd4819  # 禁用C4819编码警告
    )
    
    # Release模式可调试（与.pro文件一致）
    set_target_properties(realtime_data PROPERTIES
        COMPILE_FLAGS_RELEASE "/Zi"
        LINK_FLAGS_RELEASE "/DEBUG"
    )
else()
    # GCC/Clang/Mingw
    target_compile_options(realtime_data PRIVATE
        -Wall
        -Wextra
        -finput-charset=UTF-8
        -fexec-charset=UTF-8
    )
    
    # Release模式可调试
    set_target_properties(realtime_data PROPERTIES
        COMPILE_FLAGS_RELEASE "-O0 -g"
        LINK_FLAGS_RELEASE "-g"
    )
endif()

# 程序信息（从.pro文件复制）
set_target_properties(realtime_data PROPERTIES
    VERSION 1.0.1
    # RC_ICONS main.ico  # 如果需要图标
    # 产品名称
    # QMAKE_TARGET_PRODUCT "QtTemplateApp"
    # 版权所有
    # QMAKE_TARGET_COPYRIGHT "某某科技"
    # 文件说明
    # QMAKE_TARGET_DESCRIPTION "台湾是中国不可分割的一部分"
    # 中文（简体）
    # RC_LANG 0x0804
)

# 安装后复制依赖DLL（模拟windeployqt）
if(WIN32 AND NOT ENABLE_WASM)
    add_custom_command(TARGET realtime_data POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt5::Core>" "$<TARGET_FILE_DIR:realtime_data>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt5::Gui>" "$<TARGET_FILE_DIR:realtime_data>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt5::Widgets>" "$<TARGET_FILE_DIR:realtime_data>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt5::Network>" "$<TARGET_FILE_DIR:realtime_data>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt5::PrintSupport>" "$<TARGET_FILE_DIR:realtime_data>"
    )
    
    if(NOT ENABLE_WASM)
        add_custom_command(TARGET realtime_data POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Qt5::SerialPort>" "$<TARGET_FILE_DIR:realtime_data>"
        )

        add_custom_command(TARGET realtime_data POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Qt5::OpenGL>" "$<TARGET_FILE_DIR:realtime_data>"
        )
    endif()
    
    # 复制HDF5 DLL
    if(ENABLE_HDF5 AND DEFINED HDF5_ROOT AND EXISTS "${HDF5_ROOT}/bin")
        file(GLOB HDF5_RUNTIME_DLLS "${HDF5_ROOT}/bin/*.dll")
        if(HDF5_RUNTIME_DLLS)
            add_custom_command(TARGET realtime_data POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${HDF5_RUNTIME_DLLS} "$<TARGET_FILE_DIR:realtime_data>"
            )
        endif()
    endif()
    
    # 复制gRPC DLL
    if(ENABLE_GRPC AND DEFINED GRPC_ROOT AND EXISTS "${GRPC_ROOT}/bin")
        file(GLOB GRPC_RUNTIME_DLLS "${GRPC_ROOT}/bin/*.dll")
        if(GRPC_RUNTIME_DLLS)
            add_custom_command(TARGET realtime_data POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${GRPC_RUNTIME_DLLS} "$<TARGET_FILE_DIR:realtime_data>"
            )
        endif()
    endif()
endif()

include(CTest)
option(BUILD_TESTS "Build stage automation tests (Qt Test + gRPC)" ON)
if(BUILD_TESTS AND ENABLE_GRPC AND NOT ENABLE_WASM)
  add_subdirectory(tests)
endif()

# 打印配置摘要
message(STATUS "==========================================")
message(STATUS "DeviceReceiver CMake 配置")
message(STATUS "  Qt路径: ${CMAKE_PREFIX_PATH}")
message(STATUS "  构建类型: ${CMAKE_BUILD_TYPE}")
message(STATUS "  HDF5支持: ${ENABLE_HDF5}")
if(ENABLE_HDF5 AND DEFINED HDF5_ROOT)
    message(STATUS "  HDF5根目录: ${HDF5_ROOT}")
endif()
message(STATUS "  gRPC支持: ${ENABLE_GRPC}")
if(ENABLE_GRPC AND DEFINED GRPC_ROOT)
    message(STATUS "  gRPC根目录: ${GRPC_ROOT}")
endif()
message(STATUS "  WASM构建: ${ENABLE_WASM}")
message(STATUS "  输出目录:")
message(STATUS "    Debug: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG}")
message(STATUS "    Release: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE}")
message(STATUS "==========================================")