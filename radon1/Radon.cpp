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
#include "llvm/Support/JSON.h"

#include "llvm/Analysis/CFG.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Use.h"
#include "llvm/IR/Value.h"

using namespace llvm;


/* 全局变量 */
std::map<std::string, std::map<std::string, std::set<std::string>>> duVarMap;                                // 存储变量的def-use信息的map: <文件名与行号, <def/use, 变量>>
std::map<Value *, std::string> dbgLocMap;                                                                    // 存储指令和其对应的在源文件中位置的map, <指令, 文件名与行号>
std::map<std::string, std::map<std::string, std::map<std::string, std::set<std::string>>>> lineCallsPreMap;  // 存储行调用关系和实参形参对应关系的map, <调用的函数, <位置, <形参, 实参(set)>>>
std::map<std::string, std::map<std::string, std::map<std::string, std::set<std::string>>>> lineCallsPostMap; // 存储行调用关系和实参形参对应关系的map, <位置, <调用的函数, <形参, 实参(set)>>>
std::map<std::string, std::set<std::string>> bbLineMap;                                                      // 存储bb和其所包含所有行的map, <bb名字, 集合(包含的所有行)>
std::map<std::string, std::string> funcEntryMap;                                                             // <函数名, 其cfg中入口BB的名字>
std::map<std::string, std::string> bbFuncMap;                                                                // <bb名, 其所在函数名>
std::map<std::string, std::string> linebbMap;                                                                // <行, 其所在bb>
std::map<std::string, int> maxLineMap;                                                                       // <filename, 文件行数>


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
 * @brief 向前搜索获得用到的变量名
 *
 * @param op
 * @param varName
 */
static void fsearchVar(Instruction::op_iterator op, std::string &varName) {

  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(op))
    varName = GV->getName().str();

  if (Instruction *Inst = dyn_cast<Instruction>(op)) {

    varName = Inst->getName().str();

    if (Inst->getOpcode() == Instruction::PHI) // ?
      return;

    for (auto nop = Inst->op_begin(); nop != Inst->op_end(); nop++)
      fsearchVar(nop, varName);
  }
}


/**
 * @brief 向前搜索获得用到的变量名和它的类型
 *
 * @param op
 * @param varName
 * @param varType
 */
static void fsearchVar(Instruction::op_iterator op, std::string &varName, Type *&varType) {

  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(op))
    varName = GV->getName().str();

  if (Instruction *Inst = dyn_cast<Instruction>(op)) {

    varName = Inst->getName().str();
    varType = Inst->getType();

    if (Inst->getOpcode() == Instruction::PHI) // ?
      return;

    for (auto nop = Inst->op_begin(); nop != Inst->op_end(); nop++)
      fsearchVar(nop, varName);
  }
}


/**
 * @brief 向前搜索, 获得函数参数对应的变量集合
 *
 * @param op
 * @param varName
 * @param vars
 */
