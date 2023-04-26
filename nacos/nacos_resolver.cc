//
// Created by sx on 2022/4/5.
//

#include "Nacos.h"
#include "constant/ConfigConstant.h"
#include "constant/NamingConstant.h"
#include "naming/selectors/HealthInstanceSelector.h"
#include "absl/memory/memory.h"

#include "src/core/lib/address_utils/parse_address.h"
#include "src/core/lib/config/core_configuration.h"
#include "src/core/lib/resolver/resolver_registry.h"
#include "src/core/lib/gprpp/work_serializer.h"
#include "src/core/lib/iomgr/exec_ctx.h"

namespace grpc_core {

/// 调试用
TraceFlag grpc_nacos_resolver_trace(false, "nacos_resolver");

class NacosResolver : public Resolver {
public:
    explicit NacosResolver(ResolverArgs args);

    ~NacosResolver() override;

    void StartLocked() override;

    void RequestReresolutionLocked() override;

    void ResetBackoffLocked() override;

    void ShutdownLocked() override;

private:
    class Listener : public nacos::EventListener {
    public:
        explicit Listener(RefCountedPtr <NacosResolver> resolver)
                : resolver_(std::move(resolver)) {}

        void receiveNamingInfo(const nacos::ServiceInfo &changeAdvice) override {
            grpc_core::ExecCtx exec_ctx(GRPC_EXEC_CTX_FLAG_THREAD_RESOURCE_LOOP);
            auto changeAdviceCpy = changeAdvice;
            auto hosts = changeAdviceCpy.getHosts();
            resolver_->work_serializer_->Run([this, hosts]() mutable {
                gpr_log(GPR_INFO, "[nacos_resolver Listener %p] reresolution", this);
                resolver_->ResolveAddress(std::move(hosts));
            }, DEBUG_LOCATION);
        }

    private:
        RefCountedPtr <NacosResolver> resolver_;
    };

    /// 创建Nacos命名服务
    void CreateNamingServer();

    ///
    void ResolveAddress(std::list <nacos::Instance> &&);

    /// nacos 服务地址
    std::string nacos_server_;
    /// 服务名
    std::string name_to_resolve_;
    /// 分组名
    std::string group_name_;
    /// 通道参数
    ChannelArgs channel_args_;
    /// 回调任务执行序列,将任务通过Run丢进执行队列执行
    std::shared_ptr <WorkSerializer> work_serializer_;
    /// 结果句柄，通过它返回resolve结果
    std::unique_ptr <ResultHandler> result_handler_;
    /// 不详 pollset_set to drive the name resolution process
    grpc_pollset_set *interested_parties_;
    /// 命名服务
    std::unique_ptr <nacos::NamingService> naming_server_;
    /// nacos event listener
    std::unique_ptr <Listener> listener_;
};

NacosResolver::NacosResolver(ResolverArgs args)
        : nacos_server_(args.uri.authority()),
          name_to_resolve_(absl::StripPrefix(args.uri.path(), "/")),
          channel_args_(std::move(args.args)),
          work_serializer_(std::move(args.work_serializer)),
          result_handler_(std::move(args.result_handler)),
          interested_parties_(args.pollset_set) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_nacos_resolver_trace)) {
        gpr_log(GPR_INFO, "[nacos_resolver %p] created for server name %s", this,
                name_to_resolve_.c_str());
    }
    auto group = channel_args_.GetOwnedString(nacos::NamingConstant::GROUP_NAME.c_str());
    group_name_ = group ? group.value() : nacos::ConfigConstant::DEFAULT_GROUP;
    CreateNamingServer();
}

NacosResolver::~NacosResolver() {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_nacos_resolver_trace)) {
        gpr_log(GPR_INFO, "[nacos_resolver %p] destroyed", this);
    }
}

