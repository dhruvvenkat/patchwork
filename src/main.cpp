#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "ai/codex_client.h"
#include "ai/mock_client.h"
#include "ai/openai_client.h"
#include "app.h"
#include "buffer.h"

namespace {

void PrintUsage() {
    std::cerr << "Usage: patchwork <file> [--build \"command\"] [--ai mock|openai|codex]\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string file_path;
    std::string build_command;
    std::string ai_mode;

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
            if (index + 1 >= argc) {
                PrintUsage();
                return 1;
            }
            ai_mode = argv[++index];
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
    patchwork::Buffer buffer = patchwork::LoadFileBuffer(std::filesystem::path(file_path), &error);
    if (!error.empty()) {
        std::cerr << error << '\n';
        return 1;
    }

    std::unique_ptr<patchwork::IAiClient> ai_client;
    std::string ai_provider_name = "MOCK";
    if (ai_mode.empty()) {
        const char* ai_mode_env = std::getenv("PATCHWORK_AI_MODE");
        if (ai_mode_env != nullptr) {
            ai_mode = ai_mode_env;
        }
    }

    if (ai_mode == "openai") {
        ai_client = std::make_unique<patchwork::OpenAiClient>();
        ai_provider_name = "OPENAI";
    } else if (ai_mode == "codex") {
        ai_client = std::make_unique<patchwork::CodexClient>();
        ai_provider_name = "CODEX";
    } else {
        ai_client = std::make_unique<patchwork::MockAiClient>(
            std::filesystem::path(PATCHWORK_SOURCE_DIR) / "tests" / "fixtures");
    }

    patchwork::EditorApp app(std::move(buffer), std::move(ai_client), build_command, ai_provider_name);
    return app.Run();
}
