# 生成bc文件
clang -O0 -emit-llvm -c test.c -o test.bc

# 生成ll文件
clang -S -emit-llvm test.c -o test.ll
clang -S -g -emit-llvm -fno-discard-value-names test.c -o test.ll

# 调用共享库
clang -Xclang -load -Xclang build/radon/libRnPass.so test.c

# 调用共享库时保留变量名字
clang -S -g -emit-llvm -fno-discard-value-names -Xclang -load -Xclang build/radon/libRnPass.so examples/1_simple/test.c -o examples/1_simple/test.ll
clang -S -g -emit-llvm -fno-discard-value-names -Xclang -load -Xclang build/radon1/libRnDuPass.so examples/1_simple/test.c -o examples/1_simple/test.ll