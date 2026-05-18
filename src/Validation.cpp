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

  std::string sanitizeRoomCode(const std::string &input)
  {
    std::string out;
    out.reserve(24);
    for (unsigned char c : input)
    {
      if (std::isalnum(c))
      {
        out.push_back(static_cast<char>(std::tolower(c)));
      }
      else if (!out.empty() && out.back() != '-')
      {
        out.push_back('-');
      }
      if (out.size() >= 24)
      {
        break;
      }
    }

    while (!out.empty() && out.front() == '-')
      out.erase(out.begin());
    while (!out.empty() && out.back() == '-')
      out.pop_back();
    return out.size() < 3 ? "public" : out;
  }

  bool isTruthyBoolJsonCompatible(bool value)
  {
    return value;
  }
}
