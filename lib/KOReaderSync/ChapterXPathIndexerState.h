#pragma once

#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

#include "ChapterXPathIndexerInternal.h"

namespace ChapterXPathIndexerInternal {

// Shared parser state used by both forward and reverse mappers.
// It centralizes DOM-stack bookkeeping and XPath reconstruction so each mapper
// only implements its own match/emit logic.

struct StackNode {
  std::string tag;
  int index = 1;
  // Text-node bookkeeping that mirrors crengine's DOM (rules R1-R5 in ChapterXPathForwardMapper.cpp).
  int textNodeCount = 0;            // text nodes materialised so far under this element
  size_t codepointsInTextNode = 0;  // collapsed codepoints of the open (or provisional) run so far
  bool hasText = false;             // reverse mapper: the element has had visible text
  bool inTextNode = false;          // a node is open: visible text since the last child boundary
  bool pendingWhitespace = false;   // a whitespace-only run is open and not yet a node
  bool lastWasSpace = false;        // the open run ends in whitespace (R2 carry across chunks)
  bool childSeen = false;           // crengine childCount != 0: an element or a materialised node
  bool blockChildSeen = false;      // a block child has started: R3 applies from here on
  bool prevSiblingIsBlock = false;  // the last child boundary was a block element's end
};

struct StackState {
  int skipDepth = -1;
  // Open <pre> elements around the cursor: inside one, text is stored raw (R5).
  int preDepth = 0;
  size_t totalTextBytes = 0;
  std::vector<StackNode> stack;
  // Sibling-name → count map per parent depth. Index `d` holds the counts for
  // children that live at depth `d` in the DOM (i.e. queried just before
  // pushing a new node). Entries are cleared lazily on push rather than popped
  // and reallocated, so the per-element heap churn stays bounded.
  std::vector<std::unordered_map<std::string, int>> siblingCounters;

  StackState() {
    // Pre-size for typical EPUB chapter nesting (well below 32 levels). Avoids
    // per-element vector growth that would otherwise interleave with map node
    // allocations.
    stack.reserve(32);
    siblingCounters.resize(32);
  }

  void pushElement(const char* rawName) {
    const size_t depth = stack.size();
    if (siblingCounters.size() <= depth) {
      siblingCounters.resize(depth + 1);
    }
    // Lowercase the tag in place into the StackNode's own storage — the prior
    // implementation called toLowerStr() which returned a fresh std::string
    // per element, a major fragmentation source. Lookup into the parent's
    // sibling counter map then uses the stable in-place string with no extra
    // allocation.
    StackNode& node = stack.emplace_back();
    node.tag.assign(rawName ? rawName : "");
    for (char& c : node.tag) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const bool block = isBlockTag(node.tag);
    if (depth > 0) {
      StackNode& parent = stack[depth - 1];  // after emplace_back: the vector may have moved
      closeTextRun(parent, block, false);
      if (block) parent.blockChildSeen = true;
    }
    if (node.tag == "pre") preDepth++;
    const int sibIdx = ++siblingCounters[depth][node.tag];
    node.index = sibIdx;
    if (skipDepth < 0 && isSkippableTag(node.tag)) {
      skipDepth = static_cast<int>(stack.size()) - 1;
    }
  }

  void popElement() {
    if (stack.empty()) {
      return;
    }
    if (skipDepth == static_cast<int>(stack.size()) - 1) {
      skipDepth = -1;
    }
    // Clear the just-departed element's child-counter slot in place rather
    // than freeing the map: the next sibling at this depth needs an empty map
    // either way, and reusing the existing buckets avoids per-pop allocator
    // churn. We don't shrink siblingCounters for the same reason.
    const size_t childDepth = stack.size();
    if (childDepth < siblingCounters.size()) {
      siblingCounters[childDepth].clear();
    }
    closeTextRun(stack.back(), false, true);
    const bool block = isBlockTag(stack.back().tag);
    if (stack.back().tag == "pre") preDepth--;
    stack.pop_back();
    if (!stack.empty()) {
      StackNode& parent = stack.back();
      parent.childSeen = true;
      parent.prevSiblingIsBlock = block;
      parent.inTextNode = false;
      parent.pendingWhitespace = false;
      parent.lastWasSpace = false;
    }
  }

  struct TextRun {
    bool counts;              // false: whitespace-only and still provisional, no node yet
    int nodeIndex;            // 1-based text() index of the node the chunk belongs to
    size_t codepointsBefore;  // collapsed codepoints of that node before this chunk
    bool spaceBefore;         // the node's text before this chunk ends in whitespace
  };

