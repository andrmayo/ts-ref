#ifndef TS_REF_NODE_H_
#define TS_REF_NODE_H_

// A Node holds the parser's persistent output:
// every time the parser shifts a token, it wraps the token into a leaf node
// when parser reduces, an interior node gets created

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ts_ref/types.h"

namespace ts_ref {

// Once a CSTNode is created, it's never mutated,
// hence we can point to const CSTNode
// note also that the GSS is acyclic by construction, hence we don't need to
// worry about cycles with pointer references
// Note that CSTNode combines Subtree and TSNode from tree-sitter
class CSTNode {
 public:
  // TODO: implement constructor with lexer in mind
  CSTNode() = default;
  ~CSTNode();

  Symbol symbol = 0;
  // CSTNode's position gets compused as if measured from a local (0, 0) origin
  Length size;
  std::vector<std::shared_ptr<CSTNode>> children;
  // whitespace prior to token, for leaf nodes gets set by Lexer
  // for interior nodes (built during a reduce), padding = children[0].padding
  // when reduce operation happens, interior node must be constructed to hold
  // size = sum(size(child) for child in node.children)
  Length padding = Length(0, 0, 0);
  DynamicPrecedenceType dynamic_precedence = 0;
  // Which of the parent rule's productions built this node. A child's alias
  // and field name are properties of the *parent's* production step, not of
  // the child, so reading the tree needs this to look them up. Meaningless on
  // leaves.
  std::uint32_t production_id = 0;
  bool has_external_token = false;
  // for hidden rules
  bool visible = true;
  // for named string-literals: if true, print as rule name, otherwise print
  // quoted literal text
  bool named = false;
  // for extras rule in grammar
  bool extra = false;
  // if CSTNode has external token
  std::string external_scanner_state;

  // Note that Length.Add is non-commutative
  Length SubtreeTotalSize() const { return padding.Add(size); }

  static std::shared_ptr<CSTNode> FindLastExternalToken(
      const std::shared_ptr<CSTNode>& subtree);
};
}  // namespace ts_ref

#endif
