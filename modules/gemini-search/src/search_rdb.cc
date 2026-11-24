#include "search_rdb.h"
#include "search_commands.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

RedisModuleType* SearchModuleType = nullptr;

namespace {

constexpr uint64_t kMaxRdbPrefixes = 1024;
constexpr uint64_t kMaxRdbFields = 4096;
constexpr uint64_t kMaxRdbStopwords = 100000;
constexpr uint64_t kMaxRdbDocs = 100000000;
constexpr uint64_t kMaxRdbDocFields = 65536;
constexpr uint64_t kMaxRdbVectorDim = 65536;
constexpr uint64_t kMaxRdbSynonymGroups = 1000000;
constexpr uint64_t kMaxRdbSynonymTerms = 100000;
constexpr uint64_t kMaxRdbAuxEntries = 10000000;

bool HasIoError(RedisModuleIO* rdb) {
  return RedisModule_IsIOError && RedisModule_IsIOError(rdb) != 0;
}

bool SafeLoadUnsigned(RedisModuleIO* rdb, uint64_t& out) {
  out = RedisModule_LoadUnsigned(rdb);
  return !HasIoError(rdb);
}

bool SafeLoadDouble(RedisModuleIO* rdb, double& out) {
  out = RedisModule_LoadDouble(rdb);
  return !HasIoError(rdb);
}

bool SafeLoadString(RedisModuleIO* rdb, std::string& out) {
  size_t len = 0;
  char* buf = RedisModule_LoadStringBuffer(rdb, &len);
  if (HasIoError(rdb) || !buf) {
    if (buf) RedisModule_Free(buf);
    return false;
  }
  out.assign(buf, len);
  RedisModule_Free(buf);
  return true;
}

bool ValidSourceType(uint64_t raw) {
  return raw <= static_cast<uint64_t>(IndexSourceType::kJson);
}

bool ValidFieldType(uint64_t raw) {
  return raw <= static_cast<uint64_t>(FieldType::kGeoShape);
}

bool ValidVectorAlgorithm(uint64_t raw) {
  return raw <= static_cast<uint64_t>(VectorAlgorithm::kHnsw);
}

bool ValidDistanceMetric(uint64_t raw) {
  return raw <= static_cast<uint64_t>(DistanceMetric::kIP);
}

void EmitAofCommand(RedisModuleIO* aof, const char* cmd, RedisModuleString* key,
                    const std::vector<std::string>& args) {
  std::vector<RedisModuleString*> argv;
  argv.reserve(args.size() + 1);
  argv.push_back(key);
  for (const auto& s : args) {
    argv.push_back(RedisModule_CreateString(nullptr, s.data(), s.size()));
  }
  RedisModule_EmitAOF(aof, cmd, "v", argv.data(), argv.size());
  for (size_t i = 1; i < argv.size(); i++) {
    RedisModule_FreeString(nullptr, argv[i]);
  }
}

void DigestString(RedisModuleDigest* digest, const std::string& s) {
  RedisModule_DigestAddLongLong(digest, static_cast<long long>(s.size()));
  RedisModule_DigestAddStringBuffer(digest, s.data(), s.size());
}

}  // namespace

