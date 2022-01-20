#include <iostream>
#include <list>
#include <string>
#include <unordered_map>

#include "llvm/IR/Function.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/CFGPrinter.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"

#include "llvm/Analysis/CFG.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Use.h"
#include "llvm/IR/Value.h"
using namespace llvm;


namespace {
  class RnPass : public ModulePass {
  public:
    typedef std::pair<Value *, StringRef> Node; //指令的节点. v是?
    typedef std::pair<Node, Node> Edge;         //指令与指令之间的边,用于表示控制流?
    typedef std::list<Node> NodeList;           //指令的集合
    typedef std::list<Edge> EdgeList;           //边的集合

    EdgeList InstEdges; //存储每条指令的先后执行顺序,用于表示控制流?
    EdgeList Edges;     //存储数据流的边
    NodeList Nodes;     //存储每一条指令
    int Num;            //计数

    std::unordered_map<Value *, std::string> DbgLocMap; //记录指令及其对应的源文件位置

    static char ID;
    RnPass()
        : ModulePass(ID) {
      Num = 0;
    }

    StringRef getValueName(Value *V);
    void writeDFG(raw_fd_ostream &File, Function &F);
    void writeDFGRn(raw_fd_ostream &File, Function &F);
    bool runOnModule(Module &M) override;
  };
} // namespace


char RnPass::ID = 0;


/**
 * @brief 获取指令的所在位置:"文件名:行号"
 *
 * @param I 指令
 */
static void getDebugLoc(const Instruction *I, std::string &DbgFileName, unsigned &Line) {
  if (DILocation *Loc = I->getDebugLoc()) {
    Line = Loc->getLine();
    DbgFileName = Loc->getFilename().str();
    std::size_t Found = DbgFileName.find_last_of("/\\");
    if (Found != std::string::npos)
      DbgFileName = DbgFileName.substr(Found + 1);
    errs() << DbgFileName << ":" << Line << "\n";
  }
}


/**
 * @brief 如果是变量则获得变量的名字,是指令则获得指令的内容
 *
 * @param V
 * @return StringRef
 */
StringRef RnPass::getValueName(Value *V) {
  std::string TempResult = "val";
  if (!V)
    return "undefined";

  if (V->getName().empty()) {
    TempResult += std::to_string(Num);
    Num++;
  } else {
    TempResult = V->getName().str();
  }

  StringRef Result(TempResult);
  // errs() << "Result: " << Result << "\n";
  return Result;
}


/**
 * @brief 画数据流图-origin
 *
 * @param File
 * @param F
 */
void RnPass::writeDFG(raw_fd_ostream &File, Function &F) {
  /* 根据边的统计情况画图 */
  File << "digraph \"DFG for \'" + F.getName() + "\' function\" {\n";
  /* Dump Node */
  for (NodeList::iterator it = Nodes.begin(); it != Nodes.end(); it++) {
    if (dyn_cast<Instruction>(it->first))
      File << "\tNode" << it->first << "[shape=record, label=\"" << *(it->first) << "\"];\n";
    else
      File << "\tNode" << it->first << "[shape=record, label=\"" << it->second << "\"];\n";
  }
  /* Dump control flow */
  for (EdgeList::iterator it = InstEdges.begin(); it != InstEdges.end(); it++) {
    File << "\tNode" << it->first.first << " -> Node" << it->second.first << "\n";
  }
  /*Dump data flow*/
  File << "edge [color=red]"
       << "\n";
  for (EdgeList::iterator it = Edges.begin(); it != Edges.end(); it++) {
    File << "\tNode" << it->first.first << " -> Node" << it->second.first << "\n";
  }
  File << "}\n";
  errs() << "Write Done\n";
}


/**
 * @brief 画数据流图-Radon
 *
 * @param File
 * @param F
 */
void RnPass::writeDFGRn(raw_fd_ostream &File, Function &F) {
  /* 根据边的统计情况画图 */
  File << "digraph \"DFG for \'" + F.getName() + "\' function\" {\n";
  /* Dump Node */
  for (NodeList::iterator it = Nodes.begin(); it != Nodes.end(); it++) {
    if (dyn_cast<Instruction>(it->first))
      File << "\tNode" << it->first << "[shape=record, label=\"" << DbgLocMap[it->first] << "\"];\n";
    else
      File << "\tNode" << it->first << "[shape=record, label=\"" << it->second << "\"];\n";
  }
  /*Dump data flow*/
  for (EdgeList::iterator it = Edges.begin(); it != Edges.end(); it++) {
    File << "\tNode" << it->first.first << " -> Node" << it->second.first << " [color=red]\n";
  }
  File << "}\n";
  errs() << "Write Done\n";
}


/**
 * @brief 重写runOnModule,在编译被测对象的过程中获取数据流图
 *
 * @param M
 * @return true
 * @return false
 */
