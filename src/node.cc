#include "ts_ref/node.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <vector>

namespace ts_ref {
CSTNode::~CSTNode() {
  std::vector<std::shared_ptr<CSTNode>> deletion_pool;
  std::vector<std::shared_ptr<CSTNode>> cur_children = std::move(children);
  while (cur_children.size() > 0) {
    for (std::size_t i{0}; i < cur_children.size(); i++) {
      auto child = std::move(cur_children[i]);
      if (child.use_count() == 1) {
        deletion_pool.push_back(std::move(child));
      }
    }
    cur_children.clear();
    for (auto& node_ptr : deletion_pool) {
      cur_children.insert(cur_children.end(),
                          std::make_move_iterator(node_ptr->children.begin()),
                          std::make_move_iterator(node_ptr->children.end()));
    }
    deletion_pool.clear();
  }
}

std::shared_ptr<CSTNode> CSTNode::FindLastExternalToken(
    const std::shared_ptr<CSTNode>& subtree) {
  if (!subtree || !subtree->has_external_token) return nullptr;
  std::shared_ptr<CSTNode> node = subtree;
  while (!node->children.empty()) {
    auto node_iter =
        std::find_if(node->children.rbegin(), node->children.rend(),
                     [](auto& c) { return c->has_external_token; });
    if (node_iter == node->children.rend()) break;
    node = *node_iter;
  }
  return node;
}
}  // namespace ts_ref
