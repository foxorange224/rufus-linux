# Rufus-Linux: The Reliable USB Formatting Utility

[![CMake Build Status](https://img.shields.io/github/actions/workflow/status/FoxOrange224/rufus-linux/build.yml?branch=main&style=flat-square&label=Build)](https://github.com/FoxOrange224/rufus-linux/actions)
[![Latest Release](https://img.shields.io/github/v/release/FoxOrange224/rufus-linux?include_prereleases&style=flat-square&label=Alpha%20Release)](https://github.com/FoxOrange224/rufus-linux/releases)
[![Licence](https://img.shields.io/badge/license-GPLv3-blue.svg?style=flat-square&label=License)](https://www.gnu.org/licenses/gpl-3.0.en.html)
[![Contributors](https://img.shields.io/github/contributors/FoxOrange224/rufus-linux.svg?style=flat-square&label=Contributors)](https://github.com/FoxOrange224/rufus-linux/graphs/contributors)

![Rufus logo](res/icons/rufus-128.png)

**Rufus-Linux** is a native Linux port of the reliable Windows USB formatting utility, built with **C++17 and Qt6**. It helps format and create bootable USB flash drives on GNU/Linux distributions with an interface faithful to the original tool.

---

## Features

* Format USB flash drives and virtual drives to FAT16, FAT32, NTFS, exFAT, ext2/3/4, btrfs, XFS, and UDF.
* Create bootable drives from bootable ISOs (Linux distributions, etc.) with full ISOHybrid and MBR/GPT partition scheme detection.
* Native Qt6 integration with a clean, lightweight, and familiar UI design.
* Embedded, detachable log panel tracking operations in real-time.
* Small footprint and direct system process handling using native Qt tools (`QProcess`).
* 100% [Free Software](https://www.gnu.org/philosophy/free-sw) ([GPL v3](https://www.gnu.org/licenses/gpl-3.0)).

---

## Compilation

To build Rufus-Linux from source, ensure you have **CMake**, a C++17 compliant compiler (like `g++`), and **Qt6 development libraries** installed on your system.

Run the following commands in your terminal:

```bash
sh start-build.sh
```
and use it in ./rufus, or use the GitHub release lol
