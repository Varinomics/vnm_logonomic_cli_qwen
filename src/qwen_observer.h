#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/helper_ingress.h>
#include <vnm_filesystem_observation.h>

#include <memory>

namespace qwen {

/// Retains native child evidence only for host-reserved, helper-admitted roots.
/// The process composition root owns the shared filesystem observation service.
class Lifecycle_observer
{
public:
    Lifecycle_observer();
    Lifecycle_observer(
        std::shared_ptr<vnm::filesystem_observation::Service> service,
        vnm::filesystem_observation::Configuration configuration);
    ~Lifecycle_observer();

    Lifecycle_observer(const Lifecycle_observer&)            = delete;
    Lifecycle_observer& operator=(const Lifecycle_observer&) = delete;

    void handle(logonomic::cli_addon::Invocation_context& context);
    logonomic::cli_addon::Classify_event_result classify(
        const logonomic::cli_addon::Classify_event_request& request);

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace qwen
