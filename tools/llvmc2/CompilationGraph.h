//===--- CompilationGraph.h - The LLVM Compiler Driver ----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open
// Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  Compilation graph - definition.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVMC2_COMPILATION_GRAPH_H
#define LLVM_TOOLS_LLVMC2_COMPILATION_GRAPH_H

#include "AutoGenerated.h"
#include "Tool.h"

#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/iterator"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/System/Path.h"

#include <string>

namespace llvmc {

  // An edge of the compilation graph.
  class Edge : public llvm::RefCountedBaseVPTR<Edge> {
  public:
    Edge(const std::string& T) : ToolName_(T) {}
    virtual ~Edge() {};

    const std::string& ToolName() const { return ToolName_; }
    virtual unsigned Weight() const = 0;
  private:
    std::string ToolName_;
  };

  // Edges that have no properties are instances of this class.
  class SimpleEdge : public Edge {
  public:
    SimpleEdge(const std::string& T) : Edge(T) {}
    unsigned Weight() const { return 1; }
  };

  // A node of the compilation graph.
  struct Node {
    // A Node holds a list of the outward edges.
    typedef llvm::SmallVector<llvm::IntrusiveRefCntPtr<Edge>, 3> container_type;
    typedef container_type::iterator iterator;
    typedef container_type::const_iterator const_iterator;

    Node() : OwningGraph(0), InEdges(0) {}
    Node(CompilationGraph* G) : OwningGraph(G), InEdges(0) {}
    Node(CompilationGraph* G, Tool* T) :
      OwningGraph(G), ToolPtr(T), InEdges(0) {}

    bool HasChildren() const { return !OutEdges.empty(); }
    const std::string Name() const
    { return ToolPtr ? ToolPtr->Name() : "root"; }

    // Iteration.
    iterator EdgesBegin() { return OutEdges.begin(); }
    const_iterator EdgesBegin() const { return OutEdges.begin(); }
    iterator EdgesEnd() { return OutEdges.end(); }
    const_iterator EdgesEnd() const { return OutEdges.end(); }

    // Add an outward edge. Takes ownership of the Edge object.
    void AddEdge(Edge* E)
    { OutEdges.push_back(llvm::IntrusiveRefCntPtr<Edge>(E)); }

    // Inward edge counter. Used to implement topological sort.
    // TOTHINK: Move the mutable counter back into Tool classes? Makes
    // us more const-correct.
    void IncrInEdges() { ++InEdges; }
    void DecrInEdges() { --InEdges; }
    bool HasNoInEdges() const { return InEdges == 0; }

    // Needed to implement NodeChildIterator/GraphTraits
    CompilationGraph* OwningGraph;
    // The corresponding Tool.
    // WARNING: ToolPtr can be NULL (for the root node).
    llvm::IntrusiveRefCntPtr<Tool> ToolPtr;
    // Links to children.
    container_type OutEdges;
    // Inward edge counter. Updated in
    // CompilationGraph::insertEdge(). Used for topological sorting.
    unsigned InEdges;
  };

  class NodesIterator;

  // The compilation graph itself.
  class CompilationGraph {
    // Main data structure.
    typedef llvm::StringMap<Node> nodes_map_type;
    // These are used to map from language names to tools. (We can
    // have several tools associated with each language name, hence
    // the need for a vector of Edges.)
    typedef
    llvm::SmallVector<llvm::IntrusiveRefCntPtr<Edge>, 3> tools_vector_type;
    typedef llvm::StringMap<tools_vector_type> tools_map_type;

    // Map from file extensions to language names.
    LanguageMap ExtsToLangs;
    // Map from language names to lists of tool names.
    tools_map_type ToolsMap;
    // Map from tool names to Tool objects.
    nodes_map_type NodesMap;

  public:

    CompilationGraph();

    // insertVertex - insert a new node into the graph. Takes
    // ownership of the object.
    void insertNode(Tool* T);

    // insertEdge - Insert a new edge into the graph. Takes ownership
    // of the Edge object.
    void insertEdge(const std::string& A, Edge* E);

    // Build - Build target(s) from the input file set. Command-line
    // options are passed implicitly as global variables.
    int Build(llvm::sys::Path const& tempDir);

    // Return a reference to the node correponding to the given tool
    // name. Throws std::runtime_error.
    Node& getNode(const std::string& ToolName);
    const Node& getNode(const std::string& ToolName) const;

    // viewGraph - This function is meant for use from the debugger.
    // You can just say 'call G->viewGraph()' and a ghostview window
    // should pop up from the program, displaying the compilation
    // graph. This depends on there being a 'dot' and 'gv' program
    // in your path.
    void viewGraph();

