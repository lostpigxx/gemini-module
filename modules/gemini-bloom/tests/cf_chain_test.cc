#include <gtest/gtest.h>
#include "cf_chain.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

static std::span<const std::byte> ToSpan(const std::string& s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

TEST(CuckooChainTest, ConstructAndDestruct) {
  auto* mem = static_cast<CuckooChain*>(malloc(sizeof(CuckooChain)));
  new (mem) CuckooChain(1024, 2, 20, 2, 32);
  EXPECT_TRUE(mem->IsValid());
  EXPECT_EQ(mem->NumLayers(), 1u);
  EXPECT_EQ(mem->TotalItems(), 0u);
  EXPECT_EQ(mem->BucketSize(), 2u);
  EXPECT_EQ(mem->Expansion(), 2u);
  EXPECT_EQ(mem->MaxIterations(), 20u);
  EXPECT_EQ(mem->MaxExpansions(), 32u);
  EXPECT_GT(mem->TotalBuckets(), 0u);
  mem->~CuckooChain();
  free(mem);
}

TEST(CuckooChainTest, AddAndContains) {
  auto* mem = static_cast<CuckooChain*>(malloc(sizeof(CuckooChain)));
  new (mem) CuckooChain(1024, 2, 20, 2, 32);

  auto r1 = mem->Add(ToSpan(std::string("hello")));
  ASSERT_TRUE(r1.has_value());
  EXPECT_TRUE(*r1);
  EXPECT_TRUE(mem->Contains(ToSpan(std::string("hello"))));
  EXPECT_EQ(mem->TotalItems(), 1u);

  mem->~CuckooChain();
  free(mem);
}

TEST(CuckooChainTest, NoFalseNegatives) {
  auto* mem = static_cast<CuckooChain*>(malloc(sizeof(CuckooChain)));
  new (mem) CuckooChain(5000, 4, 500, 2, 32);

  std::vector<std::string> items;
  for (int i = 0; i < 5000; i++) items.push_back("item_" + std::to_string(i));
  for (const auto& item : items) mem->Add(ToSpan(item));
  for (const auto& item : items) {
    EXPECT_TRUE(mem->Contains(ToSpan(item))) << "False negative for " << item;
  }

  mem->~CuckooChain();
  free(mem);
}

TEST(CuckooChainTest, AutoExpansion) {
  auto* mem = static_cast<CuckooChain*>(malloc(sizeof(CuckooChain)));
  new (mem) CuckooChain(64, 2, 20, 2, 32);

  for (int i = 0; i < 500; i++) {
    auto item = "expand_" + std::to_string(i);
    mem->Add(ToSpan(item));
  }
  EXPECT_GT(mem->NumLayers(), 1u);

  for (int i = 0; i < 500; i++) {
    auto item = "expand_" + std::to_string(i);
    EXPECT_TRUE(mem->Contains(ToSpan(item)));
  }

  mem->~CuckooChain();
  free(mem);
}

TEST(CuckooChainTest, ExpansionZeroRejectsOverflow) {
  auto* mem = static_cast<CuckooChain*>(malloc(sizeof(CuckooChain)));
  new (mem) CuckooChain(16, 1, 10, 0, 32);

  bool sawFull = false;
  for (int i = 0; i < 200; i++) {
    auto item = "fixed_" + std::to_string(i);
    auto result = mem->Add(ToSpan(item));
    if (!result.has_value()) {
      sawFull = true;
      break;
    }
  }
  EXPECT_TRUE(sawFull);
  EXPECT_EQ(mem->NumLayers(), 1u);

  mem->~CuckooChain();
  free(mem);
}

TEST(CuckooChainTest, MaxExpansionsReachedReturnsNullopt) {
  auto* mem = static_cast<CuckooChain*>(malloc(sizeof(CuckooChain)));
  new (mem) CuckooChain(4, 1, 10, 2, 2);

  bool sawFull = false;
  for (int i = 0; i < 1000; i++) {
    auto item = "maxexp_" + std::to_string(i);
    auto result = mem->Add(ToSpan(item));
    if (!result.has_value()) {
      sawFull = true;
      break;
    }
  }
  EXPECT_TRUE(sawFull);
  EXPECT_LE(mem->NumLayers(), 2u);

  mem->~CuckooChain();
  free(mem);
}

TEST(CuckooChainTest, DeleteHitsFirstMatchingLayer) {
  auto* mem = static_cast<CuckooChain*>(malloc(sizeof(CuckooChain)));
  new (mem) CuckooChain(1024, 2, 20, 2, 32);

  auto item = std::string("deleteme");
  ASSERT_TRUE(mem->Add(ToSpan(item)).has_value());
  EXPECT_TRUE(mem->Contains(ToSpan(item)));

  EXPECT_TRUE(mem->Delete(ToSpan(item)));
  EXPECT_FALSE(mem->Contains(ToSpan(item)));
  EXPECT_EQ(mem->TotalDeleted(), 1u);

  EXPECT_FALSE(mem->Delete(ToSpan(item)));

  mem->~CuckooChain();
  free(mem);
}

TEST(CuckooChainTest, CountSumsAcrossLayers) {
  auto* mem = static_cast<CuckooChain*>(malloc(sizeof(CuckooChain)));
  new (mem) CuckooChain(64, 2, 20, 1, 32);

  auto item = std::string("counted");
  for (int i = 0; i < 3; i++) {
    ASSERT_TRUE(mem->Add(ToSpan(item)).has_value());
  }
  EXPECT_EQ(mem->Count(ToSpan(item)), 3u);

  mem->~CuckooChain();
  free(mem);
}

TEST(CuckooChainTest, MoveAssignmentTransfersOwnership) {
  auto* leftMem = static_cast<CuckooChain*>(malloc(sizeof(CuckooChain)));
  auto* rightMem = static_cast<CuckooChain*>(malloc(sizeof(CuckooChain)));
  new (leftMem) CuckooChain(16, 2, 20, 2, 32);
  new (rightMem) CuckooChain(32, 4, 20, 4, 32);

  rightMem->Add(ToSpan(std::string("moved")));
  *leftMem = std::move(*rightMem);

  EXPECT_TRUE(leftMem->Contains(ToSpan(std::string("moved"))));
  EXPECT_EQ(leftMem->BucketSize(), 4u);
  EXPECT_EQ(leftMem->Expansion(), 4u);
  EXPECT_EQ(leftMem->TotalItems(), 1u);
  EXPECT_FALSE(rightMem->IsValid());

  leftMem->~CuckooChain();
  rightMem->~CuckooChain();
  free(leftMem);
  free(rightMem);
}

// Bug regression: SetLayer must use placement new on calloc'd memory.
// FromRdbShell allocates the layers array with calloc (zero-filled), then
// SetLayer assigns into those slots one at a time. Without placement new,
// this is UB — mirrors ScalingBloomTest.FromRdbShellSetLayer.
TEST(CuckooChainTest, FromRdbShellSetLayer) {
  auto* orig = static_cast<CuckooChain*>(malloc(sizeof(CuckooChain)));
  new (orig) CuckooChain(64, 2, 20, 2, 32);

  std::vector<std::string> items;
  for (int i = 0; i < 300; i++) {
    auto item = "shell_" + std::to_string(i);
    if (orig->Add(ToSpan(item)).has_value()) items.push_back(item);
  }
  ASSERT_GT(orig->NumLayers(), 1u);

  CuckooChain::RdbShell shell{
    orig->TotalItems(), orig->TotalDeleted(), static_cast<uint32_t>(orig->NumLayers()),
    orig->BucketSize(), orig->MaxIterations(), orig->Expansion(), orig->MaxExpansions(),
  };
  auto* rebuilt = CuckooChain::FromRdbShell(shell);
  ASSERT_NE(rebuilt, nullptr);

  for (size_t i = 0; i < orig->NumLayers(); i++) {
    auto& src = orig->Layers()[i];
    auto layer = CuckooFilter::Create(src.cuckoo.NumBuckets(), src.cuckoo.BucketSize(),
                                       src.cuckoo.MaxIterations());
    ASSERT_TRUE(layer.has_value());
    std::memcpy(layer->GetBucketArray(), src.cuckoo.GetBucketArray(), src.cuckoo.GetDataSize());
    layer->RecountItems();
    rebuilt->SetLayer(i, CfFilterLayer{std::move(*layer)});
  }

  for (const auto& item : items) {
    EXPECT_TRUE(rebuilt->Contains(ToSpan(item)))
      << "False negative after FromRdbShell+SetLayer: " << item;
  }

  orig->~CuckooChain();
  free(orig);
  rebuilt->~CuckooChain();
  free(rebuilt);
}

// Bug regression: AppendLayer must safely move CfFilterLayer objects during
// array growth instead of relying on realloc for a non-trivial type —
// mirrors ScalingBloomTest.AppendLayerSafeRelocation.
TEST(CuckooChainTest, AppendLayerSafeRelocation) {
  auto* mem = static_cast<CuckooChain*>(malloc(sizeof(CuckooChain)));
  new (mem) CuckooChain(16, 2, 20, 2, 1024);

  std::vector<std::string> items;
  for (int i = 0; i < 2000; i++) {
    auto item = "reloc_" + std::to_string(i);
    auto result = mem->Add(ToSpan(item));
    if (!result.has_value()) break;
    items.push_back(std::move(item));
  }
  EXPECT_GT(mem->NumLayers(), 4u);

  for (const auto& item : items) {
    EXPECT_TRUE(mem->Contains(ToSpan(item))) << "False negative after layer relocation: " << item;
  }

  mem->~CuckooChain();
  free(mem);
}

TEST(CuckooChainTest, LoadingStateLifecycle) {
  auto* mem = static_cast<CuckooChain*>(malloc(sizeof(CuckooChain)));
  new (mem) CuckooChain(64, 2, 20, 2, 32);

  EXPECT_FALSE(mem->IsLoading());
  mem->SetLoading();
  EXPECT_TRUE(mem->IsLoading());
  EXPECT_EQ(mem->ChunksLoaded(), 0u);
  mem->IncrementChunksLoaded();
  mem->IncrementChunksLoaded();
  EXPECT_EQ(mem->ChunksLoaded(), 2u);
  mem->ClearLoading();
  EXPECT_FALSE(mem->IsLoading());

  mem->~CuckooChain();
  free(mem);
}

TEST(CuckooChainTest, BytesUsedAccumulates) {
  auto* mem = static_cast<CuckooChain*>(malloc(sizeof(CuckooChain)));
  new (mem) CuckooChain(64, 2, 20, 2, 32);

  uint64_t initial = mem->BytesUsed();
  EXPECT_GT(initial, sizeof(CuckooChain));

  for (int i = 0; i < 500; i++) {
    auto item = "grow_" + std::to_string(i);
    mem->Add(ToSpan(item));
  }
  EXPECT_GT(mem->NumLayers(), 1u);
  EXPECT_GT(mem->BytesUsed(), initial);

  mem->~CuckooChain();
  free(mem);
}

// --- CuckooChain::Clone ---

TEST(CuckooChainTest, CloneProducesIndependentFilter) {
  auto* mem = static_cast<CuckooChain*>(malloc(sizeof(CuckooChain)));
  new (mem) CuckooChain(64, 2, 20, 2, 32);

  std::vector<std::string> items;
  for (int i = 0; i < 500; i++) {
    auto item = "clone_" + std::to_string(i);
    if (mem->Add(ToSpan(item)).has_value()) items.push_back(item);
  }
  ASSERT_GT(mem->NumLayers(), 1u);

  auto* clone = mem->Clone();
  ASSERT_NE(clone, nullptr);
  EXPECT_EQ(clone->NumLayers(), mem->NumLayers());
  EXPECT_EQ(clone->TotalItems(), mem->TotalItems());
  EXPECT_EQ(clone->Expansion(), mem->Expansion());
  EXPECT_EQ(clone->BucketSize(), mem->BucketSize());

  for (const auto& item : items) {
    EXPECT_TRUE(clone->Contains(ToSpan(item))) << "False negative in clone for " << item;
  }

  auto extra = std::string("after_clone");
  mem->Add(ToSpan(extra));
  EXPECT_TRUE(mem->Contains(ToSpan(extra)));
  EXPECT_FALSE(clone->Contains(ToSpan(extra)));
  EXPECT_NE(clone->TotalItems(), mem->TotalItems());

  mem->~CuckooChain();
  free(mem);
  clone->~CuckooChain();
  free(clone);
}

TEST(CuckooChainTest, CloneOfEmptyFilter) {
  auto* mem = static_cast<CuckooChain*>(malloc(sizeof(CuckooChain)));
  new (mem) CuckooChain(1024, 2, 20, 2, 32);

  auto* clone = mem->Clone();
  ASSERT_NE(clone, nullptr);
  EXPECT_EQ(clone->TotalItems(), 0u);
  EXPECT_EQ(clone->NumLayers(), 1u);

  mem->~CuckooChain();
  free(mem);
  clone->~CuckooChain();
  free(clone);
}
