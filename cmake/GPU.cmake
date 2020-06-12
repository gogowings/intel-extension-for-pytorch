## Included by CMakeLists

# ---[ Build flags
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -fPIC")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-narrowing")
# Eigen fails to build with some versions, so convert this to a warning
# Details at http://eigen.tuxfamily.org/bz/show_bug.cgi?id=1459
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wextra")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-missing-field-initializers")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-type-limits")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-array-bounds")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unknown-pragmas")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-sign-compare")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-parameter")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-variable")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-function")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-result")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-strict-overflow")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-strict-aliasing")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error=deprecated-declarations")
if (CMAKE_COMPILER_IS_GNUCXX AND NOT (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 7.0.0))
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-stringop-overflow")
endif()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error=pedantic")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error=redundant-decls")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error=old-style-cast")
# These flags are not available in GCC-4.8.5. Set only when using clang.
# Compared against https://gcc.gnu.org/onlinedocs/gcc-4.8.5/gcc/Option-Summary.html
if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-local-typedef")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-infinite-recursion")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-invalid-partial-specialization")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-typedef-redefinition")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unknown-warning-option")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-private-field")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-inconsistent-missing-override")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-aligned-allocation-unavailable")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-c++14-extensions")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-constexpr-not-const")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-missing-braces")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Qunused-arguments")
  if (${COLORIZE_OUTPUT})
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fcolor-diagnostics")
  endif()
endif()
if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 4.9)
  if (${COLORIZE_OUTPUT})
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fdiagnostics-color=always")
  endif()
endif()
if ((APPLE AND (NOT ("${CLANG_VERSION_STRING}" VERSION_LESS "9.0")))
  OR (CMAKE_COMPILER_IS_GNUCXX
  AND (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 7.0 AND NOT APPLE)))
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -faligned-new")
endif()
if (WERROR)
  check_cxx_compiler_flag("-Werror" COMPILER_SUPPORT_WERROR)
  if (NOT COMPILER_SUPPORT_WERROR)
    set(WERROR FALSE)
  else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror")
  endif()
endif(WERROR)
if (NOT APPLE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-but-set-variable")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-uninitialized")
endif()
set (CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-omit-frame-pointer -O0")
set (CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_STATIC_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer -O0")
set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-math-errno")
set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-trapping-math")

# ---[ Main build

if (DPCPP_ENABLE_PROFILING)
  message(WARNING, "DPCPP profiling is enabled! Only support SYNC execution mode!")
  add_definitions(-DDPCPP_PROFILING)
endif()

# includes
if(DEFINED PYTORCH_INCLUDE_DIR)
  include_directories(${PYTORCH_INCLUDE_DIR})
else()
  message(FATAL_ERROR, "Cannot find installed PyTorch directory")
endif()

set(DPCPP_GPU_ROOT "${TORCH_IPEX_C_SOURCE_DIR}/gpu")
set(DPCPP_GPU_ATEN_SRC_ROOT "${DPCPP_GPU_ROOT}/aten")
set(DPCPP_GPU_ATEN_GENERATED "${DPCPP_GPU_ROOT}/aten/generated")

include_directories(${PYTHON_INCLUDE_DIR})
include_directories(${TORCH_IPEX_C_SOURCE_DIR})
include_directories(${DPCPP_GPU_ROOT})
include_directories(${DPCPP_GPU_ATEN_SRC_ROOT})
include_directories(${DPCPP_GPU_ATEN_GENERATED})

# generate c10 dispatch registration
if (SHOULD_GEN)
  add_custom_target(
    gen_dpcpp_gpu_c10_dispatch_registration
    COMMAND python gen-gpu-decl.py --gpu_decl=./ DPCPPGPUType.h DedicateType.h DispatchStubOverride.h RegistrationDeclarations.h
    COMMAND python gen-gpu-ops.py --output_folder=./ DPCPPGPUType.h RegistrationDeclarations_DPCPP.h Functions_DPCPP.h
    COMMAND cp ./aten_ipex_type_default.cpp.in ${DPCPP_GPU_ATEN_GENERATED}/ATen/aten_ipex_type_default.cpp
    COMMAND cp ./aten_ipex_type_default.h.in ${DPCPP_GPU_ATEN_GENERATED}/ATen/aten_ipex_type_default.h
    COMMAND cp ./aten_ipex_type_dpcpp.h.in ${DPCPP_GPU_ATEN_GENERATED}/ATen/aten_ipex_type_dpcpp.h
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/scripts/gpu
  )
endif()

# sources
set(DPCPP_SRCS)
set(DPCPP_JIT_SRCS)
set(DPCPP_ATEN_SRCS)
add_subdirectory(torch_ipex/csrc/gpu/aten)
list(APPEND DPCPP_SRCS ${DPCPP_ATEN_SRCS})

add_subdirectory(torch_ipex/csrc/gpu/jit)
list(APPEND DPCPP_SRCS ${DPCPP_JIT_SRCS})

add_library(torch_ipex SHARED ${DPCPP_SRCS})

# pytorch library
if(DEFINED PYTORCH_LIBRARY_DIR)
  link_directories(${PYTORCH_LIBRARY_DIR})
  target_link_libraries(torch_ipex PUBLIC ${PYTORCH_LIBRARY_DIR}/libtorch_cpu.so)
  target_link_libraries(torch_ipex PUBLIC ${PYTORCH_LIBRARY_DIR}/libc10.so)
else()
  message(FATAL_ERROR, "Cannot find PyTorch library directory")
endif()

target_link_libraries(torch_ipex PUBLIC ${EXTRA_SHARED_LIBS})

set_target_properties(torch_ipex PROPERTIES PREFIX "")
set_target_properties(torch_ipex PROPERTIES OUTPUT_NAME ${LIB_NAME})

if (SHOULD_GEN)
  add_dependencies(torch_ipex gen_dpcpp_gpu_c10_dispatch_registration)
endif()

find_package(oneDNN QUIET)
if(ONEDNN_FOUND)
  target_link_libraries(torch_ipex PUBLIC dnnl)
  include_directories(BEFORE SYSTEM ${ONEDNN_INCLUDE_DIR})
  add_dependencies(torch_ipex dnnl)
else()
  message(FATAL_ERROR "Cannot find oneDNN")
endif()

find_package(MKLDPCPP QUIET)
if (MKLDPCPP_FOUND)
  # add_dependencies(torch_ipex onemkl)
else()
  message(WARNING "Cannot find oneMKL.")
endif()

include(cmake/DPCPP.cmake)
if(USE_COMPUTECPP)
  include_directories(SYSTEM ${ComputeCpp_INCLUDE_DIRS} ${OpenCL_INCLUDE_DIRS})
  target_link_libraries(torch_ipex PUBLIC ${COMPUTECPP_RUNTIME_LIBRARY})
  add_sycl_to_target(TARGET torch_ipex SOURCES ${DPCPP_SRCS})
  message(STATUS "ComputeCpp found. Compiling with SYCL support")
elseif(USE_DPCPP)
  set_source_files_properties(${DPCPP_SRCS} COMPILE_FLAGS "-fsycl -D__STRICT_ANSI__ -fsycl-unnamed-lambda")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsycl -fsycl-device-code-split=per_source")
  message(STATUS "DPCPP found. Compiling with SYCL support")
else()
  message(FATAL_ERROR "ComputeCpp or DPCPP NOT found. Compiling without SYCL support")
endif()


install(TARGETS ${LIB_NAME}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
