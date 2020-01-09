cmake_minimum_required(VERSION 3.10)

project(qotdd)

include_directories(
        ${PROJECT_SOURCE_DIR}/src
)

set(SOURCES src/arguments.c  src/config.c  src/core.c  src/daemon.c  src/journal.c  src/network.c  src/pid_file.c  src/quotes.c  src/security.c  src/signal_hndl.c)

include_directories(src)
add_executable(qotdd ${SOURCES})

