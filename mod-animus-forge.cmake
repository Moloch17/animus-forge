# Included by modules/CMakeLists.txt (modules/<name>/<name>.cmake).
#
# The curriculum, the environment, the encoders, the bots and the model all live in src/ now. They were animus-lib,
# a library shared with mod-animus and carried here as a git subtree; mod-animus keeps its own copy, and this module
# no longer has a dependency to resolve, a bundle to refresh, or a second loader to call.
#
# Installing also creates etc/modules/mod_animus_forge.conf from its .dist when there is none: AzerothCore reads a
# module's settings from the .conf only, and without one every AnimusForge key logs "Missing property" and keeps its
# default. An existing .conf is never overwritten.

ModuleNameToVariable(mod-animus-forge ANIMUS_FORGE_LINKAGE_VARIABLE)
if(NOT "${${ANIMUS_FORGE_LINKAGE_VARIABLE}}" MATCHES "static|dynamic")
  return()
endif()

# Where the core installs module configs (CopyModuleConfig, src/cmake/macros/ConfigInstall.cmake).
if(WIN32)
  set(ANIMUS_MODULE_CONF_DIR "${CMAKE_INSTALL_PREFIX}/configs/modules")
else()
  set(ANIMUS_MODULE_CONF_DIR "${CONF_DIR}/modules")
endif()
install(CODE "
  set(animusConf \"\$ENV{DESTDIR}${ANIMUS_MODULE_CONF_DIR}/mod_animus_forge.conf\")
  if(NOT EXISTS \"\${animusConf}\")
    message(STATUS \"Creating: \${animusConf}\")
    configure_file(\"${CMAKE_CURRENT_LIST_DIR}/conf/mod_animus_forge.conf.dist\" \"\${animusConf}\" COPYONLY)
  endif()")

# The policy's forward pass (MlpPolicy::Decide) is a dot product per row, and its accumulator is a serial
# floating-point dependency chain. Float addition is not associative, so without permission to reorder it no
# compiler will vectorise the reduction, and every decision costs about three times what it should: measured on a
# real model, 310 us a decision at -O2 against 94 us with these flags, and 60 us where AVX2 is also allowed.
#
# The flags are scoped to this one file and are deliberately NOT -ffast-math: nothing here assumes the absence of
# NaN or infinity, only that float addition may be reordered. Verified on five shipped models over 4,000 decisions
# each -- every decision identical before and after.
#
# -O3 is needed as well: at -O2 the vectoriser's cost model declines this loop even with the flags.
set(ANIMUS_FORWARD_PASS "${CMAKE_CURRENT_LIST_DIR}/src/Model/MlpPolicy.cpp")
if(EXISTS "${ANIMUS_FORWARD_PASS}")
  if(MSVC)
    set(ANIMUS_FORWARD_PASS_FLAGS /O2 /fp:fast)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(ANIMUS_FORWARD_PASS_FLAGS -O3 -fassociative-math -fno-signed-zeros -fno-trapping-math)
  endif()

  if(ANIMUS_FORWARD_PASS_FLAGS)
    # No DIRECTORY argument: a <module>.cmake is included from modules/CMakeLists.txt, in the scope where the
    # `modules` target is defined, which is the scope this property has to land in.
    set_source_files_properties("${ANIMUS_FORWARD_PASS}" PROPERTIES COMPILE_OPTIONS "${ANIMUS_FORWARD_PASS_FLAGS}")
    message(STATUS "  mod-animus-forge: MlpPolicy built with the forward pass vectorised")
  endif()
endif()
