# 生成bc文件
###
 # @Author: Radon
 # @Date: 2022-01-22 15:24:14
 # @LastEditors: Radon
 # @LastEditTime: 2022-03-25 12:47:37
 # @Description: Hi, say something
### 
clang -O0 -emit-llvm -c test.c -o test.bc

# 生成ll文件
clang -S -emit-llvm test.c -o test.ll
clang -S -g -emit-llvm -fno-discard-value-names test.c -o test.ll

# 调用共享库
clang -Xclang -load -Xclang build/radon/libRnPass.so test.c

# 调用共享库时保留变量名字
clang -S -g -emit-llvm -fno-discard-value-names -Xclang -load -Xclang build/radon/libRnPass.so examples/1_simple/test.c -o examples/1_simple/test.ll
clang -S -g -emit-llvm -fno-discard-value-names -Xclang -load -Xclang build/radon1/libRnDuPass.so examples/1_simple/test.c -o examples/1_simple/test.ll
clang -S -g -emit-llvm -fno-discard-value-names -Xclang -load -Xclang build/radon1/libRnDuPass.so examples/3_ptrAndArr/ptr_arr.c -o examples/3_ptrAndArr/ptr_arr.ll

clang -S -g -emit-llvm -fno-discard-value-names -Xclang -load -Xclang build/radon1/libRnDuPass.so examples/4_sample/sample.c -o examples/4_sample/sample.ll; cat radon1/out-files/duVar.json | jq --tab . > radon1/out-files/duVar2.json; mv radon1/out-files/duVar2.json radon1/out-files/duVar.json
clang -S -g -emit-llvm -fno-discard-value-names -Xclang -load -Xclang build/radon1/libRnDuPass.so examples/5_address/address.c -o examples/5_address/address.ll