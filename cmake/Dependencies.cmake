# Finds protobuf + gRPC on both targets:
#  - RCE:   module gRPC/1.74.1 (protobuf 31.x, ships CMake configs for protobuf, gRPC, absl)
#  - local: Ubuntu packages (gRPC 1.51 ships a CMake config, protobuf 3.21 does not -> FindProtobuf)
find_package(Threads REQUIRED)

find_package(protobuf CONFIG QUIET)
if(NOT protobuf_FOUND)
  find_package(Protobuf REQUIRED)
endif()
find_package(gRPC CONFIG REQUIRED)

if(TARGET protobuf::protoc)
  set(DKV_PROTOC $<TARGET_FILE:protobuf::protoc>)
elseif(Protobuf_PROTOC_EXECUTABLE)
  set(DKV_PROTOC ${Protobuf_PROTOC_EXECUTABLE})
else()
  find_program(DKV_PROTOC protoc REQUIRED)
endif()

if(TARGET gRPC::grpc_cpp_plugin)
  set(DKV_GRPC_PLUGIN $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
else()
  find_program(DKV_GRPC_PLUGIN grpc_cpp_plugin REQUIRED)
endif()

message(STATUS "dkv: protobuf ${Protobuf_VERSION}${protobuf_VERSION}, gRPC ${gRPC_VERSION}")

# RCE compute nodes differ: gRPC's exported targets link /usr/lib64/libsystemd.so (the dev symlink),
# which exists on some nodes (node03) but not others (node06), where only libsystemd.so.0 is present.
# Point the link at the runtime library in that case (the binaries only need libsystemd.so.0).
if(NOT EXISTS /usr/lib64/libsystemd.so AND EXISTS /usr/lib64/libsystemd.so.0 AND gRPC_DIR
   AND EXISTS "${gRPC_DIR}/gRPCTargets.cmake")
  # CMake 3.20 (RCE module) has no IMPORTED_TARGETS property, so read the target names from the file.
  file(STRINGS "${gRPC_DIR}/gRPCTargets.cmake" _decls REGEX "^add_library\\(gRPC::")
  set(_n 0)
  foreach(_d ${_decls})
    string(REGEX MATCH "gRPC::[A-Za-z0-9_+]+" _t "${_d}")
    if(TARGET ${_t})
      get_target_property(_libs ${_t} INTERFACE_LINK_LIBRARIES)
      if(_libs)
        set(_fixed "")
        foreach(_l ${_libs})
          if(_l STREQUAL "systemd" OR _l STREQUAL "/usr/lib64/libsystemd.so")
            list(APPEND _fixed "/usr/lib64/libsystemd.so.0")
          else()
            list(APPEND _fixed "${_l}")
          endif()
        endforeach()
        set_target_properties(${_t} PROPERTIES INTERFACE_LINK_LIBRARIES "${_fixed}")
        math(EXPR _n "${_n} + 1")
      endif()
    endif()
  endforeach()
  message(STATUS "dkv: libsystemd.so dev symlink missing; ${_n} gRPC targets relinked to libsystemd.so.0")
endif()
