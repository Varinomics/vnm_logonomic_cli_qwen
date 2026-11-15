#include "qwen_acp.h"
#include "qwen_acp_test_support.h"

#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace addon = logonomic::cli_addon;
namespace wire = qwen::acp_test;

void require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

addon::Structured_inspect_result inspect(std::string state, std::string bytes, bool final = false,
    int exit_code = 0)
{
    addon::Structured_inspection request;
    request.protocol_state = std::move(state);
    request.stdout_bytes   = std::move(bytes);
    request.final          = final;
    request.exit_code      = exit_code;
    return qwen::inspect_acp(request);
}

std::string prompt_state()
{
    const auto initialized = inspect(qwen::initial_acp_state("Task"), wire::initialize_response());
    require(!initialized.failure_reason, "initialize must accept the supported native response");
    const auto prompt = inspect(initialized.protocol_state,
        R"({"jsonrpc":"2.0","id":2,"result":{"sessionId":"s","models":{"currentModelId":"m","availableModels":[{"modelId":"m","name":"Model","description":null}]},"modes":{},"configOptions":[]}})" "\n");
    require(!prompt.failure_reason && prompt.stdin_bytes &&
        prompt.stdin_bytes->find("session/prompt") != std::string::npos, "session setup must release the task");
    return prompt.protocol_state;
}

std::string chunk(std::string_view text)
{
    return R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"s","update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":)" +
        addon::serialize_json(addon::Json_value(std::string(text))) + "}}}}\n";
}

void finish(std::string state, std::string_view answer)
{
    const auto closing = inspect(std::move(state), wire::active_work(1, "s") + wire::prompt_response());
    require(!closing.complete && !closing.failure_reason && !closing.close_stdin && closing.stdin_bytes &&
        closing.stdin_bytes->find("qwen/control/session/close") != std::string::npos,
        "prompt completion and native quiescence must request a conditional close");
    const auto closed = inspect(closing.protocol_state, wire::close_response("s"));
    require(!closed.complete && !closed.failure_reason && closed.close_stdin,
        "only acknowledged native close requests a clean process exit");
    const auto settled = inspect(closed.protocol_state, "", true);
    require(settled.complete && !settled.failure_reason && settled.result_bytes == answer,
        "native completion and clean exit must return the entire answer");
    require(inspect(closed.protocol_state, "", true, 7).failure_reason.has_value(),
        "a retained answer must not override an unsuccessful process exit");
}

void accumulated_result_survives_inspections()
{
    const std::string answer(800 * 1024, 'x');
    const auto notification = chunk(std::string(100 * 1024, 'x'));
    for (const int batch_size : {8, 4, 1}) {
        auto state = prompt_state();
        for (int received = 0; received < 8; received += batch_size) {
            std::string bytes;
            for (int index = 0; index < batch_size; ++index) {
                bytes += notification;
            }
            const auto first = inspect(std::move(state), std::move(bytes));
            require(!first.complete && !first.failure_reason &&
                addon::exchange_protocol_state_valid(first.protocol_state), "model chunks must yield an accepted continuation");
            const auto idle = inspect(first.protocol_state, "");
            require(!idle.complete && !idle.failure_reason && !idle.result_bytes,
                "accepted accumulated text must survive an empty inspection");
            state = idle.protocol_state;
        }
        finish(std::move(state), answer);
    }
}

void partial_record_survives_inspections()
{
    const std::string answer(800 * 1024, 'x');
    const auto bytes = chunk(answer);
    for (const auto split : std::array<std::size_t, 4>{768 * 1024 - 1, 768 * 1024,
            768 * 1024 + 1, 800 * 1024})
    {
        const auto first = inspect(prompt_state(), bytes.substr(0, split));
        require(!first.failure_reason && addon::exchange_protocol_state_valid(first.protocol_state),
            "partial native record must yield an accepted continuation");
        const auto idle = inspect(first.protocol_state, "");
        require(!idle.failure_reason, "accepted partial record must survive an empty inspection");
        const auto retained = inspect(idle.protocol_state, bytes.substr(split));
        require(!retained.failure_reason, "native record tail must finish the same model chunk");
        finish(retained.protocol_state, answer);
    }
}

} // namespace

int main()
{
    try {
        accumulated_result_survives_inspections();
        partial_record_survives_inspections();
        std::cout << "qwen_continuation_tests batches=3 splits=4 failures=0\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "qwen_continuation_tests: " << error.what() << '\n';
        return 1;
    }
}
