include(CheckCXXCompilerFlag)

function(env_set var_name default_value type docstring)
  set(val ${default_value})
  if(DEFINED ENV{${var_name}})
    set(val $ENV{${var_name}})
  endif()
  set(${var_name} ${val} CACHE ${type} "${docstring}")
endfunction()

function(default_linker var_name)
  if(APPLE)
    set("${var_name}" "DEFAULT" PARENT_SCOPE)
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    find_program(lld_path ld.lld "Path to LLD - is only used to determine default linker")
    if(lld_path)
      set("${var_name}" "LLD" PARENT_SCOPE)
    else()
      set("${var_name}" "DEFAULT" PARENT_SCOPE)
    endif()
  else()
    set("${var_name}" "DEFAULT" PARENT_SCOPE)
  endif()
endfunction()

function(use_libcxx out)
  if(APPLE OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set("${out}" ON PARENT_SCOPE)
  else()
    set("${out}" OFF PARENT_SCOPE)
  endif()
endfunction()

function(static_link_libcxx out)
  if(USE_TSAN)
    # Libc/libstdc++ static linking is not supported for tsan
    set("${out}" OFF PARENT_SCOPE)
  elseif(APPLE)
    set("${out}" OFF PARENT_SCOPE)
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    default_linker(linker)
    if(NOT linker STREQUAL "LLD")
      set("${out}" OFF PARENT_SCOPE)
      return()
    endif()
    find_library(libcxx_a libc++.a)
    find_library(libcxx_abi libc++abi.a)
    if(libcxx_a AND libcxx_abi)
      set("${out}" ON PARENT_SCOPE)
    else()
      set("${out}" OFF PARENT_SCOPE)
    endif()
  else()
    set("${out}" ON PARENT_SCOPE)
  endif()
endfunction()

function(check_swift_source_compiles SOURCE VAR)
  file(WRITE "${CMAKE_BINARY_DIR}/CMakeTmp/src.swift" "${SOURCE}")
  try_compile(build_result "${CMAKE_BINARY_DIR}/CMakeTmp" "${CMAKE_BINARY_DIR}/CMakeTmp/src.swift")
  set(${VAR} ${build_result} PARENT_SCOPE)
endfunction()
