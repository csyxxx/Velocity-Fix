# Install script for directory: D:/velocity-fixed/velocity-fixed-main/cs2/velocity-cs2/vcpkg_installed/vcpkg/blds/lua/src/lua-5-946a01d0af.clean

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "D:/velocity-fixed/velocity-fixed-main/cs2/velocity-cs2/vcpkg_installed/vcpkg/pkgs/lua_x64-windows-static")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "OFF")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE FILE FILES
    "D:/velocity-fixed/velocity-fixed-main/cs2/velocity-cs2/vcpkg_installed/vcpkg/blds/lua/src/lua-5-946a01d0af.clean/src/lualib.h"
    "D:/velocity-fixed/velocity-fixed-main/cs2/velocity-cs2/vcpkg_installed/vcpkg/blds/lua/src/lua-5-946a01d0af.clean/src/lua.h"
    "D:/velocity-fixed/velocity-fixed-main/cs2/velocity-cs2/vcpkg_installed/vcpkg/blds/lua/src/lua-5-946a01d0af.clean/src/luaconf.h"
    "D:/velocity-fixed/velocity-fixed-main/cs2/velocity-cs2/vcpkg_installed/vcpkg/blds/lua/src/lua-5-946a01d0af.clean/src/lauxlib.h"
    "D:/velocity-fixed/velocity-fixed-main/cs2/velocity-cs2/vcpkg_installed/vcpkg/blds/lua/src/lua-5-946a01d0af.clean/src/lua.hpp"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "D:/velocity-fixed/velocity-fixed-main/cs2/velocity-cs2/vcpkg_installed/vcpkg/blds/lua/x64-windows-static-rel/lua.lib")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/unofficial-lua" TYPE FILE FILES "D:/velocity-fixed/velocity-fixed-main/cs2/velocity-cs2/vcpkg_installed/vcpkg/blds/lua/x64-windows-static-rel/unofficial-lua-config-version.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/unofficial-lua/unofficial-lua-config.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/unofficial-lua/unofficial-lua-config.cmake"
         "D:/velocity-fixed/velocity-fixed-main/cs2/velocity-cs2/vcpkg_installed/vcpkg/blds/lua/x64-windows-static-rel/CMakeFiles/Export/0fcbc78383187e0a2ea9733d64a61ce4/unofficial-lua-config.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/unofficial-lua/unofficial-lua-config-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/share/unofficial-lua/unofficial-lua-config.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/unofficial-lua" TYPE FILE FILES "D:/velocity-fixed/velocity-fixed-main/cs2/velocity-cs2/vcpkg_installed/vcpkg/blds/lua/x64-windows-static-rel/CMakeFiles/Export/0fcbc78383187e0a2ea9733d64a61ce4/unofficial-lua-config.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/unofficial-lua" TYPE FILE FILES "D:/velocity-fixed/velocity-fixed-main/cs2/velocity-cs2/vcpkg_installed/vcpkg/blds/lua/x64-windows-static-rel/CMakeFiles/Export/0fcbc78383187e0a2ea9733d64a61ce4/unofficial-lua-config-release.cmake")
  endif()
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "D:/velocity-fixed/velocity-fixed-main/cs2/velocity-cs2/vcpkg_installed/vcpkg/blds/lua/x64-windows-static-rel/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
if(CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_COMPONENT MATCHES "^[a-zA-Z0-9_.+-]+$")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
  else()
    string(MD5 CMAKE_INST_COMP_HASH "${CMAKE_INSTALL_COMPONENT}")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INST_COMP_HASH}.txt")
    unset(CMAKE_INST_COMP_HASH)
  endif()
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "D:/velocity-fixed/velocity-fixed-main/cs2/velocity-cs2/vcpkg_installed/vcpkg/blds/lua/x64-windows-static-rel/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
