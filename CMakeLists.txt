cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(maniavita C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "maniaVita")
set(VITA_TITLEID  "MAVT00001")
set(VITA_VERSION  "01.00")

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED sdl2 SDL2_mixer SDL2_ttf SDL2_image)

add_executable(${PROJECT_NAME} main.c)
target_include_directories(${PROJECT_NAME} PRIVATE ${DEPS_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME}
  ${DEPS_STATIC_LDFLAGS}
  SceIme_stub
  SceCommonDialog_stub
  SceAppUtil_stub
  ScePower_stub
  z
  m
SceAvPlayer_stub SceSysmodule_stub
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE font.ttf font.ttf
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml

)
