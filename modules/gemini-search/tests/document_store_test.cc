#include <gtest/gtest.h>
#include "document_store.h"

#include <algorithm>
#include <string>
#include <unordered_map>

TEST(DocumentStoreTest, AddNewDocReturnsTrue) {
  DocumentStore store;
  EXPECT_TRUE(store.Add("doc1", {{"name", "Alice"}}));
  EXPECT_EQ(store.Size(), 1u);
}

TEST(DocumentStoreTest, AddExistingDocReturnsFalse) {
  DocumentStore store;
  store.Add("doc1", {{"name", "Alice"}});
  EXPECT_FALSE(store.Add("doc1", {{"name", "Bob"}}));
  EXPECT_EQ(store.Size(), 1u);
}

TEST(DocumentStoreTest, ReplaceUpdatesFields) {
  DocumentStore store;
  store.Add("doc1", {{"name", "Alice"}});
  store.Add("doc1", {{"name", "Bob"}, {"age", "30"}});
  auto* doc = store.Get("doc1");
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(doc->fields.at("name"), "Bob");
  EXPECT_EQ(doc->fields.at("age"), "30");
}

TEST(DocumentStoreTest, GetExistingDoc) {
  DocumentStore store;
  store.Add("doc1", {{"status", "active"}});
  auto* doc = store.Get("doc1");
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(doc->id, "doc1");
  EXPECT_EQ(doc->fields.at("status"), "active");
}

TEST(DocumentStoreTest, GetNonexistentReturnsNull) {
  DocumentStore store;
  EXPECT_EQ(store.Get("nope"), nullptr);
}

TEST(DocumentStoreTest, RemoveExistingReturnsTrue) {
  DocumentStore store;
  store.Add("doc1", {{"a", "b"}});
  EXPECT_TRUE(store.Remove("doc1"));
  EXPECT_EQ(store.Size(), 0u);
  EXPECT_EQ(store.Get("doc1"), nullptr);
}

TEST(DocumentStoreTest, RemoveNonexistentReturnsFalse) {
  DocumentStore store;
  EXPECT_FALSE(store.Remove("nope"));
}

TEST(DocumentStoreTest, ContainsCheck) {
  DocumentStore store;
  store.Add("doc1", {});
  EXPECT_TRUE(store.Contains("doc1"));
  EXPECT_FALSE(store.Contains("doc2"));
}

TEST(DocumentStoreTest, SizeTracking) {
  DocumentStore store;
  EXPECT_EQ(store.Size(), 0u);
  store.Add("a", {});
  EXPECT_EQ(store.Size(), 1u);
  store.Add("b", {});
  EXPECT_EQ(store.Size(), 2u);
  store.Remove("a");
  EXPECT_EQ(store.Size(), 1u);
}

TEST(DocumentStoreTest, AllIdsSorted) {
  DocumentStore store;
  store.Add("charlie", {});
  store.Add("alpha", {});
  store.Add("bravo", {});
  auto ids = store.AllIds();
  ASSERT_EQ(ids.size(), 3u);
  EXPECT_EQ(ids[0], "alpha");
  EXPECT_EQ(ids[1], "bravo");
  EXPECT_EQ(ids[2], "charlie");
}

TEST(DocumentStoreTest, AllIdsEmpty) {
  DocumentStore store;
  EXPECT_TRUE(store.AllIds().empty());
}

TEST(DocumentStoreTest, ClearRemovesAll) {
  DocumentStore store;
  store.Add("a", {{"x", "1"}});
  store.Add("b", {{"y", "2"}});
  store.Clear();
  EXPECT_EQ(store.Size(), 0u);
  EXPECT_TRUE(store.AllIds().empty());
}

TEST(DocumentStoreTest, EmptyDocId) {
  DocumentStore store;
  EXPECT_TRUE(store.Add("", {{"a", "b"}}));
  EXPECT_TRUE(store.Contains(""));
  auto* doc = store.Get("");
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(doc->id, "");
}

TEST(DocumentStoreTest, EmptyFieldValues) {
  DocumentStore store;
  store.Add("doc1", {{"", ""}, {"name", ""}});
  auto* doc = store.Get("doc1");
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(doc->fields.at(""), "");
  EXPECT_EQ(doc->fields.at("name"), "");
}

TEST(DocumentStoreTest, ManyFields) {
  DocumentStore store;
  std::unordered_map<std::string, std::string> fields;
  for (int i = 0; i < 100; i++) {
    fields["field_" + std::to_string(i)] = "val_" + std::to_string(i);
  }
  store.Add("doc1", fields);
  auto* doc = store.Get("doc1");
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(doc->fields.size(), 100u);
  EXPECT_EQ(doc->fields.at("field_50"), "val_50");
}

TEST(DocumentStoreTest, ExpirationTrackingAndFiltering) {
  DocumentStore store;
  EXPECT_FALSE(store.HasExpiringDocs());

  store.Add("d1", {{"k", "v"}}, {}, false, 0);
  store.Add("d2", {{"k", "v"}}, {}, false, 1000);
  store.Add("d3", {{"k", "v"}}, {}, false, 2000);
  EXPECT_TRUE(store.HasExpiringDocs());
  EXPECT_FALSE(store.IsExpired("d1", 5000));
  EXPECT_FALSE(store.IsExpired("d2", 999));
  EXPECT_TRUE(store.IsExpired("d2", 1000));
  EXPECT_FALSE(store.IsExpired("d3", 1500));
  EXPECT_TRUE(store.HasAnyExpired(1500));
  EXPECT_FALSE(store.HasAnyExpired(500));

  auto live = store.NonExpiredIds(1500);
  ASSERT_EQ(live.size(), 2u);
  EXPECT_EQ(live[0], "d1");
  EXPECT_EQ(live[1], "d3");

  // PERSIST d2 (set expire_at_ms = 0)
  store.SetExpireAtMs("d2", 0);
  EXPECT_FALSE(store.IsExpired("d2", 5000));
  EXPECT_TRUE(store.HasExpiringDocs());

  // Remove d3 -> no more expiring docs
  store.Remove("d3");
  EXPECT_FALSE(store.HasExpiringDocs());

  // Add expiring then Clear
  store.SetExpireAtMs("d1", 100);
  EXPECT_TRUE(store.HasExpiringDocs());
  store.Clear();
  EXPECT_FALSE(store.HasExpiringDocs());
}
