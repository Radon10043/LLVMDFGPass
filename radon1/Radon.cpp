#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
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


namespace llvm {
  template <>
  struct DOTGraphTraits<Function *> : public DefaultDOTGraphTraits {
    DOTGraphTraits(bool isSimple = true)
        : DefaultDOTGraphTraits(isSimple) {}

    static std::string getGraphName(Function *F) {
      return "CFG for '" + F->getName().str() + "' function";
    }

    std::string getNodeLabel(BasicBlock *Node, Function *Graph) {
      if (!Node->getName().empty()) {
        return Node->getName().str();
      }

      std::string Str;
      raw_string_ostream OS(Str);

      Node->printAsOperand(OS, false);
      return OS.str();
    }

    std::string getNodeDescription(BasicBlock* Node, Function* Graph) {
      return "Write description here";
    }
  };
} // namespace llvm


namespace {
  class RnDuPass : public ModulePass {
  public:
    static char ID;
    RnDuPass()
        : ModulePass(ID) {}

    bool runOnModule(Module &M) override;
  };
} // namespace


char RnDuPass::ID = 0;


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
 * @brief 重写runOnModule,在编译被测对象的过程中获取数据流图
 *
 * @param M
 * @return true
 * @return false
 */
bool RnDuPass::runOnModule(Module &M) {

  /* 创建文件夹存储输出内容 */
  std::string outDirectory = "./radon1/out-files";
  if (sys::fs::create_directory(outDirectory)) {
    errs() << "Could not create directory: " << outDirectory << "\n";
  }

  /* CFG */
  for (auto &F : M) {

    bool hasBB = false;
    if (isBlacklisted(&F))
      continue;

    for (auto &BB : F) {
      std::string bbName("");
      for (auto &I : BB) {
        /* 跳过external libs */
        std::string filename;
        unsigned line;
        getDebugLoc(&I, filename, line);
        static const std::string Xlibs("/usr/");
        if (!filename.compare(0, Xlibs.size(), Xlibs))
          continue;

        /* 仅保留文件名与行号 */
        std::size_t found = filename.find_last_of("/\\");
        if (found != std::string::npos)
          filename = filename.substr(found + 1);

        /* 设置基本块名称 */
        if (bbName.empty() && !filename.empty() && line) {
          bbName = filename + ":" + std::to_string(line);
        }
      }

      /* 设置基本块名称 */
      if (!bbName.empty()) {
        BB.setName(bbName + ":");
        if (!BB.hasName()) {
          std::string newname = bbName + ":";
          Twine t(newname);
          SmallString<256> NameData;
          StringRef NameRef = t.toStringRef(NameData);
          MallocAllocator Allocator;
          BB.setValueName(ValueName::Create(NameRef, Allocator));
        }
        hasBB = true;
      }
    }

    if (hasBB) {
      /* Print CFG */
      std::error_code EC;
      std::string cfgFileName = outDirectory + "/cfg." + F.getName().str() + ".dot";
      raw_fd_ostream cfg(cfgFileName, EC, sys::fs::F_None);
      if (!EC) {
        WriteGraph(cfg, &F, true);
        cfg.close();
      }
    }
  }

  /* DFG */
  for (auto &F : M) {

    if (isBlacklisted(&F))
      continue;
    std::map<Value *, std::set<Value *>> duMap; // 存储def-use关系的map, key是起点, value是终点的集合
    std::map<Value *, std::string> dbgLocMap;   // 存储指令和其对应的在源文件中位置的map, key是指令, value是文件名+行号
    std::set<Value *> nodes;                    // 存储所有节点的集合

    for (auto &BB : F) {
      for (auto &I : BB) {
        /* 跳过external libs */
        std::string filename;
        unsigned line;
        getDebugLoc(&I, filename, line);
        static const std::string Xlibs("/usr/");
        if (!filename.compare(0, Xlibs.size(), Xlibs))
          continue;

        /* 仅保留文件名与行号 */
        std::size_t found = filename.find_last_of("/\\");
        if (found != std::string::npos)
          filename = filename.substr(found + 1);

        /* 将指令和对应的源文件中的位置存入map */
        if (filename.empty() || !line)
          dbgLocMap[&I] = "undefined";
        else
          dbgLocMap[&I] = filename + ":" + std::to_string(line);

        /* 遍历def-use链 */
        nodes.insert(&I);
        for (auto U : I.users()) {
          if (Instruction *inst = dyn_cast<Instruction>(U)) {
            duMap[&I].insert(inst);
          }
        }
      }
    }

    /* 画图 */
    if (!nodes.empty()) {
      std::error_code EC;
      std::string dfgname(outDirectory + "/dfg." + F.getName().str() + ".dot");
      raw_fd_ostream dfg(dfgname, EC, sys::fs::F_None);

      if (!EC) {
        dfg << "digraph \"DFG for \'" + F.getName() + "\' function\" {\n";
        dfg << "\tlabel=\"DFG for \'" + F.getName() + "\' function\";\n\n";

        for (auto node : nodes) {
          dfg << "\tNode" << node << "[shape=record, label=\"" << dbgLocMap[node] << "\"];\n";
          // dfg << "\tNode" << node << "[shape=record, label=\"" << dbgLocMap[node] << "\", comment=\"" << *node << "\"];\n";
        }

        for (auto chains : duMap)
          for (auto end : chains.second)
            dfg << "\tNode" << chains.first << " -> Node" << end << " [color=red];\n";

        dfg << "}\n";
        errs() << "Write Done\n";
      }

      dfg.close();
    }
  }
  return false;
}


/* 注册Pass */
static void registerRnDuPass(const PassManagerBuilder &, legacy::PassManagerBase &PM) {
  PM.add(new RnDuPass());
}
static RegisterStandardPasses RegisterRnDuPass(PassManagerBuilder::EP_OptimizerLast, registerRnDuPass);
static RegisterStandardPasses RegisterRnDuPass0(PassManagerBuilder::EP_EnabledOnOptLevel0, registerRnDuPass);