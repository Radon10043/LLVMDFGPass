'''
Author: Radon
Date: 2022-02-05 16:20:42
LastEditors: Radon
LastEditTime: 2022-04-04 15:28:57
Description: Hi, say something
'''
from ast import arguments
import pydot
import argparse
import os
import sys
import json
import heapq
import functools

import networkx as nx

from queue import Queue, PriorityQueue

# Global
DU_VAR_DICT = dict()  # <行, <def/use, {变量}>>
BB_LINE_DICT = dict()  # <bb名, 它所包含的所有行>
BB_FUNC_DICT = dict()  # <bb名, 它所在的函数>
FUNC_ENTRY_DICT = dict()  # <函数名, 它的入口BB名字>
LINE_CALLS_PRE_DICT = dict()  # <调用的函数, <行, <形参, {实参}>>>
LINE_CALLS_POST_DICT = dict() # <行, <调用的函数, <形参, {实参}>>>
LINE_BB_DICT = dict()  # <行, 其所在基本块>
MAX_LINE_DICT = dict()


class MyNode:

    def __init__(self, distance, nodeName, nodeLabel):
        self.distance = distance
        self.name = nodeName
        self.label = nodeLabel

    def __lt__(self, other):
        return self.distance < other.distance


def myCmp(x, y):
    """自定义排序, 行号大的排在前面

    Parameters
    ----------
    x : str
        filename:line
    y : str
        filename:line

    Returns
    -------
    int
        返回值必须是-1, 0, 1, 不能是True/False

    Notes
    -----
    _description_
    """
    x = int(x.split(":")[1])
    y = int(y.split(":")[1])

    if x > y:
        return -1
    elif x < y:
        return 1
    return 0


def isPreTainted(bbname: str, preSet: set, defSet: set, useSet: set):
    """查看该基本块是否被前向污染了

    Parameters
    ----------
    bbname : str
        基本块名称
    preSet : set
        前向污点分析时的变量集合
    defSet : set
        存储了各行定义的变量的集合
    useSet : set
        存储了各行使用的变量的集合

    Returns
    -------
    bool
        该基本块是否被前向污染
    set
        实时更新的前向污染变量集合

    Notes
    -----
    _description_
    """
    isTainted = False
    for bbline in BB_LINE_DICT[bbname]:
        try:
            if DU_VAR_DICT[bbline]["def"] & preSet:
                isTainted = True
                defSet |= DU_VAR_DICT[bbline]["def"]
                useSet |= DU_VAR_DICT[bbline]["use"]
        except KeyError:
            continue  # 该行没有定义-使用关系, 跳过
    return isTainted, defSet, useSet


def isPostTainted(bbname: str, postSet: set, defSet: set, useSet: set):
    isTainted = False
    for bbline in reversed(BB_LINE_DICT[bbname]):
        try:
            if DU_VAR_DICT[bbline]["use"] & postSet:
                isTainted = True
                defSet |= DU_VAR_DICT[bbline]["def"]
                useSet |= DU_VAR_DICT[bbline]["use"]
        except KeyError:
            continue # 该行没有定义使用关系, 跳过
    return isTainted, defSet, useSet


def getbbPreTainted(loc: str, preSet: set):
    """根据行数获取基本块, 并更新变量污染信息

    Parameters
    ----------
    loc : str
        行数, filename:line
    preSet : set
        前向污染变量集合

    Returns
    -------
    str
        该行所属的基本块名称
    preSet
        实时更新的污染变量集合

    Notes
    -----
    _description_
    """
    preSet |= DU_VAR_DICT[loc]["use"]  # 取并集

    filename, line = loc.split(":")
    line = int(line)

    while not loc in LINE_BB_DICT.keys() or loc != LINE_BB_DICT[loc]:
        # 倒序查看并更新污染变量集合
        line -= 1
        loc = filename + ":" + str(line)

        try:
            if DU_VAR_DICT[loc]["def"] & preSet:
                preSet = preSet - DU_VAR_DICT[loc]["def"] | DU_VAR_DICT[loc]["use"]
        except KeyError:
            continue  # 该行没有定义-使用关系, 跳过

    return loc, preSet


def getbbPostTainted(loc: str, postSet: set):
    bbname = LINE_BB_DICT[loc]

    filename, line = loc.split(":")
    line = int(line)

    while not loc in LINE_BB_DICT.keys() or loc != LINE_BB_DICT[loc]:
        # 顺序查看并更新污染变量集合
        line += 1
        loc = filename + ":" + str(line)

        if line > MAX_LINE_DICT[filename]:
            break  # 如果顺序遍历到文件末尾了, 跳出循环

        try:
            if DU_VAR_DICT[loc]["use"] & postSet:
                postSet = postSet - DU_VAR_DICT[loc]["use"] | DU_VAR_DICT[loc]["def"]
        except KeyError:
            continue  # 该行没有定义使用关系

    return bbname, postSet


