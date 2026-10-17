# The server logic is a library so the tests can link it directly.
add_library(mtfs_server_core STATIC
  rw_lock.cpp
  lock_manager.cpp
  thread_pool.cpp
  file_store.cpp
  session.cpp
  server.cpp)
target_link_libraries(mtfs_server_core PUBLIC mtfs_auth mtfs_common)

add_executable(mtfs-server main.cpp)
target_link_libraries(mtfs-server PRIVATE mtfs_server_core)
