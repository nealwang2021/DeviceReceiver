# 三轴台自动化测试（需 ENABLE_GRPC + Qt5 Test）
if(NOT ENABLE_GRPC OR NOT DEFINED GRPC_ROOT)
  message(STATUS "Tests: skipped (gRPC disabled or GRPC_ROOT unset)")
  return()
endif()

find_package(Qt5 5.15 REQUIRED COMPONENTS Test)
if(NOT ENABLE_WASM)
  find_package(OpenGL REQUIRED)
endif()

set(STAGE_GRPC_TEST_SERVER_PATH "${CMAKE_SOURCE_DIR}/stage_grpc_test_server.py")
string(REPLACE "\\" "/" STAGE_GRPC_TEST_SERVER_PATH "${STAGE_GRPC_TEST_SERVER_PATH}")
set(STAGE_TEST_FIXTURES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
string(REPLACE "\\" "/" STAGE_TEST_FIXTURES_DIR "${STAGE_TEST_FIXTURES_DIR}")

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/stage_test_paths.h.in
  ${CMAKE_BINARY_DIR}/tests/stage_test_paths.h
  @ONLY
)

set(_STAGE_TEST_INCLUDE "${CMAKE_BINARY_DIR}/tests")

# --- tst_stage_integration：仅 StageReceiverBackend ---
add_executable(tst_stage_integration
  "${CMAKE_CURRENT_SOURCE_DIR}/stage_integration_test.cpp"
  "${CMAKE_SOURCE_DIR}/StageReceiverBackend.cpp"
  "${CMAKE_SOURCE_DIR}/IReceiverBackend.h"
  "${CMAKE_SOURCE_DIR}/proto/generated/stage.pb.cc"
  "${CMAKE_SOURCE_DIR}/proto/generated/stage.grpc.pb.cc"
)
set_target_properties(tst_stage_integration PROPERTIES
  WIN32_EXECUTABLE FALSE
  OUTPUT_NAME "tst_stage_integration_v2"
)
target_include_directories(tst_stage_integration PRIVATE
  "${CMAKE_SOURCE_DIR}"
  "${CMAKE_SOURCE_DIR}/proto/generated"
  "${GRPC_ROOT}/include"
  "${_STAGE_TEST_INCLUDE}"
)
target_link_libraries(tst_stage_integration
  Qt5::Core
  Qt5::Gui
  Qt5::Widgets
  Qt5::Test
)
target_compile_definitions(tst_stage_integration PRIVATE HAS_GRPC QT_USE_UTF8STRING)
if(MSVC)
  target_compile_options(tst_stage_integration PRIVATE /utf-8 /W3)
endif()

if(WIN32)
  target_link_directories(tst_stage_integration PRIVATE ${GRPC_ROOT}/lib)
  target_link_libraries(tst_stage_integration
    grpc++ grpc gpr upb_base_lib upb_hash_lib upb_json_lib upb_lex_lib upb_mem_lib
    upb_message_lib upb_mini_descriptor_lib upb_mini_table_lib upb_reflection_lib
    upb_textformat_lib upb_wire_lib libupb address_sorting re2 utf8_range utf8_validity
    abseil_dll absl_flags_commandlineflag absl_flags_commandlineflag_internal
    absl_flags_config absl_flags_internal absl_flags_marshalling absl_flags_parse
    absl_flags_private_handle_accessor absl_flags_program_name absl_flags_reflection
    absl_flags_usage absl_flags_usage_internal cares zlib libprotobuf libssl libcrypto
    ws2_32 crypt32
  )
elseif(UNIX AND NOT APPLE)
  target_link_libraries(tst_stage_integration grpc++ grpc protobuf pthread dl)
elseif(APPLE)
  target_link_libraries(tst_stage_integration grpc++ grpc protobuf pthread)
endif()

# --- tst_stage_panel：完整应用（无 main.cpp）---
set(TST_PANEL_ABS "")
foreach(_f IN LISTS ALL_SOURCES)
  if(NOT _f STREQUAL "main.cpp")
    list(APPEND TST_PANEL_ABS "${CMAKE_SOURCE_DIR}/${_f}")
  endif()
endforeach()

add_executable(tst_stage_panel
  "${CMAKE_CURRENT_SOURCE_DIR}/tst_stage_panel.cpp"
  ${TST_PANEL_ABS}
)
target_sources(tst_stage_panel PRIVATE
  "${CMAKE_SOURCE_DIR}/3rd_qcustomplot/3rd_qcustomplot/v2_1/qcustomplot.cpp"
  "${CMAKE_SOURCE_DIR}/3rd_qcustomplot/3rd_qcustomplot/smoothcurve.cpp"
  "${CMAKE_SOURCE_DIR}/proto/generated/device.pb.cc"
  "${CMAKE_SOURCE_DIR}/proto/generated/device.grpc.pb.cc"
  "${CMAKE_SOURCE_DIR}/proto/generated/stage.pb.cc"
  "${CMAKE_SOURCE_DIR}/proto/generated/stage.grpc.pb.cc"
)
set_target_properties(tst_stage_panel PROPERTIES
  WIN32_EXECUTABLE FALSE
  OUTPUT_NAME "tst_stage_panel_v2"
)
target_include_directories(tst_stage_panel PRIVATE
  "${CMAKE_SOURCE_DIR}"
  "${CMAKE_SOURCE_DIR}/proto/generated"
  "${GRPC_ROOT}/include"
  "${CMAKE_SOURCE_DIR}/3rd_qcustomplot/3rd_qcustomplot"
  "${CMAKE_SOURCE_DIR}/3rd_qcustomplot/3rd_qcustomplot/v2_1"
  "${_STAGE_TEST_INCLUDE}"
)
if(NOT ENABLE_WASM)
  target_include_directories(tst_stage_panel PRIVATE
    "${CMAKE_SOURCE_DIR}/3rd_qcustomplot/3rd_qcustomplot/freeglut/include"
  )
