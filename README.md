# ISPC-DirectX-Graphics-Samples
This repo is forked from Microsofts [DirectX-Graphics-Samples](https://github.com/Microsoft/DirectX-Graphics-Samples) repo and follows their licensing model. 




## Intel Modifications

### CPU Compute with ISPC

ISPC is the [Intel SPMD Program Compiler](https://ispc.github.io/) that compiles C style compute kernels into vectorised CPU instructions. The compiled functions can be called directly from within your application code and can be passed data in the normal ways. ISPC is open source and targets different vector instruction backends such as Intels AVX2, AVX1 and SSE4  etc.  It is not an auto vectorising compiler, but instead it relies on some keywords and new looping constructs to control how algorithms should be parallelized, whilst utilising a high level language to deliver low-level performance gains.

Modifications have been made to the following project :

​	Samples\Desktop\D3D12nBodyGravity

The purpose of modifying this sample is to show how simple it is to integrate ISPC into a project, how easy it is to port HLSL compute or C/C++ code to an ISPC kernel and also to see the performance gains that can be achieved over straight C/C++ scalar code.
