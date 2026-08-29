cmake_minimum_required(VERSION 3.20)
project(aimp_ncm LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "AIMP plugin only supports Windows")
endif()

# Source files
# RC 编译: rc.exe 无法解析含中文的路径（会把路径截断导致失败），
# 因此将 .rc 复制到英文临时目录再编译。
file(GLOB SRC "src/*.cpp" "src/*.h")

# 生成一个英文路径的 RC 副本（每次 configure 时同步）
# 注意: TEMP 通常形如 C:\Users\...，直接拼进引号字符串会因 \U 等非法转义
# 导致 CMake 解析失败，必须先用 file(TO_CMAKE_PATH) 转成正斜杠路径。
file(TO_CMAKE_PATH "$ENV{TEMP}" NCM_RC_STAGE)
set(NCM_RC_STAGE "${NCM_RC_STAGE}/aimp_ncm_rc_stage")
file(MAKE_DIRECTORY "${NCM_RC_STAGE}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/plugin.rc" "${NCM_RC_STAGE}/plugin.rc" COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/resource.h" "${NCM_RC_STAGE}/resource.h" COPYONLY)

add_library(aimp_ncm SHARED ${SRC} "${NCM_RC_STAGE}/plugin.rc" "${CMAKE_CURRENT_SOURCE_DIR}/plugin.def")
if(MSVC)
  set_target_properties(aimp_ncm PROPERTIES LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/plugin.def\"")
endif()

target_include_directories(aimp_ncm PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/aimp_sdk
)

target_link_libraries(aimp_ncm PRIVATE
  winhttp
  bcrypt
  crypt32
  shlwapi
  comctl32
  ws2_32
)

set_target_properties(aimp_ncm PROPERTIES
  OUTPUT_NAME "aimp_ncm"
  SUFFIX ".dll"
)

target_compile_definitions(aimp_ncm PRIVATE UNICODE _UNICODE NOMINMAX)

if(MSVC)
  target_compile_options(aimp_ncm PRIVATE /W3 /EHsc /utf-8)
endif()

install(TARGETS aimp_ncm DESTINATION .)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../README.md" DESTINATION . OPTIONAL)
