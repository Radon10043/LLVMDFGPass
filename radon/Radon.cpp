#include <fstream>
#include <iostream>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

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
    typedef std::pair<Value *, StringRef> Node; //指令的节点
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
    void writeDFG_origin(raw_fd_ostream &File, Function &F);
    void writeDFG(raw_fd_ostream &File, Function &F);
    bool runOnModule(Module &M) override;
  };
} // namespace


char RnPass::ID = 0;


/**
 * @brief 获取指令的所在位置:"文件名:行号"
 *
 * @param I
 * @param Filename
 * @param Line
 */
static void getDebugLoc(const Instruction *I, std::string &Filename, unsigned &Line) {
#ifdef LLVM_OLD_DEBUG_API
  DebugLoc Loc = I->getDebugLoc();
  if (!Loc.isUnknown()) {
    DILocation cDILoc(Loc.getAsMDNode(M.getContext()));
    DILocation oDILoc = cDILoc.getOrigLocation();

    Line = oDILoc.getLineNumber();
    Filename = oDILoc.getFilename().str();

    if (filename.empty()) {
      Line = cDILoc.getLineNumber();
      Filename = cDILoc.getFilename().str();
    }
  }
#else
  if (DILocation *Loc = I->getDebugLoc()) {
    Line = Loc->getLine();
    Filename = Loc->getFilename().str();

    if (Filename.empty()) {
      DILocation *oDILoc = Loc->getInlinedAt();
      if (oDILoc) {
        Line = oDILoc->getLine();
        Filename = oDILoc->getFilename().str();
      }
    }
  }
#endif /* LLVM_OLD_DEBUG_API */
}


/**
 * @brief Blacklist
 *
 * @param F
 * @return true
 * @return false
 */
static bool isBlacklisted(const Function *F) {
  static const SmallVector<std::string, 8> Blacklist = {
      "asan.",
      "llvm.",
      "sancov.",
      "__ubsan_handle_",
      "free",
      "malloc",
      "calloc",
      "realloc"};

  for (auto const &BlacklistFunc : Blacklist) {
    if (F->getName().startswith(BlacklistFunc)) {
      return true;
    }
  }

  return false;
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
void RnPass::writeDFG_origin(raw_fd_ostream &File, Function &F) {
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
void RnPass::writeDFG(raw_fd_ostream &File, Function &F) {
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
  std::string dfgFilesFolder = "./dfg-files-origin";
  if (sys::fs::create_directory(dfgFilesFolder)) {
    errs() << "Could not create directory: " << dfgFilesFolder << "\n";
  }
  dfgFilesFolder = "./dfg-files";
  if (sys::fs::create_directory(dfgFilesFolder)) {
    errs() << "Could not create directory: " << dfgFilesFolder << "\n";
  }

  /* 获得源码中的函数调用信息, 表现形式为: 文件名:行号, 调用的函数 */
  std::ofstream linecalls("./dfg-files/linecalls.txt", std::ofstream::out | std::ofstream::app);

  /* 获取每个函数的dfg */
  for (auto &F : M) {
    /* Black list of function names */
    if (isBlacklisted(&F)) {
      continue;
    }

    Edges.clear();
    Nodes.clear();
    InstEdges.clear();

    errs() << "===============" << F.getName() << "===============\n";
    for (Function::iterator BB = F.begin(); BB != F.end(); BB++) { //使用迭代器遍历Function,如果用"auto& BB : F"的话后续的一些操作无法进行
      BasicBlock *CurBB = &*BB;                                    //将迭代器转换为指针(没找到能将指针转换为迭代器的方法)
      for (BasicBlock::iterator I = CurBB->begin(); I != CurBB->end(); I++) {
        Instruction *CurI = &*I;

        /* Don't worry about external libs */
        std::string filename;
        unsigned line;
        getDebugLoc(CurI, filename, line);
        static const std::string Xlibs("/usr/");
        if (!filename.compare(0, Xlibs.size(), Xlibs))
          continue;

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
        // Alloca指令用于栈空间的分配 (来源: https://llvm.org/docs/LangRef.html)
        // GetElement指令仅提供指针的计算, 并不会访问内存 (来源: https://llvm.org/docs/GetElementPtr.html)
        // Fence指令用于对内存操作进行排序 (c++才有? 来源: https://llvm.org/doxygen/classllvm_1_1FenceInst.html)
        // AtomicCmpXchg指令在内存种加载一个值并与给定的值进行比较, 如果它们相等, 会尝试将新的值存储到内存中 (来源同Alloca)
        // AtomicRMW指令: 原子指令好像只在c++或java里有(例如unordered_map), 因为被测对象是C所以不用考虑(来源同Alloca, 待验证)

        /* 获取指令中的变量名*/
        std::vector<std::string> vars;
        for (Instruction::op_iterator op = CurI->op_begin(); op != CurI->op_end(); op++) {
          std::string varName = op->get()->getName().str();
          if (!varName.empty())
            vars.push_back(varName);
        }

        /* 仅保留文件名 */
        std::size_t found = filename.find_last_of("/\\");
        if (found != std::string::npos)
          filename = filename.substr(found + 1);

        /* 获取函数调用信息 */
        if (auto *c = dyn_cast<CallInst>(CurI)) {
          if (auto *CalledF = c->getCalledFunction()) {
            if (!isBlacklisted(CalledF))
              linecalls << filename << ":" << line << "," << CalledF->getName().str() << "\n";
          }
        }

        /* 更新map,将指令和其所在位置对应起来 */
        if (filename.empty() || !line) //如果获取不到文件名或行号的话,label变为undefined
          DbgLocMap[CurI] = "undefined:0";
        else
          DbgLocMap[CurI] = filename + ":" + std::to_string(line);

        /* label中加入变量信息 */
        bool hasVar = false;
        DbgLocMap[CurI] += ":";
        if (!vars.empty()) {
          hasVar = true;
          for (auto var : vars)
            DbgLocMap[CurI] += var + ",";
        }
        if (hasVar)
          DbgLocMap[CurI].erase(DbgLocMap[CurI].end() - 1);

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

    /* 画数据流图 */
    if (!Nodes.empty()) {
      std::error_code EC;
      std::string FileName("./dfg-files-origin/dfg." + F.getName().str() + ".dot");
      raw_fd_ostream File(FileName, EC, sys::fs::F_None); //原本的文件输出

      std::string FileNameRn = "./dfg-files/dfg." + F.getName().str() + ".dot";
      raw_fd_ostream FileRn(FileNameRn, EC, sys::fs::F_None); //我的文件输出

      if (!EC) {
        writeDFG_origin(File, F);
        writeDFG(FileRn, F);
      }
      File.close();
      FileRn.close();
      errs() << "Write Done\n";
    }
  }
  return false;
}


/* 注册Pass */
static void registerRnPass(const PassManagerBuilder &, legacy::PassManagerBase &PM) {
  PM.add(new RnPass());
}
static RegisterStandardPasses RegisterRnPass(PassManagerBuilder::EP_OptimizerLast, registerRnPass);
static RegisterStandardPasses RegisterRnPass0(PassManagerBuilder::EP_EnabledOnOptLevel0, registerRnPass);