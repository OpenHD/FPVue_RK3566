cmake_minimum_required(VERSION 2.8)

#set(CMAKE_C_COMPILER "/usr/bin/gcc")

project(fpvue)

set(CMAKE_CXX_STANDARD 17)

include(CheckIncludeFileCXX)

add_compile_options("-Wno-address-of-packed-member")
add_compile_options("-Wno-address-of-packed-member")

SET(CMAKE_ASM_FLAGS "${CFLAGS} -x assembler-with-cpp")

include(GNUInstallDirs)

set(SOURCE_FILES drm.c
                                rtp.c
        main.cpp
        copy_util.h
        SchedulingHelper.hpp
        gstrtpreceiver.cpp
        gstrtpreceiver.h
        allwinnerv4l2display.cpp
        allwinnerv4l2display.h
        display_host.cpp
        #
        assembly/memcpymove-v7l.S
        parse_x20_util.h
)

add_executable(${PROJECT_NAME} ${SOURCE_FILES})

find_library(ROCKCHIP_MPP_LIB rockchip_mpp)
if(ROCKCHIP_MPP_LIB)
  message(STATUS "Building with Rockchip MPP")
  target_link_libraries(${PROJECT_NAME} ${ROCKCHIP_MPP_LIB})
else()
  message(STATUS "Rockchip MPP not found: building without Rockchip support")
endif()

target_link_libraries(${PROJECT_NAME} pthread m ) #-lstringlib

check_include_file_cxx("OMX_Core.h" HAVE_OMX_CORE_H)
if(HAVE_OMX_CORE_H)
  message(STATUS "Detected OMX_Core.h - enabling Cedar OpenMAX IL support")
  target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_AW_OMX=1)
  find_library(OMX_CORE_LIB OmxCore)
  find_library(AW_OMXIL_LIB aw_omxil)
  find_library(CEDARC_LIB cedarc)
  find_library(ION_LIB ion)
  set(CEDAR_LIBS ${OMX_CORE_LIB} ${AW_OMXIL_LIB} ${CEDARC_LIB} ${ION_LIB})
  list(REMOVE_ITEM CEDAR_LIBS "CEDAR_LIBS-NOTFOUND")
  if(NOT OMX_CORE_LIB OR NOT AW_OMXIL_LIB OR NOT CEDARC_LIB)
    message(WARNING "Missing Cedar OpenMAX IL libraries (OmxCore/aw_omxil/cedarc). Cedar path will be disabled.")
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_AW_OMX=0)
  else()
    target_link_libraries(${PROJECT_NAME} ${OMX_CORE_LIB} ${AW_OMXIL_LIB} ${CEDARC_LIB})
    if(ION_LIB)
      target_link_libraries(${PROJECT_NAME} ${ION_LIB})
    endif()
  endif()
else()
  message(STATUS "OMX_Core.h not found - Cedar OpenMAX IL support disabled")
  target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_AW_OMX=0)
endif()

# experiment gstreamer
find_package(PkgConfig REQUIRED)
pkg_search_module(GST REQUIRED
        gstreamer-1.0>=1.4
        gstreamer-app-1.0>=1.4
)
pkg_search_module(gstreamer REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.4)
pkg_search_module(gstreamer-app REQUIRED IMPORTED_TARGET gstreamer-app-1.0>=1.4)
target_link_libraries(${PROJECT_NAME} PkgConfig::gstreamer PkgConfig::gstreamer-app)

pkg_check_modules(LIBDRM REQUIRED libdrm)
message(STATUS "LIBDRM_LINK_LIBRARIES=${LIBDRM_LINK_LIBRARIES}")
include_directories(${LIBDRM_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} ${LIBDRM_LIBRARIES})

add_executable(display_host display_host_main.cpp display_host.cpp drm.c)
target_link_libraries(display_host pthread m ${LIBDRM_LIBRARIES})
target_compile_definitions(display_host PRIVATE DEFAULT_PRELOAD_PATH="${CMAKE_INSTALL_FULL_LIBDIR}/libdrm_fd_preload.so")

add_library(drm_fd_preload SHARED drm_fd_preload.cpp display_host.cpp)
target_link_libraries(drm_fd_preload pthread ${LIBDRM_LIBRARIES})


if(CMAKE_BUILD_TYPE MATCHES "Debug")
  set(
    CMAKE_C_FLAGS
    "${CMAKE_C_FLAGS} -Werror -fsanitize=undefined -fsanitize=address"
  )
  set(
    CMAKE_CXX_FLAGS
    "${CMAKE_CXX_FLAGS} -fsanitize=undefined -fsanitize=address"
  )
  target_link_options(${PROJECT_NAME}
    BEFORE PUBLIC -fsanitize=undefined PUBLIC -fsanitize=address
  )
  target_link_options(display_host
    BEFORE PUBLIC -fsanitize=undefined PUBLIC -fsanitize=address
  )
endif()

install(TARGETS ${PROJECT_NAME} display_host drm_fd_preload
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
