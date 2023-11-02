cmake_minimum_required(VERSION 3.0)
project(usb_rt_driver)
include(GNUInstallDirs)

SET(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_CONTACT "Lee Magnusson")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "dkms, udev")
set(CPACK_PACKAGE_VERSION 0.7.3)
set(VERSION ${CPACK_PACKAGE_VERSION})
EXECUTE_PROCESS( COMMAND uname -m COMMAND tr -d '\n' OUTPUT_VARIABLE ARCH)
set(CPACK_SYSTEM_NAME ${ARCH})
SET(CPACK_OUTPUT_FILE_PREFIX packages/)
configure_file(usb_rt_version.h.in usb_rt_version.h)
configure_file(prerm.in prerm)
configure_file(postinst.in postinst)
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA 
    postinst
    prerm)
INCLUDE(CPack)

set (CMAKE_CXX_STANDARD 11)

install(FILES 99-usb_rt_driver.rules
    DESTINATION /etc/udev/rules.d)

execute_process(COMMAND uname -r
    OUTPUT_VARIABLE os_version
    OUTPUT_STRIP_TRAILING_WHITESPACE)
set(OS_VERSION ${os_version} CACHE STRING "e.g. uname -r")
option(BUILD_MODULE "Build the module" ON)
set(module_path /lib/modules/${OS_VERSION})
set(module_build_path ${module_path}/build)
if(${BUILD_MODULE})
    add_custom_command(OUTPUT usb_rt.ko
                        COMMAND ${CMAKE_MAKE_PROGRAM} -C ${module_build_path} 
                            M=${CMAKE_CURRENT_BINARY_DIR} src=${CMAKE_CURRENT_SOURCE_DIR}
                            EXTRA_CFLAGS=-I${CMAKE_BINARY_DIR}
                        DEPENDS usb-rt.c Kbuild
                        COMMENT "Building usb_rt.ko")
    add_custom_target(usb_rt ALL DEPENDS usb_rt.ko)
endif()

configure_file(dkms.conf.in dkms.conf)

install(FILES usb-rt.c Kbuild 
        ${CMAKE_BINARY_DIR}/usb_rt_version.h
        ${CMAKE_CURRENT_BINARY_DIR}/dkms.conf DESTINATION /usr/src/usb_rt-${VERSION})