void RdbSaveSearch(RedisModuleIO* rdb, void* value) {
  auto* name_ptr = static_cast<std::string*>(value);
  auto* entry = GetIndexEntry(*name_ptr);
  if (!entry) return;

  const auto& spec = entry->spec;

  RedisModule_SaveStringBuffer(rdb, spec.name.c_str(), spec.name.size());

  RedisModule_SaveUnsigned(rdb, spec.prefixes.size());
  for (const auto& p : spec.prefixes) {
    RedisModule_SaveStringBuffer(rdb, p.c_str(), p.size());
  }

  RedisModule_SaveUnsigned(rdb, static_cast<uint64_t>(spec.source_type));

  RedisModule_SaveUnsigned(rdb, spec.fields.size());
  for (const auto& f : spec.fields) {
    RedisModule_SaveStringBuffer(rdb, f.name.c_str(), f.name.size());
    RedisModule_SaveUnsigned(rdb, static_cast<uint64_t>(f.type));
    if (f.type == FieldType::kVector) {
      RedisModule_SaveUnsigned(rdb, static_cast<uint64_t>(f.vector_params.algorithm));
      RedisModule_SaveUnsigned(rdb, f.vector_params.dim);
      RedisModule_SaveUnsigned(rdb, static_cast<uint64_t>(f.vector_params.metric));
      RedisModule_SaveUnsigned(rdb, f.vector_params.m);
      RedisModule_SaveUnsigned(rdb, f.vector_params.ef_construction);
    }
    RedisModule_SaveStringBuffer(rdb, f.json_path.c_str(), f.json_path.size());
    RedisModule_SaveUnsigned(rdb, f.sortable ? 1 : 0);
    RedisModule_SaveUnsigned(rdb, f.noindex ? 1 : 0);
    RedisModule_SaveDouble(rdb, f.weight);
    // encver >= 7: nostem, phonetic
    RedisModule_SaveUnsigned(rdb, f.nostem ? 1 : 0);
    RedisModule_SaveStringBuffer(rdb, f.phonetic.c_str(), f.phonetic.size());
  }

  // Index-level options (encver >= 6)
  RedisModule_SaveUnsigned(rdb, spec.has_custom_stopwords ? 1 : 0);
  if (spec.has_custom_stopwords) {
    RedisModule_SaveUnsigned(rdb, spec.custom_stopwords.size());
    for (const auto& sw : spec.custom_stopwords) {
      RedisModule_SaveStringBuffer(rdb, sw.c_str(), sw.size());
    }
  }
  RedisModule_SaveUnsigned(rdb, spec.nofreqs ? 1 : 0);
  RedisModule_SaveUnsigned(rdb, spec.nooffsets ? 1 : 0);
  RedisModule_SaveUnsigned(rdb, spec.nohl ? 1 : 0);
  RedisModule_SaveUnsigned(rdb, static_cast<uint64_t>(spec.temporary_ttl));

  // encver >= 7: language and per-index synonyms
  RedisModule_SaveStringBuffer(rdb, spec.language.c_str(), spec.language.size());
  auto syn_groups = entry->synonyms.Dump();
  RedisModule_SaveUnsigned(rdb, syn_groups.size());
  for (const auto& [gid, terms] : syn_groups) {
    RedisModule_SaveStringBuffer(rdb, gid.c_str(), gid.size());
    RedisModule_SaveUnsigned(rdb, terms.size());
    for (const auto& t : terms) {
      RedisModule_SaveStringBuffer(rdb, t.c_str(), t.size());
    }
  }

  auto all_ids = entry->doc_store.AllIds();
  RedisModule_SaveUnsigned(rdb, all_ids.size());
  for (const auto& doc_id : all_ids) {
    RedisModule_SaveStringBuffer(rdb, doc_id.c_str(), doc_id.size());
    const auto* doc = entry->doc_store.Get(doc_id);
    if (doc) {
      RedisModule_SaveUnsigned(rdb, doc->fields.size());
      for (const auto& [k, v] : doc->fields) {
        RedisModule_SaveStringBuffer(rdb, k.c_str(), k.size());
        RedisModule_SaveStringBuffer(rdb, v.c_str(), v.size());
      }
    } else {
      RedisModule_SaveUnsigned(rdb, 0);
    }
  }
}

