cmake_minimum_required(VERSION 3.27)

set(CMAKE_OSX_DEPLOYMENT_TARGET "27.0")
set(CMAKE_OSX_ARCHITECTURES "arm64")

project(bar-metal LANGUAGES C CXX OBJCXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 23)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

find_library(BAR_METAL Metal REQUIRED)
find_library(BAR_APPKIT AppKit REQUIRED)
find_library(BAR_QUARTZ QuartzCore REQUIRED)
find_library(BAR_FOUNDATION Foundation REQUIRED)
find_library(BAR_CORETEXT CoreText REQUIRED)
find_library(BAR_COREGRAPHICS CoreGraphics REQUIRED)
find_library(BAR_Z z REQUIRED)

set(LUA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua-5.1.5/src)
file(GLOB LUA_SOURCES ${LUA_DIR}/*.c)
list(REMOVE_ITEM LUA_SOURCES ${LUA_DIR}/lua.c ${LUA_DIR}/luac.c ${LUA_DIR}/print.c)

add_library(lua51 STATIC ${LUA_SOURCES})
target_include_directories(lua51 PUBLIC ${LUA_DIR})
target_compile_definitions(lua51 PUBLIC LUA_USE_MACOSX)

add_executable(lua51_cli ${LUA_DIR}/lua.c)
target_link_libraries(lua51_cli PRIVATE lua51)
set_target_properties(lua51_cli PROPERTIES OUTPUT_NAME lua51)

add_executable(bar-metal
  shell/main.mm
  rhi/metal4/TriangleRenderer.mm
  rhi/metal4/TerrainRenderer.mm
  lua/game_name.cpp
  lua/gadget_host.cpp
  vfs/archive.cpp
  map/startscript.cpp
  map/camera.cpp
  map/smf.cpp
  map/bookmarks.cpp
  vfs/pool.cpp
  defs/unitdef.cpp
  model/s3o.cpp
  sim/spawn.cpp
  sim/path/astar.cpp
  sim/move/unit_sim.cpp
  sim/command/orders.cpp
  sim/anim/unit_anim.cpp
  sim/anim/cob.cpp
  sim/economy/team_resources.cpp
  sim/economy/metal_extract.cpp
  sim/build/nanoframe.cpp
  sim/build/reclaim.cpp
  sim/build/death_wreck.cpp
  sim/weapon/laser.cpp
  sim/projectile/laser_bolt.cpp
  sim/weapon/cannon.cpp
  sim/projectile/cannon_shell.cpp
  sim/weapon/beam.cpp
  sim/weapon/missile.cpp
  sim/projectile/missile_rocket.cpp
  sim/weapon/lightning.cpp
  sim/weapon/flame.cpp
  sim/weapon/torpedo.cpp
  sim/weapon/bomb.cpp
  sim/weapon/dgun.cpp
  sim/los/los.cpp
  lua/gl/gl_api.cpp
  ui/widget_host.cpp
)
target_include_directories(bar-metal PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/rhi/metal4
  ${CMAKE_CURRENT_SOURCE_DIR}/lua
  ${CMAKE_CURRENT_SOURCE_DIR}/vfs
  ${CMAKE_CURRENT_SOURCE_DIR}/map
  ${CMAKE_CURRENT_SOURCE_DIR}/defs
  ${CMAKE_CURRENT_SOURCE_DIR}/model
  ${CMAKE_CURRENT_SOURCE_DIR}/sim
  ${CMAKE_CURRENT_SOURCE_DIR}/ui
)
target_compile_definitions(bar-metal PRIVATE
  "BAR_GAME_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/..\""
  "BAR_SHADER_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.metal\""
  "BAR_TERRAIN_SHADER=\"${CMAKE_CURRENT_SOURCE_DIR}/shaders/terrain.metal\""
)
target_compile_options(bar-metal PRIVATE -fobjc-arc)
target_link_libraries(bar-metal PRIVATE
  lua51
  ${BAR_Z}
  ${BAR_METAL}
  ${BAR_APPKIT}
  ${BAR_QUARTZ}
  ${BAR_FOUNDATION}
  ${BAR_CORETEXT}
  ${BAR_COREGRAPHICS}
)

add_custom_command(TARGET bar-metal POST_BUILD
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_linkage.sh $<TARGET_FILE:bar-metal>
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/bar-metal.app/Contents/MacOS
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:bar-metal> ${CMAKE_CURRENT_BINARY_DIR}/bar-metal.app/Contents/MacOS/bar-metal
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/app/Info.plist ${CMAKE_CURRENT_BINARY_DIR}/bar-metal.app/Contents/Info.plist
  COMMAND codesign --force --sign - ${CMAKE_CURRENT_BINARY_DIR}/bar-metal.app
  VERBATIM
)

add_executable(gate tools/gate/main.cpp)
