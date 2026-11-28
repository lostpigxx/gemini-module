#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct Document {
  std::string id;
  std::unordered_map<std::string, std::string> fields;
  std::string payload;
  bool has_payload = false;
  int64_t expire_at_ms = 0;  // 0 = no expiration; > 0 = Unix epoch ms
};

class DocumentStore {
  std::unordered_map<std::string, Document> docs_;
  size_t expiring_count_ = 0;

 public:
  bool Add(std::string doc_id,
           std::unordered_map<std::string, std::string> fields,
           std::string payload = {}, bool has_payload = false,
           int64_t expire_at_ms = 0);
  void SetPayload(const std::string& doc_id, std::string payload);
  void SetExpireAtMs(const std::string& doc_id, int64_t expire_at_ms);
  bool HasExpiringDocs() const { return expiring_count_ > 0; }
  bool IsExpired(const std::string& doc_id, int64_t now_ms) const;
  bool HasAnyExpired(int64_t now_ms) const;
  std::vector<std::string> NonExpiredIds(int64_t now_ms) const;
  static int64_t CurrentEpochMs();

  bool Remove(const std::string& doc_id);
  const Document* Get(const std::string& doc_id) const;
  bool Contains(const std::string& doc_id) const;
  size_t Size() const;
  size_t MemoryUsage() const;
  std::vector<std::string> AllIds() const;
  void Clear();
};
