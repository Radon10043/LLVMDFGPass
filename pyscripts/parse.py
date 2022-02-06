import pydot
import argparse
import os
import sys
import queue

import networkx as nx


def getInfo(dotPath: str, taintPath: str):
    """获取所有dot文件路径, 调用函数信息和污点源

    Parameters
    ----------
    dotPath : str
        dot文件夹的路径
    taintPath : str
        污点源txt文件的路径

    Returns
    -------
    [type]
        [description]

    Notes
    -----
    [description]
    """
    if not os.path.exists(dotPath):
        print("文件夹" + dotPath + "不存在.", file=sys.stderr)
        return
    if not os.path.isdir(dotPath):
        print(dotPath + "不是文件夹, 分析停止.", file=sys.stderr)
        return

    if not os.path.exists(taintPath):
        print("文件" + taintPath + "不存在.", file=sys.stderr)
        return
    if not os.path.splitext(taintPath)[1] == ".txt":
        print(taintPath + "不是txt文件.", file=sys.stderr)
        return

    # 获取所有dot图的路径
    dotfileList = list()  # 所有dot图的路径
    for file in os.listdir(dotPath):
        if os.path.splitext(file)[1] != ".dot":  # 跳过非dot文件
            continue
        dotfileList.append(os.path.join(dotPath, file))

    # 获取调用信息
    callDict = dict()  # <文件名与行号, <被调用的函数名, <参数, 变量>>>
    lines = open(os.path.join(dotPath, "linecalls.txt")).readlines()
    for line in lines:
        line = line.rstrip("\n")
        loc, calledF, pvs = line.split("-")  # 文件名与行号, 被调用的函数, 参数与变量列表

        if not loc in callDict.keys():
            callDict[loc] = dict()
        if not calledF in callDict[loc].keys():
            callDict[loc][calledF] = dict()

        pvs = pvs.split(",")  # 将parameter与variable写入字典
        for pv in pvs:
            try:
                p, v = pv.split(":")

                if not p in callDict[loc][calledF].keys():
                    callDict[loc][calledF][p] = v.split("|")
                else:
                    callDict[loc][calledF][p].append(v)

            except:
                continue

    # 获取污点源队列
    taintList = open(taintPath).readlines()
    taintQueue = queue.Queue()
    for taint in taintList:
        taintQueue.put(taint.rstrip("\n"))

    return dotfileList, callDict, taintQueue


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("-d", "--dots", help="存储dot图的文件夹", required=True)
    parser.add_argument("-t", "--taint", help="存储污点源信息的txt文件", required=True)
    args = parser.parse_args()
    dotfileList, callDict, taintQueue = getInfo(args.dots, args.taint)