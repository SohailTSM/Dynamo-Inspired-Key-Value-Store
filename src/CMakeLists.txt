# One static library per module; dependencies point one way (LLD §2):
# common <- version <- ring / storage <- merkle <- rpc <- node / router / admin / metadata <- tools

add_library(dkv_common STATIC
  common/status.cc common/hash.cc common/logging.cc common/flags.cc common/fs.cc
  common/config_loader.cc common/thread_pool.cc common/event_log.cc)
dkv_target_defaults(dkv_common)
target_link_libraries(dkv_common PUBLIC dkv_proto Threads::Threads)

add_library(dkv_version STATIC version/vector_clock.cc version/version.cc)
dkv_target_defaults(dkv_version)
target_link_libraries(dkv_version PUBLIC dkv_common)

add_library(dkv_ring STATIC ring/hash_ring.cc)
dkv_target_defaults(dkv_ring)
target_link_libraries(dkv_ring PUBLIC dkv_common)

add_library(dkv_storage STATIC
  storage/record_codec.cc storage/segment.cc storage/version_log.cc storage/index.cc
  storage/log_storage_engine.cc)
dkv_target_defaults(dkv_storage)
target_link_libraries(dkv_storage PUBLIC dkv_version)

add_library(dkv_merkle STATIC merkle/merkle_tree.cc)
dkv_target_defaults(dkv_merkle)
target_link_libraries(dkv_merkle PUBLIC dkv_storage dkv_ring)
