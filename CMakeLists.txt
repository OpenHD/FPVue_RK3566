cmake_minimum_required(VERSION 2.8)

#set(CMAKE_C_COMPILER "/usr/bin/gcc")

project(fpvue)

set(CMAKE_CXX_STANDARD 17)

add_compile_options("-Wno-address-of-packed-member")

set(SOURCE_FILES drm.c
				osd.c
				rtp.c
				mavlink.c
        main.cpp
        copy_util.h
        SchedulingHelper.hpp
        gstrtpreceiver.cpp
        gstrtpreceiver.h
        #
        memcpymove-v7l.S
)

include_directories("/usr/include/libdrm" "/usr/include/cairo")

add_executable(${PROJECT_NAME} ${SOURCE_FILES})
target_link_libraries(${PROJECT_NAME} rockchip_mpp pthread drm m cairo)

# experiment gstreamer
find_package(PkgConfig REQUIRED)
pkg_search_module(GST REQUIRED
        gstreamer-1.0>=1.4
        gstreamer-app-1.0>=1.4
)
pkg_search_module(gstreamer REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.4)
pkg_search_module(gstreamer-app REQUIRED IMPORTED_TARGET gstreamer-app-1.0>=1.4)
target_link_libraries(${PROJECT_NAME} PkgConfig::gstreamer PkgConfig::gstreamer-app)

if(CMAKE_BUILD_TYPE MATCHES "Debug")
  set(
    CMAKE_C_FLAGS
    "${CMAKE_C_FLAGS} -Werror -fsanitize=undefined -fsanitize=address"
  )
  target_link_options(${PROJECT_NAME}
    BEFORE PUBLIC -fsanitize=undefined PUBLIC -fsanitize=address
  )
endif()

include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