bool RnPass::runOnModule(Module &M) {
  /* 创建存储dfg图的文件夹 */
  std::string dfgFilesFolder = "./dfg-files";
  if (sys::fs::create_directory(dfgFilesFolder)) {
    errs() << "Could not create directory: " << dfgFilesFolder << "\n";
  }
  dfgFilesFolder = "./dfg-files-rn";
  if (sys::fs::create_directory(dfgFilesFolder)) {
    errs() << "Could not create directory: " << dfgFilesFolder << "\n";
  }

  /* 获取每个函数的dfg */
  for (auto &F : M) {
    std::error_code EC;
    std::string FileName("./dfg-files/dfg." + F.getName().str() + ".dot");
    raw_fd_ostream File(FileName, EC, sys::fs::F_None); //原本的文件输出

    std::string FileNameRn = "./dfg-files-rn/dfg." + F.getName().str() + ".dot";
    raw_fd_ostream FileRn(FileNameRn, EC, sys::fs::F_None); //我的文件输出

    Edges.clear();
    Nodes.clear();
    InstEdges.clear();

    errs() << "===============" << F.getName() << "===============\n";
    for (Function::iterator BB = F.begin(); BB != F.end(); BB++) { //使用迭代器遍历Function,如果用"auto& BB : F"的话后续的一些操作无法进行
      BasicBlock *CurBB = &*BB;                                    //将迭代器转换为指针(没找到能将指针转换为迭代器的方法)
      for (BasicBlock::iterator I = CurBB->begin(); I != CurBB->end(); I++) {
        Instruction *CurI = &*I;
        switch (CurI->getOpcode()) { //根据博客所述,在IR中只有load和store指令直接与内存接触,所以通过它们获取数据流的边
          case Instruction::Load: {
            LoadInst *LInst = dyn_cast<LoadInst>(CurI);     // dyn_cast用于检查操作数是否属于指定类型,在这里是检查CurI是否属于LoadInst型.如果是的话就返回指向它的指针,不是的话返回空指针
            Value *LoadValPtr = LInst->getPointerOperand(); //获取指针操作数?获取指向的操作数?
            errs() << "---------------Load---------------\n";
            errs() << *LoadValPtr << " -> " << *CurI << "\n";
            errs() << "----------------------------------\n";
            Edges.push_back(Edge(Node(LoadValPtr, getValueName(LoadValPtr)), Node(CurI, getValueName(CurI))));
            break;
          }
          case Instruction::Store: {
            StoreInst *SInst = dyn_cast<StoreInst>(CurI);
            Value *StoreValPtr = SInst->getPointerOperand();
            Value *StoreVal = SInst->getValueOperand();
            errs() << "----------Store----------\n";
            errs() << *StoreVal << " -> " << *CurI << "\n";
            errs() << *CurI << " -> " << *StoreValPtr << "\n";
            errs() << "-------------------------\n";
            Edges.push_back(Edge(Node(StoreVal, getValueName(StoreVal)), Node(CurI, getValueName(CurI))));
            Edges.push_back(Edge(Node(CurI, getValueName(CurI)), Node(StoreValPtr, getValueName(StoreValPtr))));
            break;
          }
          default: { //对于其他指令,遍历每一个指令的操作数,判断其是不是一个指令,如果是一个指令的话就添加相应的边
            for (Instruction::op_iterator op = CurI->op_begin(); op != CurI->op_end(); op++) {
              if (dyn_cast<Instruction>(*op)) { //这里和数据流有关?
                Edges.push_back(Edge(Node(op->get(), getValueName(op->get())), Node(CurI, getValueName(CurI))));
              }
            }
            break;
          }
        }

        /* 更新map,将指令和其所在位置对应起来 */
        // TODO: DbgFileName为空时...
        std::string DbgFileName("RnInit");
        unsigned Line = 0;
        getDebugLoc(CurI, DbgFileName, Line); //获取指令所在的文件名与行号
        DbgLocMap[CurI] = DbgFileName + ":" + std::to_string(Line);

        BasicBlock::iterator Next = I;
        Nodes.push_back(Node(CurI, getValueName(CurI)));
        Next++;
        if (Next != CurBB->end()) //这里是在统计控制流的边
          InstEdges.push_back(Edge(Node(CurI, getValueName(CurI)), Node(&*Next, getValueName(&*Next))));
      }
      Instruction *Terminator = CurBB->getTerminator();
      for (BasicBlock *SucBB : successors(CurBB)) {
        Instruction *First = &*(SucBB->begin());
        InstEdges.push_back(Edge(Node(Terminator, getValueName(Terminator)), Node(First, getValueName(First))));
      }
    }

    /* Debugging ... */
    // for (std::unordered_map<Value *, std::string>::iterator it = DbgLocMap.begin(); it != DbgLocMap.end(); it++) {
    //   errs() << *(it->first) << ":" << it->second << "\n";
    // }

    /* 画数据流图 */
    writeDFG(File, F);
    writeDFGRn(FileRn, F);
    File.close();
    FileRn.close();
    errs() << "Write Done\n";
  }
  return false;
}


/* 注册Pass */
static void registerRnPass(const PassManagerBuilder &, legacy::PassManagerBase &PM) {
  PM.add(new RnPass());
}
static RegisterStandardPasses RegisterRnPass(PassManagerBuilder::EP_OptimizerLast, registerRnPass);
static RegisterStandardPasses RegisterRnPass0(PassManagerBuilder::EP_EnabledOnOptLevel0, registerRnPass);