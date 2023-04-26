cmake_minimum_required(VERSION 3.25)
project(grpc_resolver)

set(CMAKE_CXX_STANDARD 11)

include_directories(
        ${GRPC_SOURCE_DIR}
        ${CMAKE_PREFIX_PATH}/include/nacos)

find_package(absl REQUIRED CONFIG)
find_package(protobuf REQUIRED CONFIG)
find_package(gRPC REQUIRED CONFIG)

add_library(grpc_nacos_resolver SHARED
        nacos/nacos_resolver_registry.h
        nacos/nacos_resolver_registry.cc
        nacos/nacos_resolver.cc)

find_library(nacos nacos-cli REQUIRED)
target_link_libraries(grpc_nacos_resolver ${nacos} absl::statusor gRPC::grpc)

install(FILES nacos/nacos_resolver_registry.h
        DESTINATION ${CMAKE_INSTALL_PREFIX}/include/grpcResolver)

install(TARGETS grpc_nacos_resolver EXPORT grpcResolverConfig
        ARCHIVE DESTINATION ${CMAKE_INSTALL_PREFIX}/lib
        INCLUDES DESTINATION ${CMAKE_INSTALL_PREFIX}/include)

install(EXPORT grpcResolverConfig
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/cmake/grpcResolver NAMESPACE grpcResolver::)