def getNodeName(nodes, nodeLabel) -> str:
    """遍历nodes, 获取nodeLabel的name, 形如Node0x56372e651a90

    Parameters
    ----------
    nodes : _type_
        _description_
    nodeLabel : _type_
        _description_

    Returns
    -------
    str
        _description_

    Notes
    -----
    _description_
    """
    for node in nodes:
        if node.get("label") == "\"{" + nodeLabel + ":}\"":
            return node.obj_dict["name"]
    return ""


def fitnessCalculation(path: str, tSrcsFile: str):
    """计算各基本块的适应度

    Parameters
    ----------
    path : str
        _description_
    tSrcsFile : str
        _description_

    Notes
    -----
    _description_
    """
    tSrcs = dict()
    fitDict = dict()  # <bb名, 适应度数组>
    resDict = dict()  # <bb名, 适应度>
    index = 0  # 下标

    with open(tSrcsFile) as f:  # 读取污点源,
        tSrcs = json.load(f)
        for k, v in tSrcs.items():
            if "def" in v.keys():
                v["def"] = set(v["def"])
            if "use" in v.keys():
                v["use"] = set(v["use"])

    global DU_VAR_DICT, BB_LINE_DICT, BB_FUNC_DICT, FUNC_ENTRY_DICT, LINE_CALLS_PRE_DICT, LINE_BB_DICT, MAX_LINE_DICT

    with open(path + "/duVar.json") as f:  # 读取定义使用关系的json文件
        DU_VAR_DICT = json.load(f)

        for k, v in DU_VAR_DICT.items():

            if "def" in v.keys():
                v["def"] = set(v["def"])
            else:
                v["def"] = set()

            if "use" in v.keys():
                v["use"] = set(v["use"])
            else:
                v["use"] = set()

    with open(path + "/bbLine.json") as f:  # 读取基本块和它所有报行的行的json文件
        BB_LINE_DICT = json.load(f)
    for k, v in BB_LINE_DICT.items():  # 对基本块所拥有的行进行排序, 从大到小, 方便后续操作
        v.sort(key=functools.cmp_to_key(myCmp))

    with open(path + "/bbFunc.json") as f:  # 该json是为了能更快地确认bb所在函数
        BB_FUNC_DICT = json.load(f)

    with open(path + "/funcEntry.json") as f:  # 该json是为了更方便地计算跨函数间的基本块距离
        FUNC_ENTRY_DICT = json.load(f)
        for k, v in FUNC_ENTRY_DICT.items():
            FUNC_ENTRY_DICT[k] = v.rstrip(":")

    with open(path + "/lineCallsPre.json") as f:  # 该json存储里每一行的调用信息
        LINE_CALLS_PRE_DICT = json.load(f)
        for k1, v1 in LINE_CALLS_PRE_DICT.items():
            for k2, v2 in v1.items():
                for k3, v3 in v2.items():
                    LINE_CALLS_PRE_DICT[k1][k2][k3] = set(v3)

    with open(path + "/linebb.json") as f:  # 该json存储了每一行对应的基本块
        LINE_BB_DICT = json.load(f)

    with open(path + "/maxLine.json") as f:
        MAX_LINE_DICT = json.load(f)

    # TODO: 下面的内容都不完整, 需要完善

    for tk, tv in tSrcs.items():
        cgDist = 0  # 函数调用之间的距离
        visited = set()  # 防止重复计算

        preSet, postSet = set(), set()
        if "use" in tv.keys():
            preSet = tv["use"]
        if "def" in tv.keys():
            postSet = tv["def"]

        preQueue = Queue()  # 该队列的元素是一个三元组, 第一个元素是待分析的可以到达污点源的行, 第二个元素是函数间的距离, 第三个元素是受污染的变量
        preQueue.put((tk, cgDist, preSet))

        postQueue = Queue()
        postQueue.put((tk, cgDist, postSet))

        # 前向污点分析
        while not preQueue.empty():
            targetLabel, cgDist, preSet = preQueue.get()
            if targetLabel in visited:
                continue

            targetLabel, preSet = getbbPreTainted(targetLabel, preSet)

            func = BB_FUNC_DICT[targetLabel]
            cfg = "cfg." + func + ".dot"
            pq = PriorityQueue()

            cfgdot = pydot.graph_from_dot_file(path + "/" + cfg)[0]
            cfgnx = nx.drawing.nx_pydot.from_pydot(cfgdot)
            nodes = cfgdot.get_nodes()

            targetName = getNodeName(nodes, targetLabel)

            entryLabel = FUNC_ENTRY_DICT[func]
            entryName = getNodeName(nodes, entryLabel)

            # 若无法获取target或entry的name, 跳过
            if len(targetName) == 0 or len(entryName) == 0:
                continue

            # 获取以污点源为终点, 能到达它的基本块, 达成前向分析的效果
            for node in nodes:
                nodeLabel = node.get("label").lstrip("\"{").rstrip(":}\"")
                nodeName = node.obj_dict["name"]
                try:
                    if nodeName == targetName:
                        distance = cgDist
                    else:
                        distance = cgDist + nx.shortest_path_length(cfgnx, nodeName, targetName)
                    pq.put(MyNode(distance, nodeName, nodeLabel))
                except nx.NetworkXNoPath:
                    pass

            nowDist = cgDist  # nowDist用于判断当前节点与上一节点是否是同一宽度
            defSet, useSet = set(), set()
            while not pq.empty():
                node = pq.get()
                distance, bbname = node.distance, node.label

                # 同一宽度时, 统计def-use情况, 并存入集合
                # 不同宽度时, 与defSet做差集, 与useSet做并集
                if distance != nowDist:
                    nowDist = distance
                    preSet = preSet - defSet | useSet
                    defSet.clear()
                    useSet.clear()

                # 如果距离与cg间的距离相等, 证明该基本块就是污点源基本块或调用里能到达污点源函数的基本块, 一定是被污染的
                if distance == cgDist:
                    isTainted = True
                else:
                    isTainted, defSet, useSet = isPreTainted(bbname, preSet, defSet, useSet)

                # 若被污染了, 计算适应度, 加入dict
                if isTainted:
                    if not bbname in fitDict.keys():  # 将value初始化为长度为len(tSrcs)的列表
                        fitDict[bbname] = [0] * len(tSrcs)
                    fitness = 1 / (1 + distance)
                    fitDict[bbname][index] = max(fitDict[bbname][index], fitness)

            cgDist += nx.shortest_path_length(cfgnx, entryName, targetName)

            # 如果没有函数调用里func, 证明前向分析到头了, 不需要再往队列里添加元素了
            if not func in LINE_CALLS_PRE_DICT.keys():
                continue

            # 将调用了当前函数的bb和cgDist加入队列
            for caller, pas in LINE_CALLS_PRE_DICT[func].items():
                # 根据调用函数的对应关系替换preSet
                nPreSet = preSet.copy()
                for param, arguments in pas.items():
                    try:
                        nPreSet.remove(param)
                        nPreSet |= arguments
                    except KeyError:  # 若param没有受到污染, 跳过
                        pass
                preQueue.put((caller, cgDist, nPreSet))

            # 将该污点源加入集合, 防止重复计算
            visited.add(targetLabel)

        # 后向污点分析
        while not postQueue.empty():
            targetLabel, cgDist, postSet = postQueue.get()
            targetLabel, postSet = getbbPostTainted(targetLabel, postSet)

            func = BB_FUNC_DICT[bbname]
            cfg = "cfg." + func + ".dot"
            pq = PriorityQueue()

            cfgdot = pydot.graph_from_dot_file(path + "/" + cfg)[0]
            cfgnx = nx.drawing.nx_pydot.from_pydot(cfgdot)
            nodes = cfgdot.get_nodes()

            targetName = getNodeName(nodes, targetLabel)

            # 获取以污点源为起点, 能被它到达的基本块, 达成后向污点分析的效果
            for node in nodes:
                nodeLabel = node.get("label").lstrip("\"{").rstrip(":}\"")
                nodeName = node.obj_dict["name"]
                try:
                    if nodeName == targetName:
                        distance = 0
                    else:
                        distance = nx.shortest_path_length(cfgnx, targetName, nodeName) + cgDist
                    pq.put(MyNode(distance, nodeName, nodeLabel))
                except nx.NetworkXNoPath:
                    pass  # 无法到达, 跳过

            nowDist = cgDist
            defSet, useSet = set(), set()
            while not pq.empty():
                node = pq.get()
                distance, bbname = node.distance, node.label

                # 同一宽度时, 统计def-use情况, 并存入集合
                # 不同宽度时, 与useSet做差集, 与defSet做并集
                if distance != nowDist:
                    nowDist = distance
                    postSet = postSet - useSet | defSet
                    defSet.clear()
                    useSet.clear()

                if distance == cgDist:
                    isTainted = True
                else:
                    isTainted, defSet, useSet = isPostTainted(bbname, postSet, defSet, useSet)

                if isTainted:
                    print(bbname + " is Tainted.")

        index += 1

    print(fitDict)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--path", help="存储dot, json, txt等文件的目录", required=True)
    parser.add_argument("-t", "--taint", help="存储污点源信息的txt文件", required=True)
    args = parser.parse_args()
    fitnessCalculation(args.path, args.taint)
