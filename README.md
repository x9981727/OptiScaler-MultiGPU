# OptiScaler MultiGPU Experimental

Experimental Windows build workspace for an OptiScaler 0.9.4 Multi-GPU Frame Generation offload prototype.

This repository is intentionally a small build kit: GitHub Actions clones the upstream OptiScaler v0.9.4 source with submodules, applies the local patch, builds Release with Visual Studio 2022/MSVC, and packages the unsigned output.

> Experimental/WIP: use only for local single-player testing. Do not treat builds as production-ready until the Multi-GPU path has been compiled and hardware-tested.
