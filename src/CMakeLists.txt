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

add_library(dkv_rpc STATIC rpc/rpc_util.cc rpc/fault_injector.cc rpc/peer_client_pool.cc rpc/ring_cache.cc
  common/signals.cc)
dkv_target_defaults(dkv_rpc)
target_link_libraries(dkv_rpc PUBLIC dkv_ring dkv_common)

add_library(dkv_nodelib STATIC
  node/failure_detector.cc node/coordinator.cc node/admin_reporter.cc node/services.cc
  node/anti_entropy.cc node/storage_node.cc)
dkv_target_defaults(dkv_nodelib)
target_link_libraries(dkv_nodelib PUBLIC dkv_rpc dkv_merkle dkv_storage)

add_library(dkv_adminlib STATIC admin/health_tracker.cc)
dkv_target_defaults(dkv_adminlib)
target_link_libraries(dkv_adminlib PUBLIC dkv_rpc)

add_library(dkv_routerlib STATIC router/router.cc)
dkv_target_defaults(dkv_routerlib)
target_link_libraries(dkv_routerlib PUBLIC dkv_rpc)

add_library(dkv_client STATIC client/kv_client.cc)
dkv_target_defaults(dkv_client)
target_link_libraries(dkv_client PUBLIC dkv_rpc dkv_version)

foreach(exe node router metadata admin)
  add_executable(dkv_${exe} ${exe}/main.cc)
  dkv_target_defaults(dkv_${exe})
endforeach()
target_link_libraries(dkv_node PRIVATE dkv_nodelib)
target_link_libraries(dkv_router PRIVATE dkv_routerlib)
target_link_libraries(dkv_metadata PRIVATE dkv_rpc)
target_link_libraries(dkv_admin PRIVATE dkv_adminlib)

add_executable(dkv_cli tools/kvcli.cc)
dkv_target_defaults(dkv_cli)
target_link_libraries(dkv_cli PRIVATE dkv_client)

add_library(dkv_workload STATIC tools/workload.cc)
dkv_target_defaults(dkv_workload)
target_link_libraries(dkv_workload PUBLIC dkv_common)
add_executable(dkv_loadgen tools/loadgen_main.cc)
dkv_target_defaults(dkv_loadgen)
target_link_libraries(dkv_loadgen PRIVATE dkv_client dkv_workload dkv_rpc)
add_executable(dkv_verify tools/verify_main.cc)
dkv_target_defaults(dkv_verify)
target_link_libraries(dkv_verify PRIVATE dkv_client dkv_workload)
