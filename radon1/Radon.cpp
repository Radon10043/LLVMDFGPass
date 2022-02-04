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


/* 全局变量 */
std::map<std::string, std::map<std::string, std::set<std::string>>> duVarMap; // 存储变量的def-use信息的map: <文件名与行号, <def/use, 变量>>
std::map<Value *, std::string> dbgLocMap;                                     // 存储指令和其对应的在源文件中位置的map, <指令, 文件名与行号>


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

    std::string getNodeDescription(BasicBlock *Node, Function *Graph) {
      std::set<std::string> varDescSet; // 存储当前基本块中变量的def/use信息
      std::string nodeDesc;             // 该节点的描述内容, 实际上是varDescSet的所有内容合并, 用回车隔开
      for (auto I = Node->begin(); I != Node->end(); I++) {

        /* 获取当前指令对应的源文件位置 */
        std::string dbgLoc = dbgLocMap[&*I];
        if (dbgLoc == "undefined") // 跳过undefined节点
          continue;

        /* 收集当前指令def的变量信息 */
        std::string desc = dbgLocMap[&*I] + "-def:";
        for (auto &var : duVarMap[dbgLoc]["def"]) {

          if (var.empty()) // 变量名为空的话就跳过
            continue;

          size_t found = var.find(".addr"); // 若变量名中有".addr"的话就去掉 (通常发生在参数传递的情况中)
          if (found != std::string::npos)
            desc += var.substr(0, found) + ",";
          else
            desc += var + ",";
        }
        if (*(desc.end() - 1) == ',')
          desc.erase(desc.end() - 1);

        /* 收集当前指令use的变量信息 */
        desc += "-use:";
        for (auto var : duVarMap[dbgLoc]["use"]) {
          if (var.empty()) // 变量名为空的话就跳过
            continue;

          size_t found = var.find(".addr"); // 若变量名中有".addr"的话就去掉 (通常发生在参数传递的情况中)
          if (found != std::string::npos)
            desc += var.substr(0, found) + ",";
          else
            desc += var + ",";
        }
        if (*(desc.end() - 1) == ',')
          desc.erase(desc.end() - 1);

        varDescSet.insert(desc);
      }

      /* 合并节点中的变量信息描述 */
      for (auto desc : varDescSet)
        nodeDesc += desc + "\n";
      nodeDesc.erase(nodeDesc.end() - 1);

      return nodeDesc;
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
 * @brief 向前搜索获得调用函数时用到的变量
 *
 * @param I
 * @param vec
 */
static void fsearch(Instruction::op_iterator op, std::string &varName) {

  if (Instruction *Inst = dyn_cast<Instruction>(op)) {

    varName = Inst->getName().str();

    for (auto op = Inst->op_begin(); op != Inst->op_end(); op++)
      fsearch(op, varName);
  }
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

  /* 在linecalls.txt中写入调用信息 */
  std::ofstream linecalls(outDirectory + "/linecalls.txt", std::ofstream::out | std::ofstream::app);

  /* Def-use */
  for (auto &F : M) {
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

        /* 获取函数调用信息 */
        if (auto *c = dyn_cast<CallInst>(&I)) {
          if (auto *CalledF = c->getCalledFunction()) {
            if (!isBlacklisted(CalledF)) {

              /* 按顺序获得调用函数时其形参对应的变量 */
              std::vector<std::string> varVec;
              for (auto op = I.op_begin(); op != I.op_end(); op++) {
                std::string varName("");
                fsearch(op, varName);
                varVec.push_back(varName);
              }

              /* 将函数调用信息与参数对应情况写入文件 */
              int i = 0, n = varVec.size();
              linecalls << filename << ":" << line << "-" << CalledF->getName().str() << "-";
              for (auto arg = CalledF->arg_begin(); arg != CalledF->arg_end(); arg++) {
                std::string argName = arg->getName().str();
                linecalls << argName << ":";
                if (i < n)
                  linecalls << varVec[i];
                if (arg + 1 != CalledF->arg_end())
                  linecalls << ",";
                i++;
              }
              linecalls << "\n";
            }
          }
        }

        /* 将指令和对应的源文件中的位置存入map */
        if (filename.empty() || !line)
          dbgLocMap[&I] = "undefined";
        else
          dbgLocMap[&I] = filename + ":" + std::to_string(line);

        /* 分析变量的定义-使用关系 */
        std::string varName;
        switch (I.getOpcode()) {

          case Instruction::Store: { // Store表示对内存有修改, 所以是def

            std::vector<std::string> varNames; // 存储Store指令中变量出现的顺序
            for (auto op = I.op_begin(); op != I.op_end(); op++) {
              fsearch(op, varName);
              varNames.push_back(varName);
            }

            int n = varNames.size(); // 根据LLVM官网的描述, n的值应该为2, 因为Store指令有两个参数, 第一个参数是要存储的值(use), 第二个指令是要存储它的地址(def)
            for (int i = 0; i < n - 1; i++)
              duVarMap[dbgLocMap[&I]]["use"].insert(varNames[i]);
            duVarMap[dbgLocMap[&I]]["def"].insert(varNames[n - 1]);

            break;
          }

          case Instruction::BitCast: { // TODO: 数组的初始化看起来和BitCast有关, 这么判断数组的def可以吗?

            for (auto op = I.op_begin(); op != I.op_end(); op++)
              fsearch(op, varName);
            duVarMap[dbgLocMap[&I]]["def"].insert(varName);

            break;
          }

          case Instruction::Load: { // load表示从内存中读取, 所以是use

            for (auto op = I.op_begin(); op != I.op_end(); op++)
              fsearch(op, varName);
            duVarMap[dbgLocMap[&I]]["use"].insert(varName);

            break;
          }
        }
      }
    }
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
  return false;
}


/* 注册Pass */
static void registerRnDuPass(const PassManagerBuilder &, legacy::PassManagerBase &PM) {
  PM.add(new RnDuPass());
}
static RegisterStandardPasses RegisterRnDuPass(PassManagerBuilder::EP_OptimizerLast, registerRnDuPass);
static RegisterStandardPasses RegisterRnDuPass0(PassManagerBuilder::EP_EnabledOnOptLevel0, registerRnDuPass);