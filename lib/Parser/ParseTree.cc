#include "Cocktail/Parser/ParseTree.h"

#include <cstdlib>

#include "Cocktail/Diagnostics/DiagnosticEmitter.h"
#include "Cocktail/Lexer/TokenKind.h"
#include "Cocktail/Parser/ParseNodeKind.h"
#include "Cocktail/Parser/ParserImpl.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/Sequence.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/iterator.h"

namespace Cocktail {

auto ParseTree::Parse(TokenizedBuffer& tokens, DiagnosticEmitter& emitter)
    -> ParseTree {
  return Parser::Parse(tokens, emitter);
}

auto ParseTree::Postorder() const -> llvm::iterator_range<PostorderIterator> {
  return {PostorderIterator(Node(0)),
          PostorderIterator(Node(node_impls.size()))};
}

auto ParseTree::Postorder(Node n) const
    -> llvm::iterator_range<PostorderIterator> {
  int end_index = n.index + 1;
  int start_index = end_index - node_impls[n.index].subtree_size;
  return {PostorderIterator(Node(start_index)),
          PostorderIterator(Node(end_index))};
}

auto ParseTree::Children(Node n) const
    -> llvm::iterator_range<SiblingIterator> {
  int end_index = n.index - node_impls[n.index].subtree_size;
  return {SiblingIterator(*this, Node(n.index - 1)),
          SiblingIterator(*this, Node(end_index))};
}

auto ParseTree::Roots() const -> llvm::iterator_range<SiblingIterator> {
  return {SiblingIterator(*this, Node(static_cast<int>(node_impls.size()) - 1)),
          SiblingIterator(*this, Node(-1))};
}

auto ParseTree::HasErrorInNode(Node n) const -> bool {
  return node_impls[n.index].has_error;
}

auto ParseTree::GetNodeKind(Node n) const -> ParseNodeKind {
  return node_impls[n.index].kind;
}

auto ParseTree::GetNodeToken(Node n) const -> TokenizedBuffer::Token {
  return node_impls[n.index].token;
}

auto ParseTree::GetNodeText(Node n) const -> llvm::StringRef {
  return tokens->GetTokenText(node_impls[n.index].token);
}

auto ParseTree::Print(llvm::raw_ostream& output) const -> void {
  output << "[\n";

  llvm::SmallVector<std::pair<Node, int>, 16> node_stack;
  for (Node n : Roots()) {
    node_stack.push_back({n, 0});
  }

  while (!node_stack.empty()) {
    Node n;
    int depth;
    std::tie(n, depth) = node_stack.pop_back_val();
    const auto& n_impl = node_impls[n.GetIndex()];

    for (int unused_indent : llvm::seq(0, depth)) {
      (void)unused_indent;
      output << "  ";
    }

    output << "{node_index: " << n.index << ", kind: '" << n_impl.kind.GetName()
           << "', text: '" << tokens->GetTokenText(n_impl.token) << "'";

    if (n_impl.has_error) {
      output << ", has_error: yes";
    }

    if (n_impl.subtree_size > 1) {
      output << ", subtree_size: " << n_impl.subtree_size;
      output << ", children: [\n";
      for (Node sibling_n : Children(n)) {
        node_stack.push_back({sibling_n, depth + 1});
      }
      continue;
    }

    assert(n_impl.subtree_size == 1 &&
           "Subtree size must always be a positive integer!");
    output << "}";

    int next_depth = node_stack.empty() ? 0 : node_stack.back().second;
    assert(next_depth <= depth && "Cannot have the next depth increase!");
    for (int close_children_count : llvm::seq(0, depth - next_depth)) {
      (void)close_children_count;
      output << "]}";
    }
    output << ",\n";
  }
  output << "]\n";
}

auto ParseTree::Verify() const -> bool {
  // Verify basic tree structure invariants.
  llvm::SmallVector<ParseTree::Node, 16> ancestors;
  for (Node n : llvm::reverse(Postorder())) {
    const auto& n_impl = node_impls[n.GetIndex()];

    if (n_impl.has_error && !has_errors) {
      llvm::errs()
          << "Node #" << n.GetIndex()
          << " has errors, but the tree is not marked as having any.\n";
      return false;
    }

    if (n_impl.subtree_size > 1) {
      if (!ancestors.empty()) {
        auto parent_n = ancestors.back();
        const auto& parent_n_impl = node_impls[parent_n.GetIndex()];
        int end_index = n.GetIndex() - n_impl.subtree_size;
        int parent_end_index = parent_n.GetIndex() - parent_n_impl.subtree_size;
        if (parent_end_index > end_index) {
          llvm::errs() << "Node #" << n.GetIndex() << " has a subtree size of "
                       << n_impl.subtree_size
                       << " which extends beyond its parent's (node #"
                       << parent_n.GetIndex() << ") subtree (size "
                       << parent_n_impl.subtree_size << ")\n";
          return false;
        }
      }
      // Has children, so we descend.
      ancestors.push_back(n);
      continue;
    }

    if (n_impl.subtree_size < 1) {
      llvm::errs() << "Node #" << n.GetIndex()
                   << " has an invalid subtree size of " << n_impl.subtree_size
                   << "!\n";
      return false;
    }

    int next_index = n.GetIndex() - 1;
    while (!ancestors.empty()) {
      ParseTree::Node parent_n = ancestors.back();
      if ((parent_n.GetIndex() -
           node_impls[parent_n.GetIndex()].subtree_size) != next_index) {
        break;
      }
      ancestors.pop_back();
    }
  }
  if (!ancestors.empty()) {
    llvm::errs()
        << "Finished walking the parse tree and there are still ancestors:\n";
    for (Node ancestor_n : ancestors) {
      llvm::errs() << "  Node #" << ancestor_n.GetIndex() << "\n";
    }
    return false;
  }

  return true;
}

}  // namespace Cocktail