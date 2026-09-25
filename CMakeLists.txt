cmake_minimum_required(VERSION 3.16)
project(token2-fido-bridge LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

# Optional: override the shipped default device identity at configure time, e.g.
#   cmake -B build -S . -DBRIDGE_DEFAULT_PID=0x0200
set(BRIDGE_DEFAULT_VID "" CACHE STRING "Default USB vendor ID (hex, e.g. 0x349E)")
set(BRIDGE_DEFAULT_PID "" CACHE STRING "Default USB product ID (hex, e.g. 0x0001)")

# Statically link the C++/GCC runtimes so one binary runs across distro versions
# without libstdc++ ABI worries. libpcsclite stays dynamic (always present with
# a working reader / pcscd).
option(STATIC_CXX_RUNTIME "Static-link libstdc++ and libgcc" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PCSCLITE REQUIRED IMPORTED_TARGET libpcsclite)

add_executable(token2-fido-bridge src/main.cpp)
target_link_libraries(token2-fido-bridge PRIVATE PkgConfig::PCSCLITE)

if(BRIDGE_DEFAULT_VID)
  target_compile_definitions(token2-fido-bridge PRIVATE BRIDGE_DEFAULT_VID=${BRIDGE_DEFAULT_VID})
endif()
if(BRIDGE_DEFAULT_PID)
  target_compile_definitions(token2-fido-bridge PRIVATE BRIDGE_DEFAULT_PID=${BRIDGE_DEFAULT_PID})
endif()

if(STATIC_CXX_RUNTIME)
  target_link_options(token2-fido-bridge PRIVATE -static-libstdc++ -static-libgcc)
endif()

include(GNUInstallDirs)
install(TARGETS token2-fido-bridge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/token2-fido-bridge.service DESTINATION /usr/lib/systemd/system)
install(FILES packaging/70-token2-fido-bridge.rules DESTINATION /usr/lib/udev/rules.d)
install(FILES packaging/uhid.conf DESTINATION /usr/lib/modules-load.d RENAME token2-fido-bridge-uhid.conf)
