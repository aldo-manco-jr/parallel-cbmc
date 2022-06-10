set(CMAKE_CXX_STANDARD 14)

set(CMAKE_PREFIX_PATH "/usr/lib/x86_64-linux-gnu/openmpi")

find_package(MPI REQUIRED)

# ----------------
# This is the only thing that made it work
# ----------------
set(MPI_C_LIBRARIES "/usr/lib/x86_64-linux-gnu/openmpi/lib/libmpi.so")
set(MPI_INCLUDE_PATH "/usr/lib/x86_64-linux-gnu/openmpi/include")
# ----------------
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}" -fopenmp)
set(GCC_COVERAGE_COMPILE_FLAGS "-Wall -pedantic -lm -O3 -funroll-loops -fopenmp -Wno-error=switch-enum")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${GCC_COVERAGE_COMPILE_FLAGS}")
add_definitions(-DOMPI_SKIP_MPICXX)

file(GLOB_RECURSE sources "*.cpp" "*.h")
add_library(goto-checker ${sources})

generic_includes(goto-checker)
include_directories(${MPI_INCLUDE_PATH})

target_link_libraries(goto-checker goto-programs goto-symex solvers util xml goto-instrument-lib ${MPI_C_LIBRARIES} MPI::MPI_CXX)