void* RdbLoadSearch(RedisModuleIO* rdb, int encver) {
  if (encver < 1 || encver > kSearchEncVer) return nullptr;

  std::string index_name;
  if (!SafeLoadString(rdb, index_name) || index_name.empty()) return nullptr;

  std::vector<std::string> prefixes;
  if (encver >= 2) {
    uint64_t prefix_count = 0;
    if (!SafeLoadUnsigned(rdb, prefix_count) || prefix_count > kMaxRdbPrefixes) return nullptr;
    prefixes.reserve(static_cast<size_t>(prefix_count));
    for (uint64_t i = 0; i < prefix_count; i++) {
      std::string p;
      if (!SafeLoadString(rdb, p)) return nullptr;
      prefixes.push_back(std::move(p));
    }
  }

  IndexSourceType source_type = IndexSourceType::kHash;
  if (encver >= 5) {
    uint64_t raw_src = 0;
    if (!SafeLoadUnsigned(rdb, raw_src) || !ValidSourceType(raw_src)) return nullptr;
    source_type = static_cast<IndexSourceType>(raw_src);
  }

  uint64_t field_count = 0;
  if (!SafeLoadUnsigned(rdb, field_count) || field_count > kMaxRdbFields) return nullptr;
  std::vector<FieldSpec> fields;
  fields.reserve(static_cast<size_t>(field_count));
  for (uint64_t i = 0; i < field_count; i++) {
    FieldSpec f;
    if (!SafeLoadString(rdb, f.name) || f.name.empty()) return nullptr;
    uint64_t raw_type = 0;
    if (!SafeLoadUnsigned(rdb, raw_type) || !ValidFieldType(raw_type)) return nullptr;
    f.type = static_cast<FieldType>(raw_type);
    if (f.type == FieldType::kVector) {
      uint64_t raw_algo = 0, raw_dim = 0, raw_metric = 0;
      if (!SafeLoadUnsigned(rdb, raw_algo) || !ValidVectorAlgorithm(raw_algo)) return nullptr;
      if (!SafeLoadUnsigned(rdb, raw_dim) || raw_dim == 0 || raw_dim > kMaxRdbVectorDim) {
        return nullptr;
      }
      if (!SafeLoadUnsigned(rdb, raw_metric) || !ValidDistanceMetric(raw_metric)) return nullptr;
      f.vector_params.algorithm = static_cast<VectorAlgorithm>(raw_algo);
      f.vector_params.dim = static_cast<size_t>(raw_dim);
      f.vector_params.metric = static_cast<DistanceMetric>(raw_metric);
      if (encver >= 3) {
        uint64_t raw_m = 0, raw_efc = 0;
        if (!SafeLoadUnsigned(rdb, raw_m) || !SafeLoadUnsigned(rdb, raw_efc)) return nullptr;
        if (raw_m == 0 || raw_m > 1024 || raw_efc == 0 || raw_efc > 100000) return nullptr;
        f.vector_params.m = static_cast<size_t>(raw_m);
        f.vector_params.ef_construction = static_cast<size_t>(raw_efc);
      }
    }
    if (encver >= 5) {
      if (!SafeLoadString(rdb, f.json_path)) return nullptr;
    }
    if (encver >= 6) {
      uint64_t raw_sortable = 0, raw_noindex = 0;
      double raw_weight = 1.0;
      if (!SafeLoadUnsigned(rdb, raw_sortable) || !SafeLoadUnsigned(rdb, raw_noindex)) {
        return nullptr;
      }
      if (!SafeLoadDouble(rdb, raw_weight) || !std::isfinite(raw_weight) || raw_weight < 0.0) {
        return nullptr;
      }
      f.sortable = raw_sortable != 0;
      f.noindex = raw_noindex != 0;
      f.weight = raw_weight;
    }
    if (encver >= 7) {
      uint64_t raw_nostem = 0;
      if (!SafeLoadUnsigned(rdb, raw_nostem)) return nullptr;
      f.nostem = raw_nostem != 0;
      if (!SafeLoadString(rdb, f.phonetic)) return nullptr;
    }
    fields.push_back(std::move(f));
  }

  // Index-level options
  std::vector<std::string> custom_stopwords;
  bool has_custom_stopwords = false;
  bool nofreqs = false, nooffsets = false, nohl = false;
  int temporary_ttl = 0;
  if (encver >= 6) {
    uint64_t raw_has_sw = 0;
    if (!SafeLoadUnsigned(rdb, raw_has_sw)) return nullptr;
    has_custom_stopwords = raw_has_sw != 0;
    if (has_custom_stopwords) {
      uint64_t sw_count = 0;
      if (!SafeLoadUnsigned(rdb, sw_count) || sw_count > kMaxRdbStopwords) return nullptr;
      custom_stopwords.reserve(static_cast<size_t>(sw_count));
      for (uint64_t si = 0; si < sw_count; si++) {
        std::string sw;
        if (!SafeLoadString(rdb, sw)) return nullptr;
        custom_stopwords.push_back(std::move(sw));
      }
    }
    uint64_t raw_nf = 0, raw_no = 0, raw_nhl = 0, raw_ttl = 0;
    if (!SafeLoadUnsigned(rdb, raw_nf) || !SafeLoadUnsigned(rdb, raw_no) ||
        !SafeLoadUnsigned(rdb, raw_nhl) || !SafeLoadUnsigned(rdb, raw_ttl)) {
      return nullptr;
    }
    if (raw_ttl > static_cast<uint64_t>(std::numeric_limits<int>::max())) return nullptr;
    nofreqs = raw_nf != 0;
    nooffsets = raw_no != 0;
    nohl = raw_nhl != 0;
    temporary_ttl = static_cast<int>(raw_ttl);
  }

  std::string language = "english";
  std::vector<std::pair<std::string, std::vector<std::string>>> syn_groups;
  if (encver >= 7) {
    if (!SafeLoadString(rdb, language)) return nullptr;
    if (language.empty()) language = "english";
    uint64_t syn_count = 0;
    if (!SafeLoadUnsigned(rdb, syn_count) || syn_count > kMaxRdbSynonymGroups) return nullptr;
    syn_groups.reserve(static_cast<size_t>(syn_count));
    for (uint64_t gi = 0; gi < syn_count; gi++) {
      std::string gid;
      uint64_t tcount = 0;
      if (!SafeLoadString(rdb, gid) || !SafeLoadUnsigned(rdb, tcount) ||
          tcount > kMaxRdbSynonymTerms) {
        return nullptr;
      }
      std::vector<std::string> terms;
      terms.reserve(static_cast<size_t>(tcount));
      for (uint64_t ti = 0; ti < tcount; ti++) {
        std::string t;
        if (!SafeLoadString(rdb, t)) return nullptr;
        terms.push_back(std::move(t));
      }
      syn_groups.emplace_back(std::move(gid), std::move(terms));
    }
  }

  uint64_t doc_count = 0;
  if (!SafeLoadUnsigned(rdb, doc_count) || doc_count > kMaxRdbDocs) return nullptr;
  std::vector<std::pair<std::string, std::unordered_map<std::string, std::string>>> docs;
  docs.reserve(static_cast<size_t>(doc_count));
  for (uint64_t i = 0; i < doc_count; i++) {
    std::string doc_id;
    uint64_t nfields = 0;
    if (!SafeLoadString(rdb, doc_id) || !SafeLoadUnsigned(rdb, nfields) ||
        nfields > kMaxRdbDocFields) {
      return nullptr;
    }
    std::unordered_map<std::string, std::string> doc_fields;
    doc_fields.reserve(static_cast<size_t>(nfields));
    for (uint64_t j = 0; j < nfields; j++) {
      std::string k, v;
      if (!SafeLoadString(rdb, k) || !SafeLoadString(rdb, v)) return nullptr;
      doc_fields[std::move(k)] = std::move(v);
    }
    docs.emplace_back(std::move(doc_id), std::move(doc_fields));
  }

  IndexSpec spec{index_name, std::move(fields), std::move(prefixes),
                 std::move(language), source_type, std::move(custom_stopwords),
                 has_custom_stopwords, nofreqs, nooffsets, nohl, temporary_ttl};
  CreateIndexFromRdb(index_name, std::move(spec), docs);

  if (auto* entry = GetIndexEntry(index_name)) {
    for (const auto& [gid, terms] : syn_groups) {
      entry->synonyms.Update(gid, terms);
    }
  }

  return new std::string(std::move(index_name));
}

