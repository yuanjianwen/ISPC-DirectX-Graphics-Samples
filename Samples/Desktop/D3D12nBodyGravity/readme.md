# n-Body Gravity Sample
This sample demonstrates the use of asynchronous compute shaders (multi-engine) to simulate an n-body gravity system. Graphics commands and compute commands can be recorded simultaneously and submitted to their respective command queues when the work is ready to begin execution on the GPU. This sample also demonstrates advanced usage of fences to synchronize tasks across command queues.

### Optional Features
This sample has been updated to build against the Windows 10 Anniversary Update SDK. In this SDK a new revision of Root Signatures is available for Direct3D 12 apps to use. Root Signature 1.1 allows for apps to declare when descriptors in a descriptor heap won't change or the data descriptors point to won't change.  This allows the option for drivers to make optimizations that might be possible knowing that something (like a descriptor or the memory it points to) is static for some period of time.

## Intel Modifications

### CPU Compute with ISPC

ISPC is the [Intel SPMD Program Compiler](https://ispc.github.io/) that compiles C style compute kernels into vectorised CPU instructions. The compiled functions can be called directly from within your application code and can be passed data in the normal ways. ISPC is open source and targets different instruction backends such as AVX2, AVX1 and SSE4  etc.  It is not an auto vectorising compiler, but instead it relies on some keywords and new looping constructs to control how algorithms should be parallelized, whilst utilising a high level language to deliver low-level performance gains.

The purpose of modifying this sample is to show how trivial it is to integrate ISPC into a project, how easy it is to port HLSL compute or C/C++ code to an ISPC kernel and also to see the performance gains that can be achieved over straight C/C++ scalar code.

### Changes

* Removed the original CPU threading code;
* Added multi-threaded scalar CPU compute path;
* Added ISPC kernel and Custom Build Tool settings;
* Added multi-threaded ISPC vectorised CPU compute path;
* Added performance data to the window title;
* [SPACE] toggles the compute method.

### Links

[ISPC Home]: https://ispc.github.io
[ISPC User Guide]: https://ispc.github.io/ispc.html
[ISPC Performance Guide]: https://ispc.github.io/perfguide.html
[ISPC Simple Example]: https://ispc.github.io/example.html
[ISPC FAQ]: https://ispc.github.io/faq.html
[ISPC Wiki]: https://github.com/ispc/ispc/wiki
[ISPC Code]: https://github.com/ispc/ispc



