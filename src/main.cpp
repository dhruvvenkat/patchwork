#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "ai/codex_client.h"
#include "ai/no_ai_client.h"
#include "ai/openai_client.h"
#include "app.h"
#include "buffer.h"

namespace {

void PrintUsage() {
    std::cerr << "Usage: flowstate <file> [--build \"command\"] [--ai [no-ai|openai|codex]] "
                 "[--no-ai] "
                 "[--cpp-standard c++17|c++20|c++23]\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string file_path;
    std::string build_command;
    std::string ai_mode;
    bool ai_mode_explicit = false;
    std::string cpp_standard;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--build") {
            if (index + 1 >= argc) {
                PrintUsage();
                return 1;
            }
            build_command = argv[++index];
            continue;
        }
        if (argument == "--ai") {
            ai_mode_explicit = true;
            if (index + 1 < argc && argv[index + 1][0] != '-') {
                ai_mode = argv[++index];
            } else {
                ai_mode = "no-ai";
            }
            continue;
        }
        if (argument.rfind("--ai=", 0) == 0) {
            ai_mode_explicit = true;
            ai_mode = argument.substr(std::string("--ai=").size());
            continue;
        }
        if (argument == "--no-ai") {
            ai_mode_explicit = true;
            ai_mode = "no-ai";
            continue;
        }
        if (argument == "--cpp-standard") {
            if (index + 1 >= argc) {
                PrintUsage();
                return 1;
            }
            cpp_standard = argv[++index];
            continue;
        }
        if (!argument.empty() && argument[0] == '-') {
            PrintUsage();
            return 1;
        }
        if (!file_path.empty()) {
            PrintUsage();
            return 1;
        }
        file_path = argument;
    }

    if (file_path.empty()) {
        PrintUsage();
        return 1;
    }

    std::string error;
    flowstate::Buffer buffer = flowstate::LoadFileBuffer(std::filesystem::path(file_path), &error);
    if (!error.empty()) {
        std::cerr << error << '\n';
        return 1;
    }

    std::unique_ptr<flowstate::IAiClient> ai_client;
    std::string ai_provider_name = "OFF";
    if (ai_mode.empty() && !ai_mode_explicit) {
        const char* ai_mode_env = std::getenv("FLOWSTATE_AI_MODE");
        if (ai_mode_env != nullptr) {
            ai_mode = ai_mode_env;
        }
    }
    if (cpp_standard.empty()) {
        const char* cpp_standard_env = std::getenv("FLOWSTATE_CPP_STANDARD");
        if (cpp_standard_env != nullptr) {
            cpp_standard = cpp_standard_env;
        }
    }
    if (ai_mode.empty()) {
        ai_mode = "no-ai";
    }

    if (ai_mode == "openai") {
        ai_client = std::make_unique<flowstate::OpenAiClient>();
        ai_provider_name = "OPENAI";
    } else if (ai_mode == "codex") {
        ai_client = std::make_unique<flowstate::CodexClient>();
        ai_provider_name = "CODEX";
    } else if (ai_mode == "no-ai" || ai_mode == "none" || ai_mode == "off") {
        ai_client = std::make_unique<flowstate::NoAiClient>();
        ai_provider_name = "OFF";
    } else {
        std::cerr << "Unknown AI mode: " << ai_mode << '\n';
        PrintUsage();
        return 1;
    }

    flowstate::EditorApp app(
        std::move(buffer), std::move(ai_client), build_command, ai_provider_name, cpp_standard);
    return app.Run();
}