void AofRewriteSearch(RedisModuleIO* aof, RedisModuleString* key, void* value) {
  auto* name_ptr = static_cast<std::string*>(value);
  auto* entry = GetIndexEntry(*name_ptr);
  if (!entry) return;

  const auto& spec = entry->spec;

  std::vector<std::string> create_args;
  if (!spec.prefixes.empty()) {
    create_args.push_back("ON");
    create_args.push_back(spec.source_type == IndexSourceType::kJson ? "JSON" : "HASH");
    create_args.push_back("PREFIX");
    create_args.push_back(std::to_string(spec.prefixes.size()));
    for (const auto& p : spec.prefixes) create_args.push_back(p);
  }
  if (!spec.language.empty() && spec.language != "english") {
    create_args.push_back("LANGUAGE");
    create_args.push_back(spec.language);
  }
  if (spec.has_custom_stopwords) {
    create_args.push_back("STOPWORDS");
    create_args.push_back(std::to_string(spec.custom_stopwords.size()));
    for (const auto& sw : spec.custom_stopwords) create_args.push_back(sw);
  }
  if (spec.nofreqs) create_args.push_back("NOFREQS");
  if (spec.nooffsets) create_args.push_back("NOOFFSETS");
  if (spec.nohl) create_args.push_back("NOHL");
  if (spec.temporary_ttl > 0) {
    create_args.push_back("TEMPORARY");
    create_args.push_back(std::to_string(spec.temporary_ttl));
  }

  create_args.push_back("SCHEMA");
  for (const auto& f : spec.fields) {
    if (!f.json_path.empty()) {
      create_args.push_back(f.json_path);
      create_args.push_back("AS");
    }
    create_args.push_back(f.name);
    create_args.push_back(FieldTypeName(f.type));
    if (f.type == FieldType::kVector) {
      create_args.push_back(VectorAlgorithmName(f.vector_params.algorithm));
      create_args.push_back("DIM");
      create_args.push_back(std::to_string(f.vector_params.dim));
      create_args.push_back("DISTANCE_METRIC");
      create_args.push_back(DistanceMetricName(f.vector_params.metric));
      if (f.vector_params.algorithm == VectorAlgorithm::kHnsw) {
        create_args.push_back("M");
        create_args.push_back(std::to_string(f.vector_params.m));
        create_args.push_back("EF_CONSTRUCTION");
        create_args.push_back(std::to_string(f.vector_params.ef_construction));
      }
    }
    if (f.nostem) create_args.push_back("NOSTEM");
    if (!f.phonetic.empty()) {
      create_args.push_back("PHONETIC");
      create_args.push_back(f.phonetic);
    }
    if (f.sortable) create_args.push_back("SORTABLE");
    if (f.noindex) create_args.push_back("NOINDEX");
    if (f.weight != 1.0) {
      create_args.push_back("WEIGHT");
      create_args.push_back(std::to_string(f.weight));
    }
  }
  EmitAofCommand(aof, "FT.CREATE", key, create_args);

  // Emit per-index synonym groups
  for (const auto& [gid, terms] : entry->synonyms.Dump()) {
    std::vector<std::string> syn_args;
    syn_args.reserve(terms.size() + 1);
    syn_args.push_back(gid);
    for (const auto& t : terms) syn_args.push_back(t);
    EmitAofCommand(aof, "FT.SYNUPDATE", key, syn_args);
  }

  // Emit all documents with unlimited fields and binary-safe values
  auto all_ids = entry->doc_store.AllIds();
  for (const auto& doc_id : all_ids) {
    const auto* doc = entry->doc_store.Get(doc_id);
    if (!doc) continue;

    std::vector<std::string> add_args;
    add_args.reserve(2 + doc->fields.size() * 2);
    add_args.push_back(doc_id);
    add_args.push_back("FIELDS");
    for (const auto& [k, v] : doc->fields) {
      add_args.push_back(k);
      add_args.push_back(v);
    }
    EmitAofCommand(aof, "FT.ADD", key, add_args);
  }
}

