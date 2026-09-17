#ifndef TS_REF_STACK_H_
#define TS_REF_STACK_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "ts_ref/node.h"
#include "ts_ref/types.h"

namespace ts_ref {

struct StackNode;

// the StackNode pointer is the prior node on the Stack,
// the CSTNode pointer is the subtree formed by a shift or reduce
// operation. if shift, that subtree is a leaf node,
// otherwise an actual tree. The StackNode is roughly "root-ward",
// the CSTNode, "leaf-ward".
struct StackLink {
  std::shared_ptr<StackNode> node_ptr;
  std::shared_ptr<CSTNode> subtree;
};

// unlike CSTNode, stacknode points backwards (i.e. towards the root)
// also, note that 'links' has no maximum size (unlike in tree-sitter)
class StackNode {
 public:
  StackNode() = default;
  StackNode(StateId id, Length pos) : state_id{id}, position{pos} {}
  StackNode(StateId new_state, const StackNode& prev_stack_node,
            const CSTNode& new_cst_node)
      : state_id{new_state},
        position{
            prev_stack_node.position.Add(new_cst_node.SubtreeTotalSize())} {}
  // Non-recursive destructor for StackNode
  ~StackNode();

  bool operator==(const StackNode& other) const;

  StateId state_id;
  // Unlike with CSTNode.size, position keeps track of absolute position
  Length position;
  std::vector<StackLink> links;
  // accumulated precedence used by StackMerge, which keeps the higher
  // precedence
  DynamicPrecedenceType dynamic_precedence = 0;
};

struct StackSlice {
  std::vector<std::shared_ptr<CSTNode>> subtrees;
  StackVersion version;
};

struct StackHead {
  // no-arg constructor assumes initial state
  StackHead() : node_ptr{std::make_shared<StackNode>(1, Length(0, 0, 0))} {}
  // transfers ownership of the shared_ptr<StackNode>
  StackHead(std::shared_ptr<StackNode> new_node_ptr)
      : node_ptr{std::move(new_node_ptr)} {}

  bool operator==(const StackHead& other) const;

  StateId GetState() const { return node_ptr->state_id; }

  Length GetPosition() const { return node_ptr->position; }

  std::shared_ptr<StackNode> node_ptr;
  // equivalent to three-way enum 'status' in TS
  bool halted = false;
  std::shared_ptr<CSTNode> last_external_token = nullptr;
};

class Stack {
 public:
  explicit Stack() : heads_{std::make_shared<StackHead>()} {};
  // The generated parse table names its own start state, which need not be the
  // default StackHead's state 1.
  explicit Stack(StateId initial_state)
      : heads_{std::make_shared<StackHead>(
            std::make_shared<StackNode>(initial_state, Length(0, 0, 0)))} {};

  // Prevent copying of pointers to resources
  Stack(const Stack&) = delete;
  Stack& operator=(const Stack&) = delete;

  // get the state at the top of a version
  // if nothing has been pushed to stack, starting version with have initial
  // state (1)
  StateId GetState(StackVersion version) { return heads_[version]->GetState(); }

  // get number of versions
  StackVersion GetVersionCount() { return heads_.size(); }

  // Get last external token associated with a version
  std::shared_ptr<CSTNode> GetLastExternalToken(StackVersion version);

  void SetLastExternalToken(StackVersion version,
                            std::shared_ptr<CSTNode> token);

  // Get the position of the given version of the stack within document
  Length GetPosition(StackVersion version) {
    return heads_[version]->GetPosition();
  }

  std::shared_ptr<StackHead> GetHead(StackVersion version);

  // Push a tree and state onto the given version of the stack.
  //
  // This transfers ownership of the tree to the Stack. Callers that
  // need to retain ownership of the tree for their own purposes should
  // first retain the tree.
  // In tree-sitter, the equivalent function takes a bool pending argument
  // to mark provisional subtrees, for the sake of error recovery and
  // incremental parsing
  void Push(StackVersion version, std::shared_ptr<CSTNode> subtree_ptr,
            StateId state);

  // Pop the given number of entries from the given version of the stack. This
  // operation can increase the number of stack versions if pop ends of nodes
  // that had merged.
  // The vector it returns collects all the PopResult structs
  // and creates a StackSlice for each one, with the same subtrees as the
  // corresponding PopResult, but with .version guaranteed to be the same
  // for all StackSlice structs generated from PopResult pointing to the same
  // StackNode.
  // The number of StackSlices in the return vector is the number of PopResults,
  // but the number of distinct .version values in these StackSlices
  // is the number of distinct StackNodes at the end of the recursion
  // A diamond-shape of StackLinks, for instance, with count = 2,
  // will return a vector of 2 StackSlices, but they'll have the same version
  // (which should be the same version as the head prior to popping)
  std::vector<StackSlice> PopCount(StackVersion version, std::uint32_t count);

  // Remove all trees from the given version
  std::vector<StackSlice> PopAll(StackVersion version);

  // appends a new version to heads_ that's a shallow copy of heads[version]
  StackVersion CopyVersion(StackVersion version);

  // returns true if merge is successful, false otherwise
  // Merge removes the newer head over the older head
  bool Merge(StackVersion left, StackVersion right);

  // note: all versions to the right of version
  // have stale references after RemoveVersion
  void RemoveVersion(StackVersion version);

 private:
  // version objects are unsigned integers that index into heads
  std::vector<std::shared_ptr<StackHead>> heads_;

  // purely for handling recursive calls for pop operations
  // a PopResult gives all the CSTNode subtrees formed with a given StackNode at
  // the root
  struct PopResult {
    std::shared_ptr<StackNode> node_ptr;
    std::vector<std::shared_ptr<CSTNode>> subtrees;
  };

  // recursive helper function for pop operations
  static std::vector<PopResult> RecursePop(
      std::shared_ptr<StackNode> stack_node_ptr, uint32_t count,
      bool pop_all = false);

  // helper function for PopCount and PopAll to get StackSlices from PopResults
  // original_version is the version being popped: the heads this creates
  // inherit its last_external_token, since a reduction does not reset the
  // external scanner's state
  std::vector<StackSlice> BuildSlices(std::vector<PopResult>,
                                      StackVersion original_version);

  // checks if two versions meet conditions for merge
  bool CanMerge(StackVersion left, StackVersion right);
};
}  // namespace ts_ref

#endif
