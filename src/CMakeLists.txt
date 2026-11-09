# One static library per module; dependencies point one way (LLD §2):
# common <- version <- ring / storage <- merkle <- rpc <- node / router / admin / metadata <- tools

add_library(dkv_common STATIC
  common/status.cc common/hash.cc common/logging.cc common/flags.cc common/fs.cc
  common/config_loader.cc common/thread_pool.cc common/event_log.cc)
dkv_target_defaults(dkv_common)
target_link_libraries(dkv_common PUBLIC dkv_proto Threads::Threads)