void FreeSearch(void* value) {
  auto* name_ptr = static_cast<std::string*>(value);
  EraseIndexEntry(*name_ptr);
  delete name_ptr;
}

size_t SearchMemUsage(const void* value) {
  auto* name_ptr = static_cast<const std::string*>(value);
  auto* entry = GetIndexEntry(*name_ptr);
  if (!entry) return 0;

  size_t bytes = sizeof(*entry) + entry->doc_store.MemoryUsage();

  // Schema metadata
  bytes += entry->spec.name.capacity() + entry->spec.language.capacity();
  for (const auto& p : entry->spec.prefixes) bytes += sizeof(std::string) + p.capacity();
  for (const auto& sw : entry->spec.custom_stopwords) bytes += sizeof(std::string) + sw.capacity();
  for (const auto& f : entry->spec.fields) {
    bytes += sizeof(FieldSpec) + f.name.capacity() + f.json_path.capacity() + f.phonetic.capacity();
    switch (f.type) {
      case FieldType::kTag:
        if (const auto* idx = entry->tag_indices.Get(f.name)) {
          bytes += sizeof(TagIndex) + idx->NumTags() * 64;
        }
        break;
      case FieldType::kNumeric:
        if (const auto* idx = entry->numeric_indices.Get(f.name)) {
          bytes += sizeof(NumericIndex) + idx->Size() * 64;
        }
        break;
      case FieldType::kText:
        if (const auto* idx = entry->text_indices.Get(f.name)) {
          bytes += sizeof(TextIndex) + idx->NumTerms() * 96 + idx->NumDocs() * 48;
        }
        break;
      case FieldType::kVector:
        if (const auto* idx = entry->vector_indices.Get(f.name)) {
          size_t per_vec = idx->Dim() * sizeof(float) + 64;
          if (f.vector_params.algorithm == VectorAlgorithm::kHnsw) {
            per_vec += f.vector_params.m * 2 * sizeof(std::string);
          }
          bytes += idx->Size() * per_vec;
        }
        break;
      case FieldType::kGeo:
        if (const auto* idx = entry->geo_indices.Get(f.name)) {
          bytes += sizeof(GeoIndex) + idx->Size() * (sizeof(GeoCoord) + 48);
        }
        break;
      case FieldType::kGeoShape:
        if (const auto* idx = entry->geoshape_indices.Get(f.name)) {
          bytes += sizeof(GeoShapeIndex) + idx->Size() * 128;
        }
        break;
    }
  }

  for (const auto& [gid, terms] : entry->synonyms.Dump()) {
    bytes += gid.size() + 48;
    for (const auto& t : terms) bytes += t.size() + 32;
  }

  return bytes;
}

