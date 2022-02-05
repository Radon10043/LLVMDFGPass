import pydot
import argparse
import os
import sys
import queue

import networkx as nx


def getInfo(path: str):
    """获取所有dot文件路径, 调用函数信息和污点源

    Parameters
    ----------
    path : str
        dot文件夹的路径

    Notes
    -----
    [description]
    """
    if not os.path.exists(path):
        print("文件夹" + path + "不存在.", file=sys.stderr)
        return
    if not os.path.isdir(path):
        print(path + "不是文件夹, 分析停止.", file=sys.stderr)
        return

    dotfileList = list()  # 所有dot图的路径
    for file in os.listdir(path):
        if os.path.splitext(file)[1] != ".dot": # 跳过非dot文件
            continue
        dotfileList.append(os.path.join(path, file))

    callDict = dict()  # <文件名与行号, <被调用的函数名, <参数, 变量>>>
    lines = open(os.path.join(path, "linecalls.txt")).readlines()
    for line in lines:
        line = line.rstrip("\n")
        loc, calledF, pvs = line.split("-") # 文件名与行号, 被调用的函数, 参数与变量列表

        callDict[loc] = dict()
        callDict[loc][calledF] = dict()

        pvs = pvs.split(",")    # 将parameter与variable写入字典
        for pv in pvs:
            try:
                p,v = pv.split(":")
                callDict[loc][calledF][p] = v
            except:
                continue

    # TODO: 污点源队列

    return dotfileList, callDict


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--path", help="存储dot图的文件夹", required=True)
    args = parser.parse_args()
    dotfileList, callDict = getInfo(args.path)