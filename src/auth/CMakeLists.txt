# Password hashing (OpenSSL), roles and the user database.
add_library(mtfs_auth STATIC
  crypto.cpp
  role.cpp
  user_store.cpp)
target_link_libraries(mtfs_auth PUBLIC mtfs_common OpenSSL::Crypto)
