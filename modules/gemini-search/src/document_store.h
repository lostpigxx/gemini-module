#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

struct Document {
  std::string id;
  std::unordered_map<std::string, std::string> fields;
};

class DocumentStore {
  std::unordered_map<std::string, Document> docs_;

 public:
  bool Add(std::string doc_id,
           std::unordered_map<std::string, std::string> fields);
  bool Remove(const std::string& doc_id);
  const Document* Get(const std::string& doc_id) const;
  bool Contains(const std::string& doc_id) const;
  size_t Size() const;
  std::vector<std::string> AllIds() const;
  void Clear();
};