  // Account for one character-data chunk under stack.back(). Called for EVERY chunk inside <body>
  // that shouldSkipText() lets through, whitespace-only ones included.
  TextRun onTextChunk(const char* text, const int len) {
    StackNode& n = stack.back();
    const bool collapse = preDepth == 0;
    if (!n.inTextNode) {
      if (isWhitespaceOnly(text, len)) {
        if (!n.pendingWhitespace) {
          n.pendingWhitespace = true;
          n.codepointsInTextNode = 0;
          n.lastWasSpace = false;
        }
        n.codepointsInTextNode += collapsedCodepoints(text, len, collapse, n.lastWasSpace);
        return {false, n.textNodeCount + 1, 0, false};
      }
      // Visible text: the run is a node, and a provisional whitespace prefix is part of it.
      n.inTextNode = true;
      n.textNodeCount++;
      n.childSeen = true;
      if (!n.pendingWhitespace) {
        n.codepointsInTextNode = 0;
        n.lastWasSpace = false;
      }
      n.pendingWhitespace = false;
    }
    const TextRun run{true, n.textNodeCount, n.codepointsInTextNode, n.lastWasSpace};
    n.codepointsInTextNode += collapsedCodepoints(text, len, collapse, n.lastWasSpace);
    return run;
  }

  // A child boundary under `parent`: a child element starts (`nextIsBlock` says which kind) or the
  // parent itself ends. Settles a provisional whitespace-only run the way crengine's DOM does.
  void closeTextRun(StackNode& parent, const bool nextIsBlock, const bool parentEnds) {
    if (parent.pendingWhitespace) {
      // R1 (ldomElementWriter::onText): the first whitespace-only run of a non-pre block is dropped.
      const bool firstOfBlock = !parent.childSeen && isBlockTag(parent.tag) && preDepth == 0;
      // R3 (removeStandaloneWhitespaceTextChildrenInMixedContent): in mixed content a whitespace-only
      // node survives only between two inline-ish siblings.
      const bool mixed = parent.blockChildSeen || nextIsBlock;
      const bool betweenInlines = parent.childSeen && !parent.prevSiblingIsBlock && !nextIsBlock && !parentEnds;
      if (!firstOfBlock && !(mixed && !betweenInlines)) {
        parent.textNodeCount++;
        parent.childSeen = true;
      }
      parent.pendingWhitespace = false;
    }
    parent.inTextNode = false;
    parent.lastWasSpace = false;
  }

  void onCharData(const char*, int) {}

  int bodyIdx() const {
    for (int i = static_cast<int>(stack.size()) - 1; i >= 0; i--) {
      if (stack[i].tag == "body") {
        return i;
      }
    }
    return -1;
  }

  bool insideBody() const { return bodyIdx() >= 0; }

  // Out-parameter form: appends the path into `out` without freeing it first
  // so the caller controls when to reuse vs reset capacity. Use this in hot
  // paths to amortise the underlying allocation.
  void buildCurrentXPath(const int spineIndex, std::string& out) const {
    out.clear();
    out.append("/body/DocFragment[");
    appendInt(out, spineIndex + 1);
    out.append("]/body");
    const int bi = bodyIdx();
    if (bi < 0) {
      return;
    }
    for (size_t i = static_cast<size_t>(bi + 1); i < stack.size(); i++) {
      out.push_back('/');
      out.append(stack[i].tag);
      out.push_back('[');
      appendInt(out, stack[i].index);
      out.push_back(']');
    }
  }

  std::string currentXPath(const int spineIndex) const {
    std::string out;
    buildCurrentXPath(spineIndex, out);
    return out;
  }

  bool shouldSkipText(const int len) const { return skipDepth >= 0 || len <= 0 || !insideBody(); }

 private:
  // Appends a non-negative int as decimal digits without allocating a temp
  // std::string (std::to_string would allocate per call).
  static void appendInt(std::string& out, int value) {
    if (value < 0) {
      out.push_back('-');
      value = -value;
    }
    char buf[12];
    int len = 0;
    if (value == 0) {
      buf[len++] = '0';
    } else {
      while (value > 0) {
        buf[len++] = static_cast<char>('0' + (value % 10));
        value /= 10;
      }
    }
    while (len-- > 0) {
      out.push_back(buf[len]);
    }
  }
};

template <typename StateT>
void parserStartCb(void* ud, const char* name, const char**) {
  static_cast<StateT*>(ud)->onStartElement(name);
}

template <typename StateT>
void parserEndCb(void* ud, const char*) {
  static_cast<StateT*>(ud)->onEndElement();
}

template <typename StateT>
void parserCharCb(void* ud, const char* text, const int len) {
  static_cast<StateT*>(ud)->onCharData(text, len);
}

template <typename StateT>
void parserDefaultCb(void* ud, const char* text, const int len) {
  if (isEntityRef(text, len)) {
    static_cast<StateT*>(ud)->onCharData(text, len);
  }
}

}  // namespace ChapterXPathIndexerInternal
