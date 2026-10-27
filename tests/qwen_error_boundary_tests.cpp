#define main adapter_entry
#include "../src/adapter.cpp"
#undef main

#include <logonomic_cli_addon_test/invocation_context.h>

#include <iostream>

int main()
{
    logonomic::cli_addon::test::Sentinel_exception_context context;
    invoke(context);
    if (context.m_status != addon::Result_status::FAILED ||
        context.m_reason != "qwen_operation_failed" ||
        context.m_message.find("private-exception-sentinel") != std::string::npos)
    {
        std::cerr << "unexpected exception crossed the provider privacy boundary\n";
        return 1;
    }
    return 0;
}
