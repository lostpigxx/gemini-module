#include "document_store.h"

#include <algorithm>
#include <chrono>
#include <utility>

int64_t DocumentStore::CurrentEpochMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

bool DocumentStore::Add(std::string doc_id,
                        std::unordered_map<std::string, std::string> fields,
                        std::string payload, bool has_payload,
                        int64_t expire_at_ms) {
  if (expire_at_ms < 0) expire_at_ms = 0;
  auto it = docs_.find(doc_id);
  if (it != docs_.end()) {
    bool was_expiring = it->second.expire_at_ms > 0;
    bool now_expiring = expire_at_ms > 0;
    if (was_expiring && !now_expiring) expiring_count_--;
    else if (!was_expiring && now_expiring) expiring_count_++;
    it->second.fields = std::move(fields);
    it->second.payload = std::move(payload);
    it->second.has_payload = has_payload;
    it->second.expire_at_ms = expire_at_ms;
    return false;
  }
  if (expire_at_ms > 0) expiring_count_++;
  std::string key = doc_id;
  docs_.emplace(std::move(key),
                Document{std::move(doc_id), std::move(fields),
                         std::move(payload), has_payload, expire_at_ms});
  return true;
}

void DocumentStore::SetPayload(const std::string& doc_id, std::string payload) {
  auto it = docs_.find(doc_id);
  if (it != docs_.end()) {
    it->second.payload = std::move(payload);
    it->second.has_payload = true;
  }
}

void DocumentStore::SetExpireAtMs(const std::string& doc_id,
                                  int64_t expire_at_ms) {
  if (expire_at_ms < 0) expire_at_ms = 0;
  auto it = docs_.find(doc_id);
  if (it == docs_.end()) return;
  bool was_expiring = it->second.expire_at_ms > 0;
  bool now_expiring = expire_at_ms > 0;
  if (was_expiring && !now_expiring) expiring_count_--;
  else if (!was_expiring && now_expiring) expiring_count_++;
  it->second.expire_at_ms = expire_at_ms;
}

bool DocumentStore::IsExpired(const std::string& doc_id, int64_t now_ms) const {
  if (expiring_count_ == 0) return false;
  auto it = docs_.find(doc_id);
  if (it == docs_.end()) return false;
  return it->second.expire_at_ms > 0 && now_ms >= it->second.expire_at_ms;
}

bool DocumentStore::HasAnyExpired(int64_t now_ms) const {
  if (expiring_count_ == 0) return false;
  for (const auto& [k, doc] : docs_) {
    if (doc.expire_at_ms > 0 && now_ms >= doc.expire_at_ms) return true;
  }
  return false;
}

std::vector<std::string> DocumentStore::NonExpiredIds(int64_t now_ms) const {
  if (expiring_count_ == 0) return AllIds();
  std::vector<std::string> ids;
  ids.reserve(docs_.size());
  for (const auto& [k, doc] : docs_) {
    if (doc.expire_at_ms == 0 || now_ms < doc.expire_at_ms) {
      ids.push_back(k);
    }
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

bool DocumentStore::Remove(const std::string& doc_id) {
  auto it = docs_.find(doc_id);
  if (it == docs_.end()) return false;
  if (it->second.expire_at_ms > 0) expiring_count_--;
  docs_.erase(it);
  return true;
}

const Document* DocumentStore::Get(const std::string& doc_id) const {
  auto it = docs_.find(doc_id);
  if (it == docs_.end()) return nullptr;
  return &it->second;
}

bool DocumentStore::Contains(const std::string& doc_id) const {
  return docs_.count(doc_id) > 0;
}

size_t DocumentStore::Size() const { return docs_.size(); }

size_t DocumentStore::MemoryUsage() const {
  size_t bytes = sizeof(*this) + docs_.bucket_count() * sizeof(void*);
  for (const auto& [k, doc] : docs_) {
    bytes += sizeof(std::pair<const std::string, Document>) + sizeof(void*);
    bytes += k.capacity() + doc.id.capacity() + doc.payload.capacity();
    bytes += doc.fields.bucket_count() * sizeof(void*);
    for (const auto& [fk, fv] : doc.fields) {
      bytes += sizeof(std::pair<const std::string, std::string>) + sizeof(void*);
      bytes += fk.capacity() + fv.capacity();
    }
  }
  return bytes;
}

std::vector<std::string> DocumentStore::AllIds() const {
  std::vector<std::string> ids;
  ids.reserve(docs_.size());
  for (auto& [k, v] : docs_) {
    ids.push_back(k);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

void DocumentStore::Clear() {
  docs_.clear();
  expiring_count_ = 0;
}