    // Write a CompilationGraph.dot file.
    void writeGraph();

    // GraphTraits support
    friend NodesIterator GraphBegin(CompilationGraph*);
    friend NodesIterator GraphEnd(CompilationGraph*);
    friend void PopulateCompilationGraph(CompilationGraph&);

  private:
    // Helper functions.

    // Find out which language corresponds to the suffix of this file.
    const std::string& getLanguage(const llvm::sys::Path& File) const;

    // Return a reference to the list of tool names corresponding to
    // the given language name. Throws std::runtime_error.
    const tools_vector_type& getToolsVector(const std::string& LangName) const;

    // Pass the input file through the toolchain.
    void PassThroughGraph (const llvm::sys::Path& In, const Node* StartNode,
                           const llvm::sys::Path& TempDir) const;

    // Find head of the toolchain corresponding to the given file.
    const Node* FindToolChain(const llvm::sys::Path& In,
                              const std::string* forceLanguage) const;

    // Sort the nodes in topological order.
    void TopologicalSort(std::vector<const Node*>& Out);
    // Call TopologicalSort and filter the resulting list to include
    // only Join nodes.
    void TopologicalSortFilterJoinNodes(std::vector<const Node*>& Out);
  };

  /// GraphTraits support code.

  // Auxiliary class needed to implement GraphTraits support.  Can be
  // generalised to something like value_iterator for map-like
  // containers.
  class NodesIterator : public llvm::StringMap<Node>::iterator {
    typedef llvm::StringMap<Node>::iterator super;
    typedef NodesIterator ThisType;
    typedef Node* pointer;
    typedef Node& reference;

  public:
    NodesIterator(super I) : super(I) {}

    inline reference operator*() const {
      return super::operator->()->second;
    }
    inline pointer operator->() const {
      return &super::operator->()->second;
    }
  };

  inline NodesIterator GraphBegin(CompilationGraph* G) {
    return NodesIterator(G->NodesMap.begin());
  }

  inline NodesIterator GraphEnd(CompilationGraph* G) {
    return NodesIterator(G->NodesMap.end());
  }


  // Another auxiliary class needed by GraphTraits.
  class NodeChildIterator : public bidirectional_iterator<Node, ptrdiff_t> {
    typedef NodeChildIterator ThisType;
    typedef Node::container_type::iterator iterator;

    CompilationGraph* OwningGraph;
    iterator EdgeIter;
  public:
    typedef Node* pointer;
    typedef Node& reference;

    NodeChildIterator(Node* N, iterator I) :
      OwningGraph(N->OwningGraph), EdgeIter(I) {}

    const ThisType& operator=(const ThisType& I) {
      assert(OwningGraph == I.OwningGraph);
      EdgeIter = I.EdgeIter;
      return *this;
    }

    inline bool operator==(const ThisType& I) const
    { return EdgeIter == I.EdgeIter; }
    inline bool operator!=(const ThisType& I) const
    { return EdgeIter != I.EdgeIter; }

    inline pointer operator*() const {
      return &OwningGraph->getNode((*EdgeIter)->ToolName());
    }
    inline pointer operator->() const {
      return &OwningGraph->getNode((*EdgeIter)->ToolName());
    }

    ThisType& operator++() { ++EdgeIter; return *this; } // Preincrement
    ThisType operator++(int) { // Postincrement
      ThisType tmp = *this;
      ++*this;
      return tmp;
    }

    inline ThisType& operator--() { --EdgeIter; return *this; }  // Predecrement
    inline ThisType operator--(int) { // Postdecrement
      ThisType tmp = *this;
      --*this;
      return tmp;
    }

  };
}

namespace llvm {
  template <>
  struct GraphTraits<llvmc::CompilationGraph*> {
    typedef llvmc::CompilationGraph GraphType;
    typedef llvmc::Node NodeType;
    typedef llvmc::NodeChildIterator ChildIteratorType;

    static NodeType* getEntryNode(GraphType* G) {
      return &G->getNode("root");
    }

    static ChildIteratorType child_begin(NodeType* N) {
      return ChildIteratorType(N, N->OutEdges.begin());
    }
    static ChildIteratorType child_end(NodeType* N) {
      return ChildIteratorType(N, N->OutEdges.end());
    }

    typedef llvmc::NodesIterator nodes_iterator;
    static nodes_iterator nodes_begin(GraphType *G) {
      return GraphBegin(G);
    }
    static nodes_iterator nodes_end(GraphType *G) {
      return GraphEnd(G);
    }
  };

}

#endif // LLVM_TOOLS_LLVMC2_COMPILATION_GRAPH_H
