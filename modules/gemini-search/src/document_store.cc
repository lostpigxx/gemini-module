#include "document_store.h"

#include <algorithm>
#include <utility>

bool DocumentStore::Add(std::string doc_id,
                        std::unordered_map<std::string, std::string> fields,
                        std::string payload, bool has_payload) {
  auto it = docs_.find(doc_id);
  if (it != docs_.end()) {
    it->second.fields = std::move(fields);
    it->second.payload = std::move(payload);
    it->second.has_payload = has_payload;
    return false;
  }
  std::string key = doc_id;
  docs_.emplace(std::move(key),
                Document{std::move(doc_id), std::move(fields),
                         std::move(payload), has_payload});
  return true;
}

void DocumentStore::SetPayload(const std::string& doc_id, std::string payload) {
  auto it = docs_.find(doc_id);
  if (it != docs_.end()) {
    it->second.payload = std::move(payload);
    it->second.has_payload = true;
  }
}

bool DocumentStore::Remove(const std::string& doc_id) {
  return docs_.erase(doc_id) > 0;
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

void DocumentStore::Clear() { docs_.clear(); }