static void fsearchCall(Instruction::op_iterator op, std::string &varName, std::set<std::string> &vars) {

  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(op))
    varName = GV->getName().str();

  if (Instruction *Inst = dyn_cast<Instruction>(op)) {

    varName = Inst->getName().str();

    for (auto op = Inst->op_begin(); op != Inst->op_end(); op++)
      fsearchCall(op, varName, vars);

  } else if (!varName.empty()) {
    vars.insert(varName);
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

  /* Def-use */
  for (auto &F : M) {

    if (isBlacklisted(&F))
      continue;

    for (auto &BB : F) {

      std::string bbname;

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

        /* 获取当前位置 */
        std::string loc = filename + ":" + std::to_string(line);

        /* 设置基本块名字 */
        if (!filename.empty() && line) {

          if (bbname.empty()) { // 若基本块名字为空时, 设置基本块名字, 并将其加入到bbFuncMap
            bbname = filename + ":" + std::to_string(line);
            bbFuncMap[bbname] = F.getName().str();
          }

          if (!bbname.empty()) { // 若基本块名字不为空, 将该行加入到map
            bbLineMap[bbname].insert(loc);
            linebbMap[loc] = bbname;
        }

          dbgLocMap[&I] = loc;
          maxLineMap[filename] = maxLineMap[filename] > line ? maxLineMap[filename] : line;
        } else
          continue;


        /* 获取函数调用信息 */
        if (auto *c = dyn_cast<CallInst>(&I)) {
          if (auto *CalledF = c->getCalledFunction()) {
            if (!isBlacklisted(CalledF)) {

              /* 按顺序获得调用函数时其形参对应的变量 */
              std::vector<std::set<std::string>> varVec;
              for (auto op = I.op_begin(); op != I.op_end(); op++) {
                std::set<std::string> vars; // 形参对应的变量可能是多个, 所以存到一个集合中
                std::string varName("");
                fsearchCall(op, varName, vars);
                varVec.push_back(vars);
              }

              /* 将函数和其参数对应的信息写入map */
              int i = 0, n = varVec.size();
              for (auto arg = CalledF->arg_begin(); arg != CalledF->arg_end(); arg++) {

                std::string argName = arg->getName().str();

                if (argName.empty())
                  continue;

                lineCallsPreMap[CalledF->getName().str()][loc][argName] = varVec[i];
                lineCallsPostMap[loc][CalledF->getName().str()][argName] = varVec[i];
                i++;
              }
            }
          }
        }


        /* 分析变量的定义-使用关系 */
        std::string varName;
        switch (I.getOpcode()) {

          case Instruction::Store: { // Store表示对内存有修改, 所以是def

            std::vector<std::string> varNames; // 存储Store指令中变量出现的顺序
            for (auto op = I.op_begin(); op != I.op_end(); op++) {
              fsearchVar(op, varName);
              varNames.push_back(varName);
            }

            int n = varNames.size(); // 根据LLVM官网的描述, n的值应该为2, 因为Store指令有两个参数, 第一个参数是要存储的值(use), 第二个指令是要存储它的地址(def)
            for (int i = 0; i < n - 1; i++) {
              if (varNames[i].empty()) // 若分析得到的变量名为空, 则不把空变量名存入map, 下同
                continue;
              duVarMap[dbgLocMap[&I]]["use"].insert(varNames[i]);
            }

            if (varNames[n - 1].empty())
              break;

            duVarMap[dbgLocMap[&I]]["def"].insert(varNames[n - 1]);

            break;
          }

          case Instruction::BitCast: { // TODO: 数组的初始化看起来和BitCast有关, 这么判断数组的def可以吗?

            for (auto op = I.op_begin(); op != I.op_end(); op++)
              fsearchVar(op, varName);

            if (varName.empty())
              break;

            duVarMap[dbgLocMap[&I]]["def"].insert(varName);

            break;
          }

          case Instruction::Load: { // load表示从内存中读取, 所以是use

            for (auto op = I.op_begin(); op != I.op_end(); op++)
              fsearchVar(op, varName);

            if (varName.empty())
              break;

            duVarMap[dbgLocMap[&I]]["use"].insert(varName);

            break;
          }

          case Instruction::Call: { // 调用函数时用到的变量也加入到def-use的map中

            Type *varType = I.getType();

            for (auto op = I.op_begin(); op != I.op_end(); op++) {
              fsearchVar(op, varName, varType);

              if (varName.empty())
                continue;

              if (varType->isPointerTy()) { // 如果是指针传递, 则认为 def,use 都有
                duVarMap[dbgLocMap[&I]]["def"].insert(varName);
                duVarMap[dbgLocMap[&I]]["use"].insert(varName);
              } else {
                duVarMap[dbgLocMap[&I]]["use"].insert(varName);
              }
            }

            break;
          }
        }
      }
    }
  }

  /* 将duVarMap转换为json并输出 */
  std::error_code EC;
  raw_fd_ostream duVarJson(outDirectory + "/duVar.json", EC, sys::fs::F_None);
  json::OStream duVarJ(duVarJson);
  duVarJ.objectBegin();
  for (auto it = duVarMap.begin(); it != duVarMap.end(); it++) { // 遍历map并转换为json, llvm的json似乎不会自动格式化?
    duVarJ.attributeBegin(it->first);
    duVarJ.objectBegin();
    for (auto iit = it->second.begin(); iit != it->second.end(); iit++) {
      duVarJ.attributeBegin(iit->first);
      duVarJ.arrayBegin();
      for (auto var : iit->second) {
        size_t found = var.find(".addr");
        if (found != std::string::npos)
          var = var.substr(0, found);
        duVarJ.value(var);
      }
      duVarJ.arrayEnd();
      duVarJ.attributeEnd();
    }
    duVarJ.objectEnd();
    duVarJ.attributeEnd();
  }
  duVarJ.objectEnd();

  /* 将bbLineMap转为json并输出 */
  raw_fd_ostream bbLineJson(outDirectory + "/bbLine.json", EC, sys::fs::F_None);
  json::OStream bbLineJ(bbLineJson);
  bbLineJ.objectBegin();
  for (auto it = bbLineMap.begin(); it != bbLineMap.end(); it++) {
    bbLineJ.attributeBegin(it->first);
    bbLineJ.arrayBegin();
    for (auto line : it->second)
      bbLineJ.value(line);
    bbLineJ.arrayEnd();
    bbLineJ.attributeEnd();
  }
  bbLineJ.objectEnd();

  /* 将linebbMap转为json并输出 */
  raw_fd_ostream linebbJson(outDirectory + "/linebb.json", EC, sys::fs::F_None);
  json::OStream linebbJ(linebbJson);
  linebbJ.objectBegin();
  for (auto pss : linebbMap) {
    linebbJ.attributeBegin(pss.first);
    linebbJ.value(pss.second);
    linebbJ.attributeEnd();
  }
  linebbJ.objectEnd();

  /* 将maxLineMap转为json并输出 */
  raw_fd_ostream maxLineJson(outDirectory + "/maxLine.json", EC, sys::fs::F_None);
  json::OStream maxLineJ(maxLineJson);
  maxLineJ.objectBegin();
  for (auto psi : maxLineMap) {
    maxLineJ.attributeBegin(psi.first);
    maxLineJ.value(psi.second);
    maxLineJ.attributeEnd();
  }
  maxLineJ.objectEnd();

  /* 将lineCallsPreMap转换为json并输出 */
  raw_fd_ostream lineCallsPreJson(outDirectory + "/lineCallsPre.json", EC, sys::fs::F_None);
  json::OStream lineCallsPreJ(lineCallsPreJson);
  lineCallsPreJ.objectBegin();
  for (auto it1 = lineCallsPreMap.begin(); it1 != lineCallsPreMap.end(); it1++) { // 遍历map并转换为json, llvm的json似乎不会自动格式化?
    lineCallsPreJ.attributeBegin(it1->first);
    lineCallsPreJ.objectBegin();
    for (auto it2 = it1->second.begin(); it2 != it1->second.end(); it2++) {
      lineCallsPreJ.attributeBegin(it2->first);
      lineCallsPreJ.objectBegin();
      for (auto it3 = it2->second.begin(); it3 != it2->second.end(); it3++) {
        lineCallsPreJ.attributeBegin(it3->first);
        lineCallsPreJ.arrayBegin();
        for (auto var : it3->second)
          lineCallsPreJ.value(var);
        lineCallsPreJ.arrayEnd();
        lineCallsPreJ.attributeEnd();
      }
      lineCallsPreJ.objectEnd();
      lineCallsPreJ.attributeEnd();
    }
    lineCallsPreJ.objectEnd();
    lineCallsPreJ.attributeEnd();
  }
  lineCallsPreJ.objectEnd();

  /* 将lineCallsPostMap转换为json并输出 */
  raw_fd_ostream lineCallsPostJson(outDirectory + "/lineCallsPost.json", EC, sys::fs::F_None);
  json::OStream lineCallsPostJ(lineCallsPostJson);
  lineCallsPostJ.objectBegin();
  for (auto it1 = lineCallsPostMap.begin(); it1 != lineCallsPostMap.end(); it1++) { // 遍历map并转换为json, llvm的json似乎不会自动格式化?
    lineCallsPostJ.attributeBegin(it1->first);
    lineCallsPostJ.objectBegin();
    for (auto it2 = it1->second.begin(); it2 != it1->second.end(); it2++) {
      lineCallsPostJ.attributeBegin(it2->first);
      lineCallsPostJ.objectBegin();
      for (auto it3 = it2->second.begin(); it3 != it2->second.end(); it3++) {
        lineCallsPostJ.attributeBegin(it3->first);
        lineCallsPostJ.arrayBegin();
        for (auto var : it3->second)
          lineCallsPostJ.value(var);
        lineCallsPostJ.arrayEnd();
        lineCallsPostJ.attributeEnd();
      }
      lineCallsPostJ.objectEnd();
      lineCallsPostJ.attributeEnd();
    }
    lineCallsPostJ.objectEnd();
    lineCallsPostJ.attributeEnd();
  }
  lineCallsPostJ.objectEnd();


  /* 将bbFuncMap转换为json并输出 */
  raw_fd_ostream bbFuncJson(outDirectory + "/bbFunc.json", EC, sys::fs::F_None);
  json::OStream bbFuncJ(bbFuncJson);
  bbFuncJ.objectBegin();
  for (auto it = bbFuncMap.begin(); it != bbFuncMap.end(); it++) { // 遍历map并转换为json, llvm的json似乎不会自动格式化?
    bbFuncJ.attributeBegin(it->first);
    bbFuncJ.value(it->second);
    bbFuncJ.attributeEnd();
  }
  bbFuncJ.objectEnd();

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

      /* Get entry BB */
      funcEntryMap[F.getName().str()] = F.getEntryBlock().getName().str();

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

  /* 将funcEntryMap转换为json并输出 */
  raw_fd_ostream funcEntryJson(outDirectory + "/funcEntry.json", EC, sys::fs::F_None);
  json::OStream funcEntryJ(funcEntryJson);
  funcEntryJ.objectBegin();
  for (auto it = funcEntryMap.begin(); it != funcEntryMap.end(); it++) { // 遍历map并转换为json, llvm的json似乎不会自动格式化?
    funcEntryJ.attributeBegin(it->first);
    funcEntryJ.value(it->second);
    funcEntryJ.attributeEnd();
  }
  funcEntryJ.objectEnd();

  return false;
}


/* 注册Pass */
static void registerRnDuPass(const PassManagerBuilder &, legacy::PassManagerBase &PM) {
  PM.add(new RnDuPass());
}
static RegisterStandardPasses RegisterRnDuPass(PassManagerBuilder::EP_OptimizerLast, registerRnDuPass);
static RegisterStandardPasses RegisterRnDuPass0(PassManagerBuilder::EP_EnabledOnOptLevel0, registerRnDuPass);