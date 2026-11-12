# Public application API shared by GUI and CLI (module 24/25 backend, ТЗ §12.1.4).
set(AM_APP_DEPS am::foundation am::contracts am::domain am::jobs am::storage
                am::processing am::services am::automation am::plugin_runtime)
if(AM_WITH_AI)
  list(APPEND AM_APP_DEPS am::ai)
endif()
if(AM_WITH_CONTAINER)
  find_package(tomlplusplus CONFIG REQUIRED)
  set(AM_MESSAGES "${PROJECT_SOURCE_DIR}/resources/messages/en.toml")
  file(READ "${AM_MESSAGES}" AM_MESSAGE_TEXT)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${AM_MESSAGES}")
  string(REPLACE "\n" "\n)AMMSG\",\nR\"AMMSG(" AM_MESSAGE_PARTS "${AM_MESSAGE_TEXT}")
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/messages_resource.hpp"
    CONTENT "#pragma once\ninline constexpr const char* message_parts[] = {R\"AMMSG(${AM_MESSAGE_PARTS})AMMSG\"};\n")
  am_add_module(application SOURCES common/asset_service.cpp common/development.cpp common/messages.cpp
    DEPENDS ${AM_APP_DEPS})
  set_property(SOURCE common/messages.cpp APPEND PROPERTY OBJECT_DEPENDS
    "${AM_MESSAGES};${CMAKE_CURRENT_BINARY_DIR}/generated/messages_resource.hpp")
  target_include_directories(am_application PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_link_libraries(am_application PRIVATE tomlplusplus::tomlplusplus)
else()
  am_add_module(application DEPENDS ${AM_APP_DEPS})
endif()

if(AM_BUILD_CLI AND AM_WITH_CONTAINER)
  add_subdirectory(cli)
endif()
if(AM_BUILD_GUI)
  add_subdirectory(gui)               # Qt 6 Widgets (official installer, AM_QT_ROOT)
endif()
