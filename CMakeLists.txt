cmake_minimum_required(VERSION 3.12 FATAL_ERROR)


# Default to a release build.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "No build type selected; defaulting to Release")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build" FORCE)
endif()

# Extract the version string from upnpglobalvars.h so that it doesn't have to be
# duplicated here.
set(VERSION_REGEX "#define MINIDLNA_VERSION[ \t]+\"([0-9\\.]+)\"")
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/upnpglobalvars.h VERSION_STRING REGEX ${VERSION_REGEX})
string(REGEX REPLACE ${VERSION_REGEX} "\\1" VERSION_STRING "${VERSION_STRING}")

project(minidlna
        LANGUAGES C
        VERSION ${VERSION_STRING})

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/")

set(MINIDLNA_LOG_PATH "/var/log" CACHE PATH "Default log path")
set(MINIDLNA_DB_PATH "/var/cache/minidlna" CACHE PATH "Default DB path")
set(MINIDLNA_OS_NAME "${CMAKE_SYSTEM_NAME}" CACHE STRING "OS Name")
set(MINIDLNA_OS_VERSION "${CMAKE_SYSTEM_VERSION}" CACHE STRING "OS Version")
set(MINIDLNA_OS_URL "http://sourceforge.net/projects/minidlna/" CACHE STRING "OS URL")
set(NETGEAR OFF CACHE BOOL "Enable generic NETGEAR device support")
set(READYNAS OFF CACHE BOOL "Enable NETGEAR ReadyNAS support")
set(TIVO_SUPPORT OFF CACHE BOOL "Enable TiVo support")

if(READYNAS)
    set(NETGEAR ON)
    set(TIVO_SUPPORT ON)
endif()

include(GNUInstallDirs)
include(features)

add_executable(minidlnad)

target_sources(minidlnad PRIVATE
            minidlna.c
            upnphttp.c
            upnpdescgen.c
            upnpsoap.c
            upnpreplyparse.c
            minixml.c
            clients.c
            getifaddr.c
            process.c
            upnpglobalvars.c
            options.c
            minissdp.c
            uuid.c
            upnpevents.c
            sql.c
            utils.c
            metadata.c
            scanner.c
            monitor.c
            tivo_utils.c
            tivo_beacon.c
            tivo_commands.c
            playlist.c
            image_utils.c
            albumart.c
            log.c
            containers.c
            avahi.c
            tagutils/tagutils.c)


if(HAVE_KQUEUE)
    target_sources(minidlnad PRIVATE kqueue.c monitor_kqueue.c)
else()
    target_sources(minidlnad PRIVATE select.c)
endif()

if(HAVE_INOTIFY)
    target_sources(minidlnad PRIVATE monitor_inotify.c)
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/config.h.in config.h)
target_sources(minidlnad PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/config.h)

target_compile_definitions(minidlnad PRIVATE -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE)
target_compile_options(minidlnad PRIVATE -Wall)
target_include_directories(minidlnad PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(minidlnad PRIVATE
        PkgConfig::LIBAV
        PkgConfig::ID3TAG
        PkgConfig::FLAC
        PkgConfig::LIBEXIF
        PkgConfig::LIBOGG
        PkgConfig::LIBOPUS
        PkgConfig::VORBIS
        PkgConfig::VORBISENC
        PkgConfig::VORBISFILE
        SQLite::SQLite3
        JPEG::JPEG
        m)

if(NOT HAVE_ICONV)
    find_package(Iconv REQUIRED)
    target_link_libraries(minidlnad PRIVATE Iconv::Iconv)
endif()

if(TIVO_SUPPORT AND HAVE_AVAHI)
    target_link_libraries(minidlnad PRIVATE PkgConfig::AVAHI)
endif()


install(TARGETS minidlnad DESTINATION ${CMAKE_INSTALL_SBINDIR})
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/minidlnad.8 DESTINATION ${CMAKE_INSTALL_PREFIX}/man/man8)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/minidlna.conf.5 DESTINATION ${CMAKE_INSTALL_PREFIX}/man/man5)
