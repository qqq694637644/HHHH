cmake_minimum_required(VERSION 3.15...3.30)
project(HHHH LANGUAGES C CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
  # /EHsc set per target
  string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  # Default /O2 /Ob2 collides with target optimization (D9025)
  string(REPLACE "/O2" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
  string(REPLACE "/Ob2" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
  # CMP0092 NEW drops /W3 from defaults
  add_compile_options(/W3)
endif()

add_library(Screen2Zlib STATIC
  "参考/ScreenPlugin2/zlib/include/adler32.c"
  "参考/ScreenPlugin2/zlib/include/compress.c"
  "参考/ScreenPlugin2/zlib/include/crc32.c"
  "参考/ScreenPlugin2/zlib/include/deflate.c"
  "参考/ScreenPlugin2/zlib/include/gzclose.c"
  "参考/ScreenPlugin2/zlib/include/gzlib.c"
  "参考/ScreenPlugin2/zlib/include/gzread.c"
  "参考/ScreenPlugin2/zlib/include/gzwrite.c"
  "参考/ScreenPlugin2/zlib/include/infback.c"
  "参考/ScreenPlugin2/zlib/include/inffast.c"
  "参考/ScreenPlugin2/zlib/include/inflate.c"
  "参考/ScreenPlugin2/zlib/include/inftrees.c"
  "参考/ScreenPlugin2/zlib/include/trees.c"
  "参考/ScreenPlugin2/zlib/include/uncompr.c"
  "参考/ScreenPlugin2/zlib/include/zutil.c"
)
target_include_directories(Screen2Zlib PUBLIC "参考/ScreenPlugin2/zlib/include")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(SCREEN2_JPEG_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/参考/ScreenPlugin2/jpeg/x64")
else()
  set(SCREEN2_JPEG_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/参考/ScreenPlugin2/jpeg/x86")
endif()

add_library(ScreenTransfer STATIC
  common/ScreenTransfer/ScreenCodec.cpp
)
target_include_directories(ScreenTransfer PUBLIC
  common/ScreenTransfer
  "参考"
  "参考/ScreenPlugin2/zlib/include"
)
target_link_libraries(ScreenTransfer PUBLIC
  Screen2Zlib
  "${SCREEN2_JPEG_LIB_DIR}/turbojpeg-static.lib"
  "${SCREEN2_JPEG_LIB_DIR}/jpeg-static.lib"
)
if(MSVC)
  target_compile_options(ScreenTransfer PRIVATE /EHsc)
endif()

add_executable(Server WIN32
  Server/Main.cpp
  Server/Server.cpp
  Server/ControlWindow.cpp
)
target_compile_definitions(Server PRIVATE UNICODE _UNICODE)
target_link_libraries(Server PRIVATE ws2_32 ScreenTransfer)
if(MSVC)
  # <iostream> uses exceptions (C4530)
  target_compile_options(Server PRIVATE
    /EHsc
    $<$<CONFIG:Release>:/O2>
  )
  set_target_properties(Server PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

add_executable(Client
  Client/Main.cpp
  Client/Hd.cpp
  common/Api.cpp
  common/HTTP.cpp
  common/Panel.cpp
  common/Utils.cpp
)
target_link_libraries(Client PRIVATE gdiplus ScreenTransfer)
if(MSVC)
  target_compile_options(Client PRIVATE
    $<$<CONFIG:Release>:/O1 /Ob0 /Oi>
  )
else()
  # -O3 hangs InputThread on first char[N] = { 0 } aggregate init
  target_compile_options(Client PRIVATE
    $<$<CONFIG:Release>:-O1>
  )
endif()
