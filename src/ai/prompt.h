#pragma once

#include <string>

#include "ai/client.h"

namespace flowstate {

std::string BuildPrompt(const AiRequest& request);

}  // namespace flowstate

