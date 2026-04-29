#pragma once

#include <string>

#include "ai/client.h"

namespace patchwork {

class OpenAiClient : public IAiClient {
  public:
    OpenAiClient();

    AiResponse Complete(const AiRequest& request) override;

  private:
    std::string model_;
};

}  // namespace patchwork

