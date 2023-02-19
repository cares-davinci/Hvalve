# Hvalve

## What is Hvalve?

HPBvalve (Hvalve) is an integrated host-SSD mapping table management scheme for smartphones.

Hvalve prioritizes the foreground app in managing mapping table entries in the HPB memory. Hvalve dynamically resizes the overall capacity
of the HPB memory depending on the memory pressure status
of the smartphone. Our experimental results using the prototype implementation demonstrate that Hvalve improves
UX-critical app launching time by up to 43% (250 ms) over
the existing HPB management scheme, without negatively
affecting memory pressure. Meanwhile, the L2P mapping
misses are alleviated by up to 78%.

Our [USENIX FAST 2023 paper](https://www.usenix.org/conference/fast23/presentation/kim-yoona) describes Hvalve in detail.

To cite us, please use the following BibTex entry:
``` latex
@inproceedings {285748,
author = {Yoona Kim and Inhyuk Choi and Juhyung Park and Jaeheon Lee and Sungjin Lee and Jihong Kim},
title = {Integrated {Host-SSD} Mapping Table Management for Improving User Experience of Smartphones},
booktitle = {21st USENIX Conference on File and Storage Technologies (FAST 23)},
year = {2023},
isbn = {978-1-939133-32-8},
address = {Santa Clara, CA},
pages = {441--456},
url = {https://www.usenix.org/conference/fast23/presentation/kim-yoona},
publisher = {USENIX Association},
month = feb,
}
```

## Sources

A prototype version of Hvalve is implemented on a [hardware development kit (HDK) based on the Snapdragon 888 SoC](https://developer.qualcomm.com/hardware/snapdragon-888-hdk).

To more accurately represent practical usage running on real production hardware, BSP for Snapdragon 888 SoC is used:
* AOSP (System image): [`LA.QSSI.12.0.r1-08300-qssi.0`](https://git.codelinaro.org/clo/la/la/system/manifest/-/commit/c25f38f3703fc1876dbe33ffc86273e9bf382936) (based on Android 12).
* Kernel: [`LA.UM.9.14.r1-18600.02-LAHAINA.QSSI12.0`](https://git.codelinaro.org/clo/la/kernel/msm-5.4/-/tree/846e80aba10f06141d1a125745a59f74610b812c) (based on Linux v5.4.161).


## Platform

The Snapdragon 888 HDK does not come with UFS that supports HPB. To evaluate HPB, we've used the PCIe slot available on the HDK to emulate HPB layer backed by a low-latency storage (ZSSD).

The HPB source code was taken from the latest ACK (Android Common Kernel) version available at the time of development: Linux v5.15 (android13-5.15).


## Git repository

To better track what has changed in order to incorporate Hvalve, this Git repository contains branches for each AOSP component that has been changed, with full commit histories:

* [kernel](https://github.com/cares-davinci/Hvalve/tree/kernel)

  * Based on [`LA.UM.9.14.r1-18600.02-LAHAINA.QSSI12.0`](https://git.codelinaro.org/clo/la/kernel/msm-5.4/-/tree/846e80aba10f06141d1a125745a59f74610b812c).
  * The kernel repository to build a monolithic boot.img. Device-tree will not compile as it is a proprietary source code from Qualcomm Inc.

* [frameworks/base](https://github.com/cares-davinci/Hvalve/tree/frameworks/base)

  * Based on [`LA.QSSI.12.0.r1-08300-qssi.0`](https://git.codelinaro.org/clo/la/platform/frameworks/base/-/tree/LA.QSSI.12.0.r1-08300-qssi.0).
  * The AOSP tree's `frameworks/base` repository.
  * Monitors and delivers app-level information to the kernel.

* [system/memory/lmkd](https://github.com/cares-davinci/Hvalve/tree/system/memory/lmkd)

  * Based on [`LA.QSSI.12.0.r1-08300-qssi.0`](https://git.codelinaro.org/clo/la/platform/system/memory/lmkd/-/tree/LA.QSSI.12.0.r1-08300-qssi.0)
  * The AOSP tree's `system/memory/lmkd` repository. (Produces `/system/bin/lmkd` executable binary.)
  * Monitors and delivers the memory pressure status of the system to the kernel.

If you want to fetch just the commit patches, run the following and locate `.patch` files from [`patches/`](https://github.com/cares-davinci/Hvalve/tree/main/patches) directory:

``` bash
$ git clone https://github.com/cares-davinci/Hvalve --single-branch --branch main
```


## Maintainer
Yoona Kim (yoonakim@davinci.snu.ac.kr)
