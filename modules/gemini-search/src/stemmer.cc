// Clean-room Porter2 (Snowball) English stemmer.
// Based on the publicly documented algorithm at snowballstem.org/algorithms/english/stemmer.html

#include "stemmer.h"

#include <algorithm>
#include <string>

static bool IsVowel(char c) {
  return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}

static bool IsVowelY(const std::string& w, size_t i) {
  if (w[i] == 'y') return i > 0 && !IsVowel(w[i - 1]);
  return IsVowel(w[i]);
}

static bool EndsWith(const std::string& w, const std::string& suffix) {
  if (suffix.size() > w.size()) return false;
  return w.compare(w.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static bool EndsWithDouble(const std::string& w) {
  if (w.size() < 2) return false;
  char c = w.back();
  return w[w.size() - 2] == c &&
         (c == 'b' || c == 'd' || c == 'f' || c == 'g' || c == 'm' ||
          c == 'n' || c == 'p' || c == 'r' || c == 't');
}

static bool IsShortSyllable(const std::string& w, size_t i) {
  if (i == 0) return w.size() > 0 && IsVowel(w[0]) && (w.size() == 1 || !IsVowel(w[1]));
  if (i >= w.size()) return false;
  return i >= 1 && i + 1 < w.size() && !IsVowel(w[i - 1]) && IsVowelY(w, i) &&
         !IsVowel(w[i + 1]) && w[i + 1] != 'w' && w[i + 1] != 'x' && w[i + 1] != 'Y';
}

static size_t FindR(const std::string& w, size_t start) {
  for (size_t i = start; i < w.size(); i++) {
    if (i > start && IsVowelY(w, i - 1) && !IsVowelY(w, i)) return i + 1;
  }
  return w.size();
}

static bool ContainsVowel(const std::string& w, size_t start, size_t end) {
  for (size_t i = start; i < end && i < w.size(); i++) {
    if (IsVowelY(w, i)) return true;
  }
  return false;
}

static bool IsShortWord(const std::string& w, size_t r1) {
  if (r1 < w.size()) return false;
  if (w.size() < 2) return true;
  return IsShortSyllable(w, w.size() - 2);
}

static bool ReplaceSuffix(std::string& w, const std::string& from,
                          const std::string& to, size_t r) {
  if (!EndsWith(w, from)) return false;
  if (w.size() - from.size() >= r) {
    w.resize(w.size() - from.size());
    w += to;
  }
  return true;
}

static const char* kExceptions[][2] = {
    {"skis", "ski"},       {"skies", "sky"},     {"dying", "die"},
    {"lying", "lie"},      {"tying", "tie"},      {"idly", "idl"},
    {"gently", "gentl"},   {"ugly", "ugli"},      {"early", "earli"},
    {"only", "onli"},      {"singly", "singl"},
};

static const char* kExceptions2[] = {
    "inning", "outing", "canning", "herring", "earring",
    "proceed", "exceed", "succeed",
};

static const char* kInvariant[] = {
    "sky",    "news",   "howe",   "atlas",  "cosmos", "bias",
    "andes",
};

std::string StemEnglish(const std::string& word) {
  if (word.size() <= 2) return word;

  std::string w = word;

  for (auto& [from, to] : kExceptions) {
    if (w == from) return to;
  }
  for (auto& inv : kInvariant) {
    if (w == inv) return w;
  }

  // Normalize initial apostrophes
  if (w[0] == '\'') w = w.substr(1);
  if (w.empty()) return word;

  // Set initial Y after vowel to uppercase Y
  if (w[0] == 'y') w[0] = 'Y';
  for (size_t i = 1; i < w.size(); i++) {
    if (w[i] == 'y' && IsVowel(w[i - 1])) w[i] = 'Y';
  }

  size_t r1 = FindR(w, 0);
  size_t r2 = FindR(w, r1);

  // Adjust R1 for special prefixes
  if (w.compare(0, 5, "gener") == 0 || w.compare(0, 5, "arsen") == 0) {
    if (r1 < 5) r1 = 5;
  } else if (w.compare(0, 6, "commun") == 0) {
    if (r1 < 6) r1 = 6;
  }

  // Step 0
  if (EndsWith(w, "'s'")) {
    w.resize(w.size() - 3);
  } else if (EndsWith(w, "'s")) {
    w.resize(w.size() - 2);
  } else if (w.back() == '\'') {
    w.pop_back();
  }

  // Step 1a
  if (EndsWith(w, "sses")) {
    w.resize(w.size() - 2);
  } else if (EndsWith(w, "ied") || EndsWith(w, "ies")) {
    w.resize(w.size() - (w.size() > 4 ? 2 : 1));
  } else if (EndsWith(w, "us") || EndsWith(w, "ss")) {
    // do nothing
  } else if (w.back() == 's' && w.size() > 2) {
    bool has_vowel = false;
    for (size_t i = 0; i + 2 < w.size(); i++) {
      if (IsVowelY(w, i)) { has_vowel = true; break; }
    }
    if (has_vowel) w.pop_back();
  }

  // Post step-1a exceptions
  for (auto& exc : kExceptions2) {
    if (w == exc) return w;
  }

  // Step 1b
  bool step1b_extra = false;
  if (EndsWith(w, "eedly")) {
    if (w.size() - 5 >= r1) {
      w.resize(w.size() - 3);
    }
  } else if (EndsWith(w, "eed")) {
    if (w.size() - 3 >= r1) {
      w.resize(w.size() - 1);
    }
  } else {
    bool found = false;
    std::string base;
    if (EndsWith(w, "ingly")) {
      base = w.substr(0, w.size() - 5);
      found = true;
    } else if (EndsWith(w, "edly")) {
      base = w.substr(0, w.size() - 4);
      found = true;
    } else if (EndsWith(w, "ing")) {
      base = w.substr(0, w.size() - 3);
      found = true;
    } else if (EndsWith(w, "ed")) {
      base = w.substr(0, w.size() - 2);
      found = true;
    }
    if (found && ContainsVowel(base, 0, base.size())) {
      w = base;
      step1b_extra = true;
    }
  }

  if (step1b_extra) {
    if (EndsWith(w, "at") || EndsWith(w, "bl") || EndsWith(w, "iz")) {
      w += 'e';
    } else if (EndsWithDouble(w)) {
      w.pop_back();
    } else if (IsShortWord(w, r1)) {
      w += 'e';
    }
  }

  // Step 1c
  if (w.size() > 2 && (w.back() == 'y' || w.back() == 'Y') &&
      !IsVowel(w[w.size() - 2])) {
    w.back() = 'i';
  }

  // Step 2
  if (w.size() >= 2) {
    switch (w[w.size() - 2]) {
      case 'a':
        ReplaceSuffix(w, "ational", "ate", r1) || ReplaceSuffix(w, "tional", "tion", r1);
        break;
      case 'c':
        ReplaceSuffix(w, "enci", "ence", r1) || ReplaceSuffix(w, "anci", "ance", r1);
        break;
      case 'e':
        ReplaceSuffix(w, "izer", "ize", r1);
        break;
      case 'g':
        ReplaceSuffix(w, "ogi", "og", r1) &&
            (w.size() >= 2 && w[w.size() - 2] == 'l'); // only if preceded by l
        if (EndsWith(w, "ogi") && w.size() >= 4 && w[w.size() - 4] == 'l') {
          // already handled or not applicable
        }
        break;
      case 'l':
        ReplaceSuffix(w, "bli", "ble", r1) || ReplaceSuffix(w, "alli", "al", r1) ||
            ReplaceSuffix(w, "entli", "ent", r1) || ReplaceSuffix(w, "eli", "e", r1) ||
            ReplaceSuffix(w, "ousli", "ous", r1);
        break;
      case 'o':
        ReplaceSuffix(w, "ization", "ize", r1) || ReplaceSuffix(w, "ation", "ate", r1) ||
            ReplaceSuffix(w, "ator", "ate", r1);
        break;
      case 's':
        ReplaceSuffix(w, "alism", "al", r1) || ReplaceSuffix(w, "iveness", "ive", r1) ||
            ReplaceSuffix(w, "fulness", "ful", r1) || ReplaceSuffix(w, "ousness", "ous", r1);
        break;
      case 't':
        ReplaceSuffix(w, "aliti", "al", r1) || ReplaceSuffix(w, "iviti", "ive", r1) ||
            ReplaceSuffix(w, "biliti", "ble", r1);
        break;
      case 'u':
        ReplaceSuffix(w, "fulli", "ful", r1);
        break;
      case 'i':
        ReplaceSuffix(w, "li", "", r1) &&
            (w.size() >= 1 &&
             (w.back() == 'c' || w.back() == 'd' || w.back() == 'e' || w.back() == 'g' ||
              w.back() == 'h' || w.back() == 'k' || w.back() == 'm' || w.back() == 'n' ||
              w.back() == 'r' || w.back() == 't'));
        break;
    }
  }

  // Step 3
  if (EndsWith(w, "ational") && w.size() - 7 >= r1) {
    w.resize(w.size() - 7);
    w += "ate";
  } else if (ReplaceSuffix(w, "tional", "tion", r1)) {
  } else if (ReplaceSuffix(w, "alize", "al", r1)) {
  } else if (ReplaceSuffix(w, "icate", "ic", r1) ||
             ReplaceSuffix(w, "iciti", "ic", r1)) {
  } else if (EndsWith(w, "ical") && w.size() - 4 >= r1) {
    w.resize(w.size() - 2);
  } else if (EndsWith(w, "ful") && w.size() - 3 >= r1) {
    w.resize(w.size() - 3);
  } else if (EndsWith(w, "ness") && w.size() - 4 >= r1) {
    w.resize(w.size() - 4);
  } else if (EndsWith(w, "ative") && w.size() - 5 >= r2) {
    w.resize(w.size() - 5);
  }

  // Step 4
  if (w.size() >= 2) {
    auto Try4 = [&](const std::string& suf) -> bool {
      if (!EndsWith(w, suf)) return false;
      if (w.size() - suf.size() >= r2) {
        w.resize(w.size() - suf.size());
        return true;
      }
      return false;
    };
    Try4("al") || Try4("ance") || Try4("ence") || Try4("er") || Try4("ic") ||
        Try4("able") || Try4("ible") || Try4("ant") || Try4("ement") ||
        Try4("ment") || Try4("ent") || Try4("ism") || Try4("ate") ||
        Try4("iti") || Try4("ous") || Try4("ive") || Try4("ize") ||
        (EndsWith(w, "ion") && w.size() - 3 >= r2 && w.size() >= 4 &&
         (w[w.size() - 4] == 's' || w[w.size() - 4] == 't') &&
         (w.resize(w.size() - 3), true));
  }

  // Step 5
  if (w.back() == 'e') {
    if (w.size() - 1 >= r2) {
      w.pop_back();
    } else if (w.size() - 1 >= r1 && !IsShortSyllable(w, w.size() - 3)) {
      w.pop_back();
    }
  } else if (w.back() == 'l' && EndsWithDouble(w) && w.size() - 1 >= r2) {
    w.pop_back();
  }

  // Restore Y → y
  for (auto& c : w) {
    if (c == 'Y') c = 'y';
  }

  return w;
}
