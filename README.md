# XS-GEM5 Learning Repository

This is a learning fork of XS-GEM5 (Xiangshan specialized version of gem5 simulator).

## About This Repository

XS-GEM5 is a specialized version of the gem5 simulator for Xiangshan (Kunminghu) RISC-V processor. It's calibrated with Xiangshan V3 microarchitecture and supports full-system simulation with RVGCpt checkpoints.

## Recent Changes

- Applied PR #332: Calibrated latency for SimpleMemory
- Added SimpleMemBus class with response latency calibration
- Enhanced memory model for better Xiangshan simulation accuracy

## Branch: gem5-learning

The `gem5-learning` branch contains experimental changes and learning materials for understanding the GEM5 simulator internals.