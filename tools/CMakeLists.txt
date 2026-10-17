# User administration CLI (works directly on the user database file).
add_executable(mtfs-user mtfs_user.cpp)
target_link_libraries(mtfs-user PRIVATE mtfs_auth)

# Load generator and consistency checker.
add_executable(mtfs-stress mtfs_stress.cpp)
target_link_libraries(mtfs-stress PRIVATE mtfs_client)
