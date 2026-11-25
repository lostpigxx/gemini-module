#pragma once

#include <string>

std::string StemEnglish(const std::string& word);
std::string StemFrench(const std::string& word);
std::string StemGerman(const std::string& word);
std::string StemSpanish(const std::string& word);

bool IsSupportedStemLanguage(const std::string& language);
std::string StemWord(const std::string& word, const std::string& language);
