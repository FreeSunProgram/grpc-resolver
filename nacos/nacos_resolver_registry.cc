//
// Created by 孙逍 on 2023/4/26.
//

#include <grpc/grpc.h>
#include "nacos_resolver_registry.h"
#include "src/core/lib/config/core_configuration.h"

void grpc_resolver_nacos_init();
void grpc_resolver_nacos_shutdown();

namespace grpc_core {
void RegisterNacosResolver(CoreConfiguration::Builder *builder);
}

static int g_nacos_resolver_initialization = []() {
    grpc_core::CoreConfiguration::RegisterBuilder(
            grpc_core::RegisterNacosResolver);
   return 0;
}();

void RegisterNacosResolver(void) {
//    grpc_register_plugin(grpc_resolver_nacos_init, grpc_resolver_nacos_shutdown);
}