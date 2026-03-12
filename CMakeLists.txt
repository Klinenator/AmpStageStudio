cmake_minimum_required(VERSION 3.16)
project(AmpStageStudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(tube_stage_test
  tools/tube_stage_test.cpp
)

target_include_directories(tube_stage_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
)

set(NAM_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../NeuralAmpModelerCore" CACHE PATH
  "Path to the NeuralAmpModelerCore checkout")

find_path(NLOHMANN_JSON_INCLUDE_ROOT
  nlohmann/json.hpp
)

if(EXISTS "${NAM_CORE_DIR}/NAM/get_dsp.h" AND NLOHMANN_JSON_INCLUDE_ROOT)
  file(GLOB NAM_CORE_SOURCES
    "${NAM_CORE_DIR}/NAM/*.cpp"
    "${NAM_CORE_DIR}/Dependencies/AudioDSPTools/dsp/wav.cpp"
  )

  add_executable(nam_offline_render
    tools/nam_offline_render.cpp
    ${NAM_CORE_SOURCES}
  )

  target_include_directories(nam_offline_render PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${NAM_CORE_DIR}"
    "${NAM_CORE_DIR}/Dependencies/AudioDSPTools"
    "${NAM_CORE_DIR}/Dependencies/eigen"
    "${NLOHMANN_JSON_INCLUDE_ROOT}"
    "${NLOHMANN_JSON_INCLUDE_ROOT}/nlohmann"
  )

  target_compile_definitions(nam_offline_render PRIVATE
    NAM_SAMPLE_FLOAT
  )
else()
  message(STATUS "NeuralAmpModelerCore or nlohmann/json not found; nam_offline_render target will not be built")
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PORTAUDIO QUIET IMPORTED_TARGET portaudio-2.0)
endif()

if(TARGET PkgConfig::PORTAUDIO)
  add_executable(amp_stage_live
    tools/amp_stage_live.cpp
  )

  target_include_directories(amp_stage_live PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
  )

  target_link_libraries(amp_stage_live PRIVATE
    PkgConfig::PORTAUDIO
  )
else()
  message(STATUS "PortAudio not found; amp_stage_live target will not be built")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(tube_stage_test PRIVATE
    -O3
    -Wall
    -Wextra
    -Wpedantic
  )

  if(TARGET amp_stage_live)
    target_compile_options(amp_stage_live PRIVATE
      -O3
      -Wall
      -Wextra
      -Wpedantic
    )
  endif()

  if(TARGET nam_offline_render)
    target_compile_options(nam_offline_render PRIVATE
      -O3
      -Wall
      -Wextra
      -Wpedantic
    )
  endif()
endif()