void DigestSearch(RedisModuleDigest* digest, void* value) {
  auto* name_ptr = static_cast<std::string*>(value);
  auto* entry = GetIndexEntry(*name_ptr);
  if (!entry) {
    RedisModule_DigestEndSequence(digest);
    return;
  }

  const auto& spec = entry->spec;
  DigestString(digest, spec.name);
  RedisModule_DigestAddLongLong(digest, static_cast<long long>(spec.source_type));
  DigestString(digest, spec.language);
  RedisModule_DigestAddLongLong(digest, spec.has_custom_stopwords ? 1 : 0);
  RedisModule_DigestAddLongLong(digest, spec.nofreqs ? 1 : 0);
  RedisModule_DigestAddLongLong(digest, spec.nooffsets ? 1 : 0);
  RedisModule_DigestAddLongLong(digest, spec.nohl ? 1 : 0);
  RedisModule_DigestAddLongLong(digest, static_cast<long long>(spec.temporary_ttl));

  RedisModule_DigestAddLongLong(digest, static_cast<long long>(spec.prefixes.size()));
  for (const auto& p : spec.prefixes) DigestString(digest, p);

  RedisModule_DigestAddLongLong(digest, static_cast<long long>(spec.fields.size()));
  for (const auto& f : spec.fields) {
    DigestString(digest, f.name);
    RedisModule_DigestAddLongLong(digest, static_cast<long long>(f.type));
    DigestString(digest, f.json_path);
    RedisModule_DigestAddLongLong(digest, f.nostem ? 1 : 0);
    DigestString(digest, f.phonetic);
    RedisModule_DigestAddLongLong(digest, f.sortable ? 1 : 0);
    RedisModule_DigestAddLongLong(digest, f.noindex ? 1 : 0);
    if (f.type == FieldType::kVector) {
      RedisModule_DigestAddLongLong(digest, static_cast<long long>(f.vector_params.algorithm));
      RedisModule_DigestAddLongLong(digest, static_cast<long long>(f.vector_params.dim));
      RedisModule_DigestAddLongLong(digest, static_cast<long long>(f.vector_params.metric));
    }
  }

  auto syn_groups = entry->synonyms.Dump();
  RedisModule_DigestAddLongLong(digest, static_cast<long long>(syn_groups.size()));
  for (const auto& [gid, terms] : syn_groups) {
    DigestString(digest, gid);
    RedisModule_DigestAddLongLong(digest, static_cast<long long>(terms.size()));
    for (const auto& t : terms) DigestString(digest, t);
  }

  auto all_ids = entry->doc_store.AllIds();
  RedisModule_DigestAddLongLong(digest, static_cast<long long>(all_ids.size()));
  for (const auto& doc_id : all_ids) {
    DigestString(digest, doc_id);
    const auto* doc = entry->doc_store.Get(doc_id);
    if (!doc) continue;
    std::vector<std::string> keys;
    keys.reserve(doc->fields.size());
    for (const auto& [k, v] : doc->fields) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    RedisModule_DigestAddLongLong(digest, static_cast<long long>(keys.size()));
    for (const auto& k : keys) {
      DigestString(digest, k);
      DigestString(digest, doc->fields.at(k));
    }
  }

  RedisModule_DigestEndSequence(digest);
}

