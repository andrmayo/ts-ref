#include "ts_ref/stack.h"

#include <cstddef>
#include <iterator>
#include <memory>

#include "absl/log/check.h"

#include "ts_ref/node.h"
#include "ts_ref/types.h"

namespace ts_ref {

// true for equality
static bool CmpExternalTokens(const std::shared_ptr<CSTNode>& left_ptr,
                              const std::shared_ptr<CSTNode>& right_ptr) {
  // handles forks where last_external_token is the same common ancestor,
  // as well has where neither branch has an external token (both are nullptr)
  if (left_ptr == right_ptr) {
    return true;
  };
  if (left_ptr == nullptr || right_ptr == nullptr) {
    return false;
  }
  if (!left_ptr->has_external_token && !right_ptr->has_external_token) {
    return true;
  }
  if (left_ptr->has_external_token && right_ptr->has_external_token) {
    return left_ptr->external_scanner_state ==
           right_ptr->external_scanner_state;
  }
  return false;
}

static bool SubtreeIsEquivalent(const std::shared_ptr<CSTNode>& left,
                                const std::shared_ptr<CSTNode>& right) {
  if (left == right) return true;
  if (!left || !right) return false;
  return left->symbol == right->symbol && left->padding == right->padding &&
         left->size == right->size &&
         left->children.size() == right->children.size() &&
         left->extra == right->extra && CmpExternalTokens(left, right);
}

bool StackHead::operator==(const StackHead& other) const {
  return GetPosition().bytes == other.GetPosition().bytes &&
         CmpExternalTokens(last_external_token, other.last_external_token) &&
         (*(node_ptr) == *(other.node_ptr));
}

StackNode::~StackNode() {
  // iteratively delete all StackNodes
  // and StackLinks that would have ref count == 0
  // after deletion completes
  std::vector<std::shared_ptr<StackNode>> deletion_pool;
  std::vector<StackLink> cur_links = std::move(links);

  while (cur_links.size() > 0) {
    auto cur_link = std::move(cur_links.back());
    cur_links.pop_back();
    if (cur_link.node_ptr.use_count() == 1) {
      deletion_pool.push_back(std::move(cur_link.node_ptr));
    }
    if (cur_links.size() == 0) {
      for (auto& node_ptr : deletion_pool) {
        cur_links.insert(cur_links.end(),
                         std::make_move_iterator(node_ptr->links.begin()),
                         std::make_move_iterator(node_ptr->links.end()));
      }
      deletion_pool.clear();
    }
  }
}

bool StackNode::operator==(const StackNode& other) const {
  return state_id == other.state_id && position == other.position;
}

void Stack::Push(StackVersion version, std::shared_ptr<CSTNode> subtree_ptr,
                 StateId state) {
  auto head_ptr = GetHead(version);
  auto new_node_ptr =
      std::make_shared<StackNode>(state, *(head_ptr->node_ptr), *subtree_ptr);
  auto link = StackLink{.node_ptr = head_ptr->node_ptr,
                        .subtree = std::move(subtree_ptr)};
  new_node_ptr->links.push_back(link);
  head_ptr->node_ptr = new_node_ptr;
}

std::shared_ptr<CSTNode> Stack::GetLastExternalToken(StackVersion version) {
  DCHECK_LT(version, heads_.size());
  return heads_[version]->last_external_token;
}

void Stack::SetLastExternalToken(StackVersion version,
                                 std::shared_ptr<CSTNode> token) {
  DCHECK_LT(version, heads_.size());
  heads_[version]->last_external_token = std::move(token);
}

std::shared_ptr<StackHead> Stack::GetHead(StackVersion version) {
  DCHECK_LT(version, heads_.size());
  return heads_[version];
}

// if pop_all == true, count is ignored
std::vector<Stack::PopResult> Stack::RecursePop(
    std::shared_ptr<StackNode> stack_node_ptr, std::uint32_t count,
    bool pop_all) {
  if ((!pop_all && count == 0) || stack_node_ptr->links.size() == 0) {
    // it should be an invariant that PopCount never gets called
    // with count > Stack depth
    // even with fanning branches as we move rootward, this holds,
    // since if two branches merged they must have had identical (state,
    // position), and state encodes symbol-count-so-far
    DCHECK(pop_all || count == 0);
    return std::vector<PopResult>{
        PopResult{.node_ptr = stack_node_ptr, .subtrees = {}}};
  }
  std::vector<PopResult> pop_results;
  for (auto pop_link : stack_node_ptr->links) {
    // An `extras` subtree (whitespace, a comment) sits on the stack between
    // real children but is not one of them: a production of N symbols must pop
    // N *non-extra* entries. So extras are collected into the result without
    // being counted against the goal. Mirrors tree-sitter's stack__iter, which
    // only increments subtree_count for non-extra links.
    const bool is_extra =
        pop_link.subtree != nullptr && pop_link.subtree->extra;
    const std::uint32_t remaining = (pop_all || is_extra) ? count : count - 1;
    std::vector<PopResult> new_pop =
        RecursePop(pop_link.node_ptr, remaining, pop_all);
    for (auto& result : new_pop) {
      result.subtrees.push_back(pop_link.subtree);
    }
    pop_results.insert(pop_results.end(),
                       std::make_move_iterator(new_pop.begin()),
                       std::make_move_iterator(new_pop.end()));
  }
  return pop_results;
}

std::vector<StackSlice> Stack::BuildSlices(std::vector<PopResult> pop_results,
                                           StackVersion original_version) {
  std::vector<StackSlice> stack_slices;
  // to build return vector, iterate through pop_results;
  // PopCount doesn't mutate existing heads, but does add a head
  // for each unique StackNode at the base of the recursion
  // If two paths have the same base StackNode, only one head
  // gets added (deduplication is done via linear sweep)
  StackVersion version_tracker = static_cast<StackVersion>(heads_.size());

  for (std::size_t i{0}; i < pop_results.size(); i++) {
    StackVersion cur_version = version_tracker;
    PopResult& pop = pop_results[i];
    std::shared_ptr<StackNode> cur_stack_node_ptr = pop.node_ptr;
    for (std::size_t j{0}; j < i; j++) {
      auto prev_pop = pop_results[j];
      if (pop.node_ptr == prev_pop.node_ptr) {
        cur_version = stack_slices[j].version;
        break;
      }
    }
    StackSlice new_slice{.version = cur_version};
    if (cur_version == version_tracker) {
      auto new_head = std::make_shared<StackHead>(cur_stack_node_ptr);
      // A reduction does not rewind the external scanner, so the new head
      // resumes from the same scanner state the popped version had. Without
      // this a stateful scanner -- an indent stack, say -- silently resets on
      // every reduce. Mirrors tree-sitter's ts_stack__add_version.
      new_head->last_external_token =
          heads_[original_version]->last_external_token;
      heads_.push_back(std::move(new_head));
      version_tracker++;
    }
    new_slice.subtrees.insert(new_slice.subtrees.end(),
                              std::make_move_iterator(pop.subtrees.begin()),
                              std::make_move_iterator(pop.subtrees.end()));
    stack_slices.push_back(new_slice);
  }

  return stack_slices;
}

// Note that PopCount doesn't mutate the Stack directly,
// and is a traversal rather than a pop in the usual sense
std::vector<StackSlice> Stack::PopCount(StackVersion version,
                                        std::uint32_t count) {
  std::vector<PopResult> pop_results =
      Stack::RecursePop(heads_[version]->node_ptr, count);

  return BuildSlices(pop_results, version);
}

std::vector<StackSlice> Stack::PopAll(StackVersion version) {
  std::vector<PopResult> pop_results =
      Stack::RecursePop(heads_[version]->node_ptr, 0, true);
  return BuildSlices(pop_results, version);
}

StackVersion Stack::CopyVersion(StackVersion version) {
  DCHECK_LT(version, heads_.size());
  heads_.push_back(std::make_shared<StackHead>(*heads_[version]));
  return static_cast<StackVersion>(heads_.size() - 1);
}

bool Stack::Merge(StackVersion left, StackVersion right) {
  DCHECK_NE(left, right);
  DCHECK_LT(left, heads_.size());
  DCHECK_LT(right, heads_.size());

  if (!CanMerge(left, right)) {
    return false;
  }

  // ensure that the recently created version gets removed
  if (right < left) {
    left += right;
    right = left - right;
    left -= right;
  }

  // for a merge to make sense, the left and right StackHeads
  // must have links
  DCHECK_GT(heads_[left]->node_ptr->links.size(), 0);
  DCHECK_GT(heads_[right]->node_ptr->links.size(), 0);

  auto& left_node_ptr = heads_[left]->node_ptr;
  auto& right_node_ptr = heads_[right]->node_ptr;
  StackNode& left_stack_node = *(left_node_ptr);
  auto left_link_size = left_node_ptr->links.size();
  for (auto& link : right_node_ptr->links) {
    // check for equal subtree, in which case deduplicate
    auto link_it = std::find_if(
        left_node_ptr->links.begin(),
        left_node_ptr->links.begin() + left_link_size, [&link](auto& l) {
          return l.node_ptr == link.node_ptr &&
                 (l.subtree == link.subtree ||
                  SubtreeIsEquivalent(l.subtree, link.subtree));
        });
    if (link_it != left_node_ptr->links.begin() + left_link_size) {
      if (link.subtree->dynamic_precedence >
          link_it->subtree->dynamic_precedence) {
        link_it->subtree = link.subtree;
      }
      continue;
    }
    left_stack_node.links.push_back(link);
  }
  RemoveVersion(right);
  return true;
}

bool Stack::CanMerge(StackVersion left, StackVersion right) {
  DCHECK_LT(left, heads_.size());
  DCHECK_LT(right, heads_.size());
  DCHECK_NE(left, right);
  StackHead left_head = *(heads_[left]);
  StackHead right_head = *(heads_[right]);

  return !(left_head.halted || right_head.halted) && left_head == right_head;
}

void Stack::RemoveVersion(StackVersion version) {
  DCHECK_LT(version, heads_.size());
  heads_.erase(heads_.begin() + version);
}

}  // namespace ts_ref
