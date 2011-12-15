#ifndef COMP_TABLE
#define COMP_TABLE

#include <memory>
#include <vector>
#include <list>
#include <istream>
#include <string>

#include "Tree.hh"
#include "Properties.hh"
#include "SymCode.hh"
#include "SymProb.hh"

class HuffmanTable
{
public:
  typedef std::unique_ptr<Node> Node_ptr;
  typedef std::list<Node*> HeadList;
  typedef std::vector<Node_ptr>NodeVec;

  struct ItPair
  {
    typedef HeadList::iterator It;
    It low_;
    It high_;
    
    It left() { return high_; }
    It right() { return low_; }
    
    ItPair():low_(),high_() { }
    ItPair(It higher, It lower):low_(lower),high_(higher) { }
  };

  typedef ItPair (*Algo)(HeadList&);

  HuffmanTable(std::istream& training_set, size_t countmax);
  HuffmanTable(ADCCountVec const& training_set, size_t countmax);

  void extractTable(SymTable& out) const;
  void writeTable(std::string const& filename) const;
  void print(std::ostream& ost) const
  { heads_.front()->print(ost); }

private:
  void initNodes(SymsVec const&);
  void initHeads();
  void constructTree();
  void makeTable(ADCCountVec const&, size_t countmax);

  NodeVec nodes_;
  HeadList heads_;
};

inline std::ostream& operator<<(std::ostream& ost, HuffmanTable const& h)
{
  h.print(ost);
  return ost;
}

#endif
