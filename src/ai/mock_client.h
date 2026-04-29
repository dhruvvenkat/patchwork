#pragma once

#include <filesystem>

#include "ai/client.h"

namespace patchwork {

class MockAiClient : public IAiClient {
  public:
    explicit MockAiClient(std::filesystem::path fixture_root);

    AiResponse Complete(const AiRequest& request) override;

  private:
    std::filesystem::path FixtureFor(const AiRequest& request) const;

    std::filesystem::path fixture_root_;
};

}  // namespace patchwork

