#pragma once

#include <string>

#include "ai/client.h"

namespace patchwork {

std::string BuildPrompt(const AiRequest& request);

}  // namespace patchwork

