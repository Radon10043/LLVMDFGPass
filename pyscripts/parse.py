'''
Author: Radon
Date: 2022-02-05 16:20:42
LastEditors: Radon
LastEditTime: 2022-03-31 15:26:19
Description: Hi, say something
'''
import pydot
import argparse
import os
import sys
import json
import heapq
import functools

import networkx as nx

from queue import Queue, PriorityQueue


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
    tSrcs = list()
    tQueue = Queue()
    duVarDict = dict()
    bbLineDict = dict()  # <bb名, 它所包含的所有行>
    fitDict = dict()  # <bb名, 适应度数组>
    resDict = dict()  # <bb名, 适应度>
    index = 0  # 下标

    with open(tSrcsFile) as f:  # 读取污点源, 并加入队列
        for line in f.readlines():
            tSrcs.append(line.rstrip("\n"))

    with open(path + "/duVar.json") as f:  # 读取定义使用关系的json文件
        duVarDict = json.load(f)

        for k, v in duVarDict.items():

            if "def" in v.keys():
                v["def"] = set(v["def"])
            else:
                v["def"] = set()

            if "use" in v.keys():
                v["use"] = set(v["use"])
            else:
                v["use"] = set()

    with open(path + "/bbLine.json") as f:  # 读取基本块和它所有报行的行的json文件
        bbLineDict = json.load(f)
    for k, v in bbLineDict.items():  # 对基本块所拥有的行进行排序, 从大到小, 方便后续操作
        v.sort(key=functools.cmp_to_key(myCmp))

    # TODO: 下面的内容都不完整, 需要完善

    for tSrc in tSrcs:
        cgDist = 0  # 函数调用之间的距离
        tQueue.put(tSrc)
        visited = set()  # 防止重复计算

        while not tQueue.empty():
            targetLabel = tQueue.get()
            if targetLabel in visited:
                continue

            func = "main"
            cfg = "cfg.main.dot"
            pq = PriorityQueue()

            cfgdot = pydot.graph_from_dot_file(path + "/" + cfg)[0]
            cfgnx = nx.drawing.nx_pydot.from_pydot(cfgdot)
            nodes = cfgdot.get_nodes()

            targetName = getNodeName(nodes, targetLabel)  # TODO: target不一定是基本块名称

            # TODO: 若target为空, 则跳过
            # TODO: 目前只有前向分析

            for node in nodes:
                nodeLabel = node.get("label").lstrip("\"{").rstrip(":}\"")
                nodeName = node.obj_dict["name"]
                try:
                    distance = nx.shortest_path_length(cfgnx, nodeName, targetName)  # + cgDist
                    if distance == 0:
                        continue
                    pq.put(MyNode(distance, nodeName, nodeLabel))
                except nx.NetworkXNoPath:
                    print(nodeLabel + " cant reach target")

            preSet = set()
            for var in duVarDict[targetLabel]["use"]:
                preSet.add(var)

            while not pq.empty():
                node = pq.get()
                distance, bbname = node.distance, node.label

                # 检查该bb是否被污染, 并实时更新preSet
                isTainted = False
                for bbline in bbLineDict[bbname]:
                    try:
                        if duVarDict[bbline]["use"] & preSet:
                            isTainted = True
                        if duVarDict[bbline]["def"] & preSet:
                            isTainted = True
                            preSet = preSet - duVarDict[bbline]["def"] | duVarDict[bbline]["use"]
                    except KeyError:
                        continue  # 该行没有定义-使用关系, 跳过

                if isTainted:
                    if not bbname in fitDict.keys():  # 将value初始化为长度为len(tSrcs)的列表
                        fitDict[bbname] = [0] * len(tSrcs)
                    fitness = 1 / (1 + distance)
                    fitDict[bbname][index] = max(fitDict[bbname][index], fitness)

            # cgDist += shortest(targetName, entryName)
            # tQueue已经pop
            # callers = bbs which called func (set)
            # tQueue.put(callers)

        index += 1

    print(fitDict)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--path", help="存储dot, json, txt等文件的目录", required=True)
    parser.add_argument("-t", "--taint", help="存储污点源信息的txt文件", required=True)
    args = parser.parse_args()
    fitnessCalculation(args.path, args.taint)