void* CopySearch2(RedisModuleKeyOptCtx* ctx, const void* value) {
  auto* src_name = static_cast<const std::string*>(value);
  auto* src_entry = GetIndexEntry(*src_name);
  if (!src_entry) return nullptr;

  if (!ctx || !RedisModule_GetToKeyNameFromOptCtx || !RedisModule_StringPtrLen) {
    return nullptr;
  }
  const RedisModuleString* to_key = RedisModule_GetToKeyNameFromOptCtx(ctx);
  if (!to_key) return nullptr;
  size_t len = 0;
  const char* ptr = RedisModule_StringPtrLen(to_key, &len);
  if (!ptr || len == 0) return nullptr;
  std::string dst_name(ptr, len);
  if (dst_name == *src_name || GetIndexEntry(dst_name) != nullptr) {
    return nullptr;
  }

  IndexSpec spec_copy = src_entry->spec;
  spec_copy.name = dst_name;

  auto all_ids = src_entry->doc_store.AllIds();
  std::vector<std::pair<std::string, std::unordered_map<std::string, std::string>>> docs;
  docs.reserve(all_ids.size());
  for (const auto& doc_id : all_ids) {
    if (const auto* doc = src_entry->doc_store.Get(doc_id)) {
      docs.emplace_back(doc_id, doc->fields);
    }
  }

  CreateIndexFromRdb(dst_name, std::move(spec_copy), docs);
  if (auto* dst_entry = GetIndexEntry(dst_name)) {
    for (const auto& [gid, terms] : src_entry->synonyms.Dump()) {
      dst_entry->synonyms.Update(gid, terms);
    }
  }

  return new std::string(std::move(dst_name));
}

size_t FreeEffortSearch2(RedisModuleKeyOptCtx* ctx, const void* value) {
  (void)ctx;
  auto* name_ptr = static_cast<const std::string*>(value);
  auto* entry = GetIndexEntry(*name_ptr);
  if (!entry) return 1;
  return 1 + entry->doc_store.Size();
}

int DefragSearch(RedisModuleDefragCtx* ctx, RedisModuleString* key, void** value) {
  (void)ctx;
  (void)key;
  (void)value;
  // Index storage uses C++ standard allocator rather than RedisModule_Alloc.
  return 0;
}

void AuxSaveSearch(RedisModuleIO* rdb, int when) {
  if (when != REDISMODULE_AUX_AFTER_RDB) return;

  RedisModule_SaveUnsigned(rdb, kSearchAuxEncVer);

  // 1. Aliases (sorted by alias name)
  const auto& aliases = GetAliases();
  std::vector<std::string> alias_keys;
  alias_keys.reserve(aliases.size());
  for (const auto& [k, v] : aliases) alias_keys.push_back(k);
  std::sort(alias_keys.begin(), alias_keys.end());
  RedisModule_SaveUnsigned(rdb, alias_keys.size());
  for (const auto& k : alias_keys) {
    const auto& v = aliases.at(k);
    RedisModule_SaveStringBuffer(rdb, k.c_str(), k.size());
    RedisModule_SaveStringBuffer(rdb, v.c_str(), v.size());
  }

  // 2. Suggest dictionaries (sorted by dict name)
  const auto& suggest_dicts = GetSuggestDicts();
  std::vector<std::string> sug_names;
  sug_names.reserve(suggest_dicts.size());
  for (const auto& [k, v] : suggest_dicts) sug_names.push_back(k);
  std::sort(sug_names.begin(), sug_names.end());
  RedisModule_SaveUnsigned(rdb, sug_names.size());
  for (const auto& dname : sug_names) {
    RedisModule_SaveStringBuffer(rdb, dname.c_str(), dname.size());
    auto entries = suggest_dicts.at(dname).DumpAll();
    RedisModule_SaveUnsigned(rdb, entries.size());
    for (const auto& e : entries) {
      RedisModule_SaveStringBuffer(rdb, e.str.c_str(), e.str.size());
      RedisModule_SaveDouble(rdb, e.score);
      RedisModule_SaveStringBuffer(rdb, e.payload.c_str(), e.payload.size());
    }
  }

  // 3. Term dictionaries (sorted by dict name)
  const auto& term_dicts = GetTermDicts();
  std::vector<std::string> term_names;
  term_names.reserve(term_dicts.size());
  for (const auto& [k, v] : term_dicts) term_names.push_back(k);
  std::sort(term_names.begin(), term_names.end());
  RedisModule_SaveUnsigned(rdb, term_names.size());
  for (const auto& dname : term_names) {
    RedisModule_SaveStringBuffer(rdb, dname.c_str(), dname.size());
    auto terms = term_dicts.at(dname).Dump();
    RedisModule_SaveUnsigned(rdb, terms.size());
    for (const auto& t : terms) {
      RedisModule_SaveStringBuffer(rdb, t.c_str(), t.size());
    }
  }

  // 4. Module config (sorted by key)
  const auto& cfg = GetConfig();
  std::vector<std::string> cfg_keys;
  cfg_keys.reserve(cfg.size());
  for (const auto& [k, v] : cfg) cfg_keys.push_back(k);
  std::sort(cfg_keys.begin(), cfg_keys.end());
  RedisModule_SaveUnsigned(rdb, cfg_keys.size());
  for (const auto& k : cfg_keys) {
    const auto& v = cfg.at(k);
    RedisModule_SaveStringBuffer(rdb, k.c_str(), k.size());
    RedisModule_SaveStringBuffer(rdb, v.c_str(), v.size());
  }
}

