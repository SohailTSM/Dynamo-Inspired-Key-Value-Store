#include "merkle/merkle_tree.h"
#include "test_framework.h"

using namespace dkv;

static Digest160 D(const std::string& s) { return Sha1Of(s); }

TEST_CASE("merkle: heap layout") {
  CHECK_EQ(MerkleTree::children(0).size(), 16u);
  CHECK_EQ(MerkleTree::children(0).front(), 1u);
  CHECK_EQ(MerkleTree::children(1).front(), 17u);
  CHECK_EQ(MerkleTree::children(16).back(), 272u);
  CHECK(MerkleTree::children(17).empty());
  CHECK(MerkleTree::is_leaf(272));
  CHECK(!MerkleTree::is_leaf(16));
}

TEST_CASE("merkle: identical contents give identical roots; order of inserts irrelevant") {
  MerkleTree a, b;
  for (int i = 0; i < 500; ++i) a.upsert("k" + std::to_string(i), {D("v" + std::to_string(i))});
  for (int i = 499; i >= 0; --i) b.upsert("k" + std::to_string(i), {D("v" + std::to_string(i))});
  CHECK(a.node_hash(0) == b.node_hash(0));
  // Sibling digests are order-insensitive.
  MerkleTree c, d;
  c.upsert("x", {D("1"), D("2")});
  d.upsert("x", {D("2"), D("1")});
  CHECK(c.node_hash(0) == d.node_hash(0));
}

TEST_CASE("merkle: one differing key changes exactly one root->leaf path") {
  MerkleTree a, b;
  for (int i = 0; i < 1000; ++i) {
    a.upsert("k" + std::to_string(i), {D("v" + std::to_string(i))});
    b.upsert("k" + std::to_string(i), {D("v" + std::to_string(i))});
  }
  b.upsert("k77", {D("changed")});
  CHECK(!(a.node_hash(0) == b.node_hash(0)));
  int inner_diff = 0, leaf_diff = 0;
  for (uint32_t i = 1; i <= 16; ++i) inner_diff += !(a.node_hash(i) == b.node_hash(i));
  for (uint32_t i = 17; i < 273; ++i) leaf_diff += !(a.node_hash(i) == b.node_hash(i));
  CHECK_EQ(inner_diff, 1);
  CHECK_EQ(leaf_diff, 1);
  uint32_t leaf = MerkleTree::leaf_of("k77");
  CHECK(!(a.node_hash(17 + leaf) == b.node_hash(17 + leaf)));
  bool found = false;
  for (auto& kd : b.leaf_contents(leaf)) found |= kd.first == "k77";
  CHECK(found);
}

TEST_CASE("merkle: lazy recompute after updates; removal restores hash") {
  MerkleTree t;
  Digest160 empty_root = t.node_hash(0);
  t.upsert("a", {D("1")});
  Digest160 r1 = t.node_hash(0);
  CHECK(!(r1 == empty_root));
  t.upsert("a", {D("2")});
  CHECK(!(t.node_hash(0) == r1));  // dirty bits forced recompute
  t.upsert("a", {});
  CHECK(t.node_hash(0) == empty_root);
  CHECK_EQ(t.key_count(), 0u);
}