endif()
target_link_libraries(tst_stage_panel
  Qt5::Core
  Qt5::Gui
  Qt5::Widgets
  Qt5::Network
  Qt5::PrintSupport
  Qt5::Test
)
if(NOT ENABLE_WASM)
  target_link_libraries(tst_stage_panel Qt5::SerialPort Qt5::OpenGL)
  target_link_libraries(tst_stage_panel OpenGL::GL OpenGL::GLU)
  if(EXISTS "${CMAKE_SOURCE_DIR}/3rd_qcustomplot/3rd_qcustomplot/freeglut/lib/x64/freeglut.lib")
    target_link_libraries(tst_stage_panel
      "${CMAKE_SOURCE_DIR}/3rd_qcustomplot/3rd_qcustomplot/freeglut/lib/x64/freeglut.lib"
    )
  endif()
endif()
target_compile_definitions(tst_stage_panel PRIVATE
  qcustomplot_v2_1
  qcustomplot
  QT_USE_UTF8STRING
  HAS_GRPC
)
if(NOT ENABLE_WASM)
  target_compile_definitions(tst_stage_panel PRIVATE QCUSTOMPLOT_USE_OPENGL)
endif()
if(ENABLE_HDF5 AND DEFINED HDF5_ROOT AND EXISTS "${HDF5_ROOT}/include/hdf5.h")
  target_include_directories(tst_stage_panel PRIVATE ${HDF5_ROOT}/include)
  target_link_directories(tst_stage_panel PRIVATE ${HDF5_ROOT}/lib)
  target_link_libraries(tst_stage_panel hdf5)
  target_compile_definitions(tst_stage_panel PRIVATE HAS_HDF5)
endif()
if(WIN32)
  target_compile_definitions(tst_stage_panel PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
if(MSVC)
  target_compile_options(tst_stage_panel PRIVATE /utf-8 /MP /W3 /wd4819)
endif()
if(WIN32)
  target_link_directories(tst_stage_panel PRIVATE ${GRPC_ROOT}/lib)
  target_link_libraries(tst_stage_panel
    grpc++ grpc gpr upb_base_lib upb_hash_lib upb_json_lib upb_lex_lib upb_mem_lib
    upb_message_lib upb_mini_descriptor_lib upb_mini_table_lib upb_reflection_lib
    upb_textformat_lib upb_wire_lib libupb address_sorting re2 utf8_range utf8_validity
    abseil_dll absl_flags_commandlineflag absl_flags_commandlineflag_internal
    absl_flags_config absl_flags_internal absl_flags_marshalling absl_flags_parse
    absl_flags_private_handle_accessor absl_flags_program_name absl_flags_reflection
    absl_flags_usage absl_flags_usage_internal cares zlib libprotobuf libssl libcrypto
    ws2_32 crypt32
  )
elseif(UNIX AND NOT APPLE)
  target_link_libraries(tst_stage_panel grpc++ grpc protobuf pthread dl)
elseif(APPLE)
  target_link_libraries(tst_stage_panel grpc++ grpc protobuf pthread)
endif()

add_dependencies(tst_stage_integration realtime_data)
add_dependencies(tst_stage_panel realtime_data)

enable_testing()
add_test(NAME StageIntegration COMMAND tst_stage_integration)
add_test(NAME StagePanelUi COMMAND tst_stage_panel)
# 与 realtime_data 同目录以便加载 DLL；勿对集成测试设 QT_QPA_PLATFORM=minimal（在部分环境下会触发 0xC0000409）
set_tests_properties(StageIntegration StagePanelUi PROPERTIES
  WORKING_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE}"
)

# Windows：可执行文件旁必须有 platforms/qwindows.dll，否则启动报
# "no Qt platform plugin could be initialized"（仅复制 Qt*.dll 不够；build_cmake.bat 只对 realtime_data 跑 windeployqt）
if(WIN32)
  get_filename_component(_QT5_ROOT "${Qt5_DIR}/../../.." ABSOLUTE)
  if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND EXISTS "${_QT5_ROOT}/plugins/platforms/qwindowsd.dll")
    set(_QT_QWINDOWS_DLL "${_QT5_ROOT}/plugins/platforms/qwindowsd.dll")
  else()
    set(_QT_QWINDOWS_DLL "${_QT5_ROOT}/plugins/platforms/qwindows.dll")
  endif()
  if(EXISTS "${_QT_QWINDOWS_DLL}")
    foreach(_stage_test_tgt IN ITEMS tst_stage_integration tst_stage_panel)
      add_custom_command(
        TARGET ${_stage_test_tgt}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_stage_test_tgt}>/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_QT_QWINDOWS_DLL}"
          "$<TARGET_FILE_DIR:${_stage_test_tgt}>/platforms"
        COMMENT "Qt platform plugin -> $<TARGET_FILE_DIR:${_stage_test_tgt}>/platforms/"
      )
    endforeach()
  else()
    message(WARNING "Qt platform plugin not found: ${_QT_QWINDOWS_DLL} (set Qt5_DIR / CMAKE_PREFIX_PATH)")
  endif()
endif()

message(STATUS "Tests: tst_stage_integration, tst_stage_panel enabled")
