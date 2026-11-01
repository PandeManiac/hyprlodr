cmake_minimum_required(VERSION 3.20)
project(hyprlodr VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
include(CTest)

set(HYPRLODR_WARNINGS -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion)

add_library(hyprlodr_loader STATIC native/src/loader.c native/src/anchors.c)
target_include_directories(hyprlodr_loader PUBLIC native/include)
target_compile_options(hyprlodr_loader PRIVATE ${HYPRLODR_WARNINGS})
if(UNIX AND NOT APPLE)
  target_link_libraries(hyprlodr_loader PUBLIC dl)
endif()

add_library(hyprlodr_feather STATIC native/src/feather_spring.c)
target_include_directories(hyprlodr_feather PUBLIC native/include)
target_compile_options(hyprlodr_feather PRIVATE ${HYPRLODR_WARNINGS})
if(UNIX AND NOT APPLE)
  target_link_libraries(hyprlodr_feather PUBLIC m)
endif()

add_executable(hyprlodr-host native/src/host.c)
target_link_libraries(hyprlodr-host PRIVATE hyprlodr_loader)
target_compile_options(hyprlodr-host PRIVATE ${HYPRLODR_WARNINGS})

# --- Mandatory sample mod suite ------------------------------------------------
set(MOD_IDS z.foundation a.palette b.audio m.overlay 00.final)
set(MOD_SOURCES foundation palette audio overlay final)
list(LENGTH MOD_IDS MOD_COUNT)
math(EXPR MOD_LAST "${MOD_COUNT} - 1")
foreach(I RANGE ${MOD_LAST})
  list(GET MOD_IDS ${I} MOD_ID)
  list(GET MOD_SOURCES ${I} MOD_SOURCE)
  add_library(mod_${MOD_SOURCE} SHARED samples/${MOD_SOURCE}.c)
  target_include_directories(mod_${MOD_SOURCE} PRIVATE native/include)
  target_compile_options(mod_${MOD_SOURCE} PRIVATE ${HYPRLODR_WARNINGS})
  set_target_properties(mod_${MOD_SOURCE} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${MOD_ID}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mods")
endforeach()

# --- Worked example mod --------------------------------------------------------
# Deliberately outside ${CMAKE_BINARY_DIR}/mods: the sample five are asserted by
# the loader suite, and an extra library there would change those expectations.
add_library(example_uptime SHARED examples/uptime.c)
target_include_directories(example_uptime PRIVATE native/include)
target_compile_options(example_uptime PRIVATE ${HYPRLODR_WARNINGS})
set_target_properties(example_uptime PROPERTIES
  PREFIX ""
  OUTPUT_NAME "example.uptime"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples")

# The first data-reading example: proves ABI 5 image access as a mod author would
# use it, and is the only example that refuses to run against an unknown build.
add_library(example_game_info SHARED examples/game_info.c)
target_include_directories(example_game_info PRIVATE native/include)
target_compile_options(example_game_info PRIVATE ${HYPRLODR_WARNINGS})
set_target_properties(example_game_info PROPERTIES
  PREFIX ""
  OUTPUT_NAME "example.game-info"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples")

# --- Error-case fixtures -------------------------------------------------------
# Each fixture is the same source compiled with different identity macros.
function(hyprlodr_fixture name)
  add_library(fix_${name} SHARED native/tests/fixtures/plugin.c)
  target_include_directories(fix_${name} PRIVATE native/include)
  target_compile_options(fix_${name} PRIVATE ${HYPRLODR_WARNINGS})
  target_compile_definitions(fix_${name} PRIVATE ${ARGN})
  set_target_properties(fix_${name} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${name}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fixtures")
endfunction()

# Dependency ordering and environment errors
hyprlodr_fixture(tie_alpha FIX_ID="tie.alpha")
hyprlodr_fixture(tie_beta FIX_ID="tie.beta")
hyprlodr_fixture(tie_gamma FIX_ID="tie.gamma")

hyprlodr_fixture(diamond_bottom FIX_ID="d.bottom" FIX_CAP="test.d.bottom")
hyprlodr_fixture(diamond_left FIX_ID="d.left" FIX_DEP1="d.bottom" FIX_CAP="test.d.left")
hyprlodr_fixture(diamond_right FIX_ID="d.right" FIX_DEP1="d.bottom" FIX_CAP="test.d.right")
hyprlodr_fixture(diamond_top FIX_ID="d.top" FIX_DEP1="d.left" FIX_DEP2="d.right" FIX_REQUIRE_CAP="test.d.left")

hyprlodr_fixture(state_writer FIX_ID="k.writer" FIX_STATE_KEY="fixture.state" FIX_STATE_VALUE="written")
hyprlodr_fixture(state_reader FIX_ID="k.reader" FIX_DEP1="k.writer" FIX_REQUIRE_STATE="fixture.state" FIX_REQUIRE_VALUE="written")

# A failing mod removes a key another mod set. Rollback must put it back, which
# the keeper verifies through the public ABI when it shuts down after rollback.
hyprlodr_fixture(state_keeper FIX_ID="k.keeper" FIX_STATE_KEY="fixture.remove" FIX_STATE_VALUE="kept"
  FIX_SHUTDOWN_REQUIRE_STATE="fixture.remove" FIX_SHUTDOWN_REQUIRE_VALUE="kept"
  FIX_SHUTDOWN_REQUIRE_ABSENT="fixture.remover")
hyprlodr_fixture(state_remover FIX_ID="k.remover" FIX_DEP1="k.keeper" FIX_STATE_REMOVE="fixture.remove"
  FIX_STATE_KEY="fixture.remover" FIX_STATE_VALUE="transient" FIX_FAIL=1)

hyprlodr_fixture(missing_dep FIX_ID="needs.missing" FIX_DEP1="not.there")
hyprlodr_fixture(cycle_a FIX_ID="cycle.a" FIX_DEP1="cycle.b")
hyprlodr_fixture(cycle_b FIX_ID="cycle.b" FIX_DEP1="cycle.a")
hyprlodr_fixture(self_dep FIX_ID="self.dep" FIX_DEP1="self.dep")
hyprlodr_fixture(dup_one FIX_ID="dup.id" FIX_CAP="test.dup.one")
hyprlodr_fixture(dup_two FIX_ID="dup.id" FIX_CAP="test.dup.two")

hyprlodr_fixture(ver_ok FIX_ID="ver.ok" FIX_DEP1="z.foundation@^1.0.0")
hyprlodr_fixture(ver_old FIX_ID="ver.old" FIX_DEP1="z.foundation@>=2.0.0")
hyprlodr_fixture(conflict_a FIX_ID="conflict.a" FIX_CONFLICT1="conflict.b")
hyprlodr_fixture(conflict_b FIX_ID="conflict.b")

hyprlodr_fixture(abi_old FIX_ID="abi.old" FIX_DESC_ABI=1 FIX_ALWAYS_ANSWER=1)
hyprlodr_fixture(tiny_descriptor FIX_ID="tiny.descriptor" FIX_DESC_SIZE=16)
hyprlodr_fixture(bad_descriptor FIX_ID="bad.descriptor" FIX_BAD_DESCRIPTOR=1)
hyprlodr_fixture(no_entry FIX_ID="no.entry" FIX_NO_ENTRY=1)

hyprlodr_fixture(early_ok FIX_ID="a.early")
hyprlodr_fixture(failer FIX_ID="g.failer" FIX_FAIL=1 FIX_STATE_KEY="fixture.leak" FIX_STATE_VALUE="leaked" FIX_CAP="test.failer")
hyprlodr_fixture(dependent_of_failer FIX_ID="a.dependent" FIX_DEP1="g.failer" FIX_REQUIRE_CAP="test.failer")
hyprlodr_fixture(residue_check FIX_ID="z.residue" FIX_DEP1="g.failer")
hyprlodr_fixture(plain_a FIX_ID="plain.a")

# ABI 3 per-mod state directories
hyprlodr_fixture(dir_writer_a FIX_ID="p.writer" FIX_STATE_FILE="probe.txt")
hyprlodr_fixture(dir_writer_b FIX_ID="p.writer2" FIX_STATE_FILE="probe.txt")
hyprlodr_fixture(state_counter FIX_ID="p.counter" FIX_STATE_COUNTER="counter.txt")

# ABI 4 file redirection: one mod that registers successfully and one that
# registers then fails, so the rollback path is exercised as well.
hyprlodr_fixture(redirect_mod FIX_ID="r.redirect"
  FIX_REDIRECT_FROM="asset/asset" FIX_REDIRECT_TO="C:/hyprlodr/replacement.bin")
hyprlodr_fixture(redirect_fail FIX_ID="r.failing"
  FIX_REDIRECT_FROM="shader/x.glsl" FIX_REDIRECT_TO="C:/hyprlodr/other.bin" FIX_FAIL=1)

# ABI 5 read-only image access: one read inside the fixture image, one past its end
hyprlodr_fixture(image_reader FIX_ID="i.reader" FIX_IMAGE_READ=16)
hyprlodr_fixture(image_reader_far FIX_ID="i.far" FIX_IMAGE_READ=4096)

# ABI 6 writes: one patch inside the fixture image, one past its end, and one
# patch made by a mod that then fails (rollback must restore it).
hyprlodr_fixture(image_writer FIX_ID="i.writer" FIX_IMAGE_WRITE=16)
hyprlodr_fixture(image_writer_far FIX_ID="i.wfar" FIX_IMAGE_WRITE=4096)
hyprlodr_fixture(image_writer_fail FIX_ID="i.wfail" FIX_IMAGE_WRITE=16 FIX_FAIL=1)

# ABI 7 named locations: resolvable, unknown, and resolvable-but-tampered.
hyprlodr_fixture(symbol_probe FIX_ID="s.probe" FIX_SYMBOL="probe")
hyprlodr_fixture(symbol_missing FIX_ID="s.missing" FIX_SYMBOL="missing")
hyprlodr_fixture(symbol_stale FIX_ID="s.stale" FIX_SYMBOL="probe")

if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)

  # Runtime build fingerprints: the verifier that guards address-dependent work.
  add_executable(anchor_test native/tests/anchor_test.c)
  target_link_libraries(anchor_test PRIVATE hyprlodr_loader)
  target_compile_options(anchor_test PRIVATE ${HYPRLODR_WARNINGS})
  add_test(NAME anchor_test COMMAND anchor_test)

  add_executable(feather_spring_test native/tests/feather_spring_test.c)
  target_link_libraries(feather_spring_test PRIVATE hyprlodr_feather)
  target_compile_options(feather_spring_test PRIVATE ${HYPRLODR_WARNINGS})
  add_test(NAME feather_spring_test COMMAND feather_spring_test)

  # Asset container / cipher probe: proves the implementation round-trips a
  # synthetic archive. The real archive check is a manual, evidence-producing
  # run (python3 tools/re/asset_probe.py --exe ... --asset ...).
  add_test(NAME asset_probe_suite
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/re/asset_probe.py --selftest)

  add_test(NAME loader_suite
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/native/tests/loader_suite.py
      $<TARGET_FILE:hyprlodr-host>
      ${CMAKE_BINARY_DIR}/mods
      ${CMAKE_BINARY_DIR}/fixtures
      $<TARGET_FILE_DIR:example_game_info>)
  add_test(NAME package_suite
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/tests/test_package.py
      $<TARGET_FILE:hyprlodr-host>
      ${CMAKE_BINARY_DIR}/mods/z.foundation${CMAKE_SHARED_LIBRARY_SUFFIX})

  # Installers must be non-destructive and must roll back failed installs.
  add_test(NAME install_suite
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/tests/test_install.py)

  # Desktop manager core services (headless; skipped when the .NET SDK is absent).
  find_program(DOTNET_EXECUTABLE dotnet)
  if(DOTNET_EXECUTABLE)
    add_test(NAME manager_suite
      COMMAND ${CMAKE_SOURCE_DIR}/tools/test_manager.sh
        $<TARGET_FILE_DIR:mod_foundation>/z.foundation${CMAKE_SHARED_LIBRARY_SUFFIX})
  endif()
endif()
