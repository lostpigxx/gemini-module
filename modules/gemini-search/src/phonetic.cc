#include "phonetic.h"

#include <algorithm>
#include <cctype>

static bool IsVowel(char c) {
  return c == 'A' || c == 'E' || c == 'I' || c == 'O' || c == 'U';
}

static bool SlavoGermanic(const std::string& s) {
  return s.find('W') != std::string::npos ||
         s.find('K') != std::string::npos ||
         s.find("CZ") != std::string::npos ||
         s.find("WITZ") != std::string::npos;
}

static char At(const std::string& s, int i) {
  if (i < 0 || i >= static_cast<int>(s.size())) return '\0';
  return s[static_cast<size_t>(i)];
}

static bool StringAt(const std::string& s, int start, int len, const char* targets) {
  if (start < 0 || start + len > static_cast<int>(s.size())) return false;
  std::string sub = s.substr(static_cast<size_t>(start), static_cast<size_t>(len));
  const char* p = targets;
  while (*p) {
    size_t tl = 0;
    while (p[tl] && p[tl] != '|') tl++;
    if (tl == sub.size() && sub.compare(0, tl, p, tl) == 0) return true;
    p += tl;
    if (*p == '|') p++;
  }
  return false;
}

std::pair<std::string, std::string> DoubleMetaphone(const std::string& word) {
  std::string w;
  w.reserve(word.size());
  for (auto c : word) w += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

  std::string primary, secondary;
  int length = static_cast<int>(w.size());
  int current = 0;

  if (length < 1) return {"", ""};

  // Skip initial silent letters
  if (StringAt(w, 0, 2, "GN|KN|PN|AE|WR")) current = 1;

  if (At(w, 0) == 'X') {
    primary += 'S';
    secondary += 'S';
    current = 1;
  }

  while (current < length && (primary.size() < 4 || secondary.size() < 4)) {
    char c = At(w, current);

    if (IsVowel(c)) {
      if (current == 0) { primary += 'A'; secondary += 'A'; }
      current++;
      continue;
    }

    switch (c) {
      case 'B':
        primary += 'P'; secondary += 'P';
        current += (At(w, current + 1) == 'B') ? 2 : 1;
        break;

      case 'C':
        if (At(w, current + 1) == 'H') {
          primary += 'X'; secondary += 'X';
          current += 2;
        } else if (StringAt(w, current, 2, "CI|CE|CY")) {
          primary += 'S'; secondary += 'S';
          current += 2;
        } else {
          primary += 'K'; secondary += 'K';
          current += (StringAt(w, current, 2, "CK|CC")) ? 2 : 1;
        }
        break;

      case 'D':
        if (StringAt(w, current, 2, "DG")) {
          if (StringAt(w, current + 2, 1, "I|E|Y")) {
            primary += 'J'; secondary += 'J';
            current += 3;
          } else {
            primary += "TK"; secondary += "TK";
            current += 2;
          }
        } else {
          primary += 'T'; secondary += 'T';
          current += (At(w, current + 1) == 'D') ? 2 : 1;
        }
        break;

      case 'F':
        primary += 'F'; secondary += 'F';
        current += (At(w, current + 1) == 'F') ? 2 : 1;
        break;

      case 'G':
        if (At(w, current + 1) == 'H') {
          if (current > 0 && !IsVowel(At(w, current - 1))) {
            primary += 'K'; secondary += 'K';
            current += 2;
          } else if (current == 0) {
            primary += 'K'; secondary += 'K';
            current += 2;
          } else {
            current += 2;
          }
        } else if (At(w, current + 1) == 'N') {
          if (current == 0) {
            primary += "KN"; secondary += "N";
          }
          current += (At(w, current + 2) == 'N') ? 3 : 2;
        } else if (StringAt(w, current + 1, 1, "I|E|Y")) {
          primary += 'J'; secondary += 'K';
          current += 2;
        } else {
          primary += 'K'; secondary += 'K';
          current += (At(w, current + 1) == 'G') ? 2 : 1;
        }
        break;

      case 'H':
        if (IsVowel(At(w, current + 1)) &&
            (current == 0 || IsVowel(At(w, current - 1)))) {
          primary += 'H'; secondary += 'H';
          current += 2;
        } else {
          current++;
        }
        break;

      case 'J':
        primary += 'J'; secondary += 'J';
        current += (At(w, current + 1) == 'J') ? 2 : 1;
        break;

      case 'K':
        primary += 'K'; secondary += 'K';
        current += (At(w, current + 1) == 'K') ? 2 : 1;
        break;

      case 'L':
        primary += 'L'; secondary += 'L';
        current += (At(w, current + 1) == 'L') ? 2 : 1;
        break;

      case 'M':
        primary += 'M'; secondary += 'M';
        current += (At(w, current + 1) == 'M') ? 2 : 1;
        break;

      case 'N':
        primary += 'N'; secondary += 'N';
        current += (At(w, current + 1) == 'N') ? 2 : 1;
        break;

      case 'P':
        if (At(w, current + 1) == 'H') {
          primary += 'F'; secondary += 'F';
          current += 2;
        } else {
          primary += 'P'; secondary += 'P';
          current += (At(w, current + 1) == 'P') ? 2 : 1;
        }
        break;

      case 'Q':
        primary += 'K'; secondary += 'K';
        current += (At(w, current + 1) == 'Q') ? 2 : 1;
        break;

      case 'R':
        primary += 'R'; secondary += 'R';
        current += (At(w, current + 1) == 'R') ? 2 : 1;
        break;

      case 'S':
        if (StringAt(w, current, 2, "SH")) {
          primary += 'X'; secondary += 'X';
          current += 2;
        } else if (StringAt(w, current, 3, "SIO|SIA")) {
          if (SlavoGermanic(w)) {
            primary += 'S'; secondary += 'S';
          } else {
            primary += 'X'; secondary += 'S';
          }
          current += 3;
        } else if (StringAt(w, current, 2, "SC")) {
          if (StringAt(w, current + 2, 1, "I|E|Y")) {
            primary += 'S'; secondary += 'S';
          } else {
            primary += "SK"; secondary += "SK";
          }
          current += 3;
        } else {
          primary += 'S'; secondary += 'S';
          current += (StringAt(w, current, 2, "SS|SZ")) ? 2 : 1;
        }
        break;

      case 'T':
        if (StringAt(w, current, 4, "TION")) {
          primary += 'X'; secondary += 'X';
          current += 3;
        } else if (StringAt(w, current, 3, "TIA|TCH")) {
          primary += 'X'; secondary += 'X';
          current += 3;
        } else if (StringAt(w, current, 2, "TH")) {
          primary += '0'; secondary += 'T';
          current += 2;
        } else {
          primary += 'T'; secondary += 'T';
          current += (StringAt(w, current, 2, "TT|TD")) ? 2 : 1;
        }
        break;

      case 'V':
        primary += 'F'; secondary += 'F';
        current += (At(w, current + 1) == 'V') ? 2 : 1;
        break;

      case 'W':
        if (IsVowel(At(w, current + 1))) {
          primary += 'A'; secondary += 'F';
          current += 2;
        } else {
          current++;
        }
        break;

      case 'X':
        primary += "KS"; secondary += "KS";
        current += (StringAt(w, current, 2, "XX")) ? 2 : 1;
        break;

      case 'Z':
        primary += 'S'; secondary += 'S';
        current += (At(w, current + 1) == 'Z') ? 2 : 1;
        break;

      default:
        current++;
        break;
    }
  }

  if (primary.size() > 4) primary.resize(4);
  if (secondary.size() > 4) secondary.resize(4);

  return {primary, secondary};
}