void NacosResolver::StartLocked() {
    if (!naming_server_) {
        Result result;
        result.service_config =
                absl::UnavailableError("Resolver transient failure");
        result_handler_->ReportResult(result);
        return;
    }
    try {
        nacos::naming::selectors::HealthInstanceSelector selector;
        auto instances =
//        naming_server_->getInstanceWithPredicate(name_to_resolve_, &selector);
                naming_server_->getAllInstances(name_to_resolve_, group_name_);
        ResolveAddress(std::move(instances));

    } catch (nacos::NacosException &e) {
        gpr_log(GPR_INFO, "[nacos_resolver %p] %s", this, e.what());
        Result result;
        result.service_config = absl::UnavailableError(e.what());
        result_handler_->ReportResult(result);
    }
}

void NacosResolver::RequestReresolutionLocked() {
    Resolver::RequestReresolutionLocked();
}

void NacosResolver::ResetBackoffLocked() {
    // TODO
}

void NacosResolver::ShutdownLocked() {
    // TODO
}

void NacosResolver::CreateNamingServer() {
    try {
        // TODO nacos_server_ 空时从配置读取
        nacos::Properties nacos_props;
        nacos_props[nacos::PropertyKeyConst::SERVER_ADDR] = nacos_server_;
        nacos_props[nacos::PropertyKeyConst::UDP_RECEIVER_PORT] = "0";
        nacos_props[nacos::PropertyKeyConst::SUBSCRIPTION_POLL_INTERVAL] = "2000";
        std::unique_ptr <nacos::INacosServiceFactory> factory(
                nacos::NacosFactoryFactory::getNacosFactory(nacos_props));
        naming_server_ =
                std::unique_ptr<nacos::NamingService>(factory->CreateNamingService());
        listener_ = absl::make_unique<NacosResolver::Listener>(Ref());
        naming_server_->subscribe(name_to_resolve_, group_name_, listener_.get());
    } catch (nacos::NacosException &e) {
        gpr_log(GPR_INFO, "[nacos_resolver %p] %s", this, e.what());
    }
}

void NacosResolver::ResolveAddress(std::list <nacos::Instance> &&instances) {
    static auto is_healthy = std::not1(
            std::function<bool(nacos::Instance & )>(&nacos::Instance::healthy));
    static auto instance_sort = [](nacos::Instance &l, nacos::Instance &r) {
        return atoll(l.metadata["createdTime"].c_str()) < atoll(r.metadata["createdTime"].c_str());
    };
    instances.erase(std::remove_if(
                            instances.begin(), instances.end(), is_healthy),
                    instances.end());

    instances.sort(instance_sort);

    Result result;
    grpc_arg new_args[] = {};
    result.args = channel_args_;
    grpc_resolved_address addr{};
    ServerAddressList addressList;
    for (const auto &instance: instances) {
        auto uri = URI::Create(
                "ipv4", {},
                instance.ip + ":" + nacos::NacosStringOps::valueOf(instance.port), {},
                {});
        if (uri.ok() and grpc_parse_uri(uri.value(), &addr)) {
            addressList.emplace_back(addr, channel_args_);
        }
    }
    result.addresses = std::move(addressList);
    result_handler_->ReportResult(result);
}

class NacosResolverFactory : public ResolverFactory {
public:
    bool IsValidUri(const URI &uri) const override {
        // 符合：nacos:///service
        if (absl::StripPrefix(uri.path(), "/").empty()) {
            gpr_log(GPR_ERROR, "no server name supplied in nacos URI");
            return false;
        }
        return true;
    }

    OrphanablePtr <Resolver> CreateResolver(ResolverArgs args) const override {
        if (!IsValidUri(args.uri)) return nullptr;
        return MakeOrphanable<NacosResolver>(std::move(args));
    }

    absl::string_view scheme() const override { return "nacos"; }
};

void RegisterNacosResolver(CoreConfiguration::Builder *builder) {
    builder->resolver_registry()->RegisterResolverFactory(
            absl::make_unique<grpc_core::NacosResolverFactory>());
}

}  // namespace grpc_core

void grpc_resolver_nacos_init() {}

void grpc_resolver_nacos_shutdown() {}