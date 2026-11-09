# Generates C++ for all protos into ${CMAKE_CURRENT_BINARY_DIR} and builds library dkv_proto.
set(DKV_PROTOS common kv node admin metadata config)
set(GEN ${CMAKE_CURRENT_BINARY_DIR})
set(SRCS)
foreach(name ${DKV_PROTOS})
  set(p ${CMAKE_CURRENT_SOURCE_DIR}/${name}.proto)
  set(out ${GEN}/${name}.pb.cc ${GEN}/${name}.pb.h ${GEN}/${name}.grpc.pb.cc ${GEN}/${name}.grpc.pb.h)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${DKV_PROTOC}
            --cpp_out=${GEN} --grpc_out=${GEN}
            --plugin=protoc-gen-grpc=${DKV_GRPC_PLUGIN}
            -I ${CMAKE_CURRENT_SOURCE_DIR} ${p}
    DEPENDS ${p}
    COMMENT "protoc ${name}.proto")
  list(APPEND SRCS ${GEN}/${name}.pb.cc ${GEN}/${name}.grpc.pb.cc)
endforeach()

add_library(dkv_proto STATIC ${SRCS})
target_include_directories(dkv_proto PUBLIC ${GEN})
target_link_libraries(dkv_proto PUBLIC gRPC::grpc++ protobuf::libprotobuf Threads::Threads)
