add_library(mtfs_client STATIC client.cpp)
target_link_libraries(mtfs_client PUBLIC mtfs_common)

add_executable(mtfs-client main.cpp)
target_link_libraries(mtfs-client PRIVATE mtfs_client)