int AuxLoadSearch(RedisModuleIO* rdb, int encver, int when) {
  (void)encver;
  if (when != REDISMODULE_AUX_AFTER_RDB) return REDISMODULE_OK;

  uint64_t aux_ver = 0;
  if (!SafeLoadUnsigned(rdb, aux_ver) || aux_ver != kSearchAuxEncVer) {
    return REDISMODULE_ERR;
  }

  // Load into temporary containers so partial failures do not corrupt global state.
  std::unordered_map<std::string, std::string> loaded_aliases;
  uint64_t alias_count = 0;
  if (!SafeLoadUnsigned(rdb, alias_count) || alias_count > kMaxRdbAuxEntries) {
    return REDISMODULE_ERR;
  }
  for (uint64_t i = 0; i < alias_count; i++) {
    std::string k, v;
    if (!SafeLoadString(rdb, k) || !SafeLoadString(rdb, v)) return REDISMODULE_ERR;
    loaded_aliases[std::move(k)] = std::move(v);
  }

  std::unordered_map<std::string, SuggestDict> loaded_suggest;
  uint64_t sug_dict_count = 0;
  if (!SafeLoadUnsigned(rdb, sug_dict_count) || sug_dict_count > kMaxRdbAuxEntries) {
    return REDISMODULE_ERR;
  }
  for (uint64_t i = 0; i < sug_dict_count; i++) {
    std::string dname;
    uint64_t entry_count = 0;
    if (!SafeLoadString(rdb, dname) || !SafeLoadUnsigned(rdb, entry_count) ||
        entry_count > kMaxRdbAuxEntries) {
      return REDISMODULE_ERR;
    }
    auto& dict = loaded_suggest[dname];
    for (uint64_t j = 0; j < entry_count; j++) {
      std::string s, payload;
      double score = 0.0;
      if (!SafeLoadString(rdb, s) || !SafeLoadDouble(rdb, score) ||
          !std::isfinite(score) || !SafeLoadString(rdb, payload)) {
        return REDISMODULE_ERR;
      }
      dict.Add(s, score, false, payload);
    }
  }

  std::unordered_map<std::string, TermDict> loaded_terms;
  uint64_t term_dict_count = 0;
  if (!SafeLoadUnsigned(rdb, term_dict_count) || term_dict_count > kMaxRdbAuxEntries) {
    return REDISMODULE_ERR;
  }
  for (uint64_t i = 0; i < term_dict_count; i++) {
    std::string dname;
    uint64_t tcount = 0;
    if (!SafeLoadString(rdb, dname) || !SafeLoadUnsigned(rdb, tcount) ||
        tcount > kMaxRdbAuxEntries) {
      return REDISMODULE_ERR;
    }
    std::vector<std::string> terms;
    terms.reserve(static_cast<size_t>(tcount));
    for (uint64_t j = 0; j < tcount; j++) {
      std::string t;
      if (!SafeLoadString(rdb, t)) return REDISMODULE_ERR;
      terms.push_back(std::move(t));
    }
    loaded_terms[dname].Add(terms);
  }

  std::unordered_map<std::string, std::string> loaded_cfg;
  uint64_t cfg_count = 0;
  if (!SafeLoadUnsigned(rdb, cfg_count) || cfg_count > kMaxRdbAuxEntries) {
    return REDISMODULE_ERR;
  }
  for (uint64_t i = 0; i < cfg_count; i++) {
    std::string k, v;
    if (!SafeLoadString(rdb, k) || !SafeLoadString(rdb, v)) return REDISMODULE_ERR;
    loaded_cfg[std::move(k)] = std::move(v);
  }

  GetAliases() = std::move(loaded_aliases);
  GetSuggestDicts() = std::move(loaded_suggest);
  GetTermDicts() = std::move(loaded_terms);
  for (auto& [k, v] : loaded_cfg) {
    GetConfig()[k] = std::move(v);
  }

  return REDISMODULE_OK;
}
