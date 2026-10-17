# Pieces shared by the server, the client and the tools: fd/socket RAII,
# synchronization wrappers, logging and the wire protocol.
add_library(mtfs_common STATIC
  fd.cpp
  log.cpp
  net.cpp
  protocol.cpp
  terminal.cpp)
target_include_directories(mtfs_common PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(mtfs_common PUBLIC Threads::Threads mtfs_options)
