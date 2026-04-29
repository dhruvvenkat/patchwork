#pragma once

#include <string>

#include "ai/client.h"

namespace patchwork {

class CodexClient : public IAiClient {
  public:
    CodexClient();

    AiResponse Complete(const AiRequest& request) override;

  private:
    std::string model_;
    std::string profile_;
};

}  // namespace patchwork

