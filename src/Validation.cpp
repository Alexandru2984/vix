#include "Validation.hpp"

#include <algorithm>
#include <cctype>

namespace arena
{
  namespace
  {
    std::string cleanText(const std::string &input, std::size_t maxLen)
    {
      std::string out;
      out.reserve(std::min(input.size(), maxLen));

      bool lastWasSpace = false;
      for (unsigned char c : input)
      {
        if (out.size() >= maxLen)
        {
          break;
        }

        if (c < 32 || c == 127)
        {
          if (!lastWasSpace && !out.empty())
          {
            out.push_back(' ');
            lastWasSpace = true;
          }
          continue;
        }

        if (std::isspace(c))
        {
          if (!lastWasSpace && !out.empty())
          {
            out.push_back(' ');
            lastWasSpace = true;
          }
          continue;
        }

        out.push_back(static_cast<char>(c));
        lastWasSpace = false;
      }

      while (!out.empty() && out.back() == ' ')
      {
        out.pop_back();
      }

      return out;
    }
  }

  std::string sanitizeDisplayName(const std::string &input)
  {
    return cleanText(input, maxNameLength);
  }

  std::string sanitizeChatMessage(const std::string &input)
  {
    return cleanText(input, maxChatLength);
  }

  bool isTruthyBoolJsonCompatible(bool value)
  {
    return value;
  }
}
