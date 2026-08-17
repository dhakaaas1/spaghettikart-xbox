if(WIN32)
  find_package(Ogg CONFIG REQUIRED)
  find_package(Vorbis CONFIG REQUIRED)
  if(SPAGHETTIKART_UWP)
    # LIBUWP: the CoreWindow/refresh-rate/screen-size glue DLL (see the UwpLibs
    # FetchContent block in the root CMakeLists.txt). WindowsApp: the umbrella
    # WinRT/UWP import lib needed for any Windows.* API surface calls; required
    # for an AppContainer-targeted binary to link at all.
    list(APPEND ADDITIONAL_LIBRARY_DEPENDENCIES LIBUWP WindowsApp)
  endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
  set(ADDITIONAL_LIBRARY_DEPENDENCIES -lglad SDL2::SDL2)
elseif(CMAKE_SYSTEM_NAME STREQUAL "CafeOS")
  set(ADDITIONAL_LIBRARY_DEPENDENCIES "$<$<CONFIG:Debug>:-Wl,--wrap=abort>")
  target_include_directories(${PROJECT_NAME} PRIVATE
                             ${DEVKITPRO}/portlibs/wiiu/include/)
else()
  find_package(Ogg REQUIRED)
  find_package(Vorbis REQUIRED)
endif()

if(NOT CMAKE_SYSTEM_NAME MATCHES "NintendoSwitch|CafeOS")
  set(ADDITIONAL_LIBRARY_DEPENDENCIES Ogg::ogg Vorbis::vorbis
                                      Vorbis::vorbisenc Vorbis::vorbisfile)
endif()

if(UNIX AND NOT APPLE)
  if(USE_OPENGLES)
    find_library(GLESv2_LIBRARY GLESv2 REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${GLESv2_LIBRARY})
  else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE OpenGL::GL)
  endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
  find_package(SDL2)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE ${SDL2_INCLUDE_DIRS})

if(NOT USE_OPENGLES)
  target_include_directories(${PROJECT_NAME} PRIVATE ${GLEW_INCLUDE_DIRS})
endif()

target_link_libraries(${PROJECT_NAME}
                      PRIVATE torch ${ADDITIONAL_LIBRARY_DEPENDENCIES})
