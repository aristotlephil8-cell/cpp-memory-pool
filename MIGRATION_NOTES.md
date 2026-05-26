# Migration Notes

## Source

- Original reference repository: https://github.com/youngyangyang04/memory-pool.git
- Local source directory:
  - Windows: `F:\Work\master\CodingLearning\JobApplicationProject\RefProject\memory-pool`
  - WSL: `/mnt/f/Work/master/CodingLearning/JobApplicationProject/RefProject/memory-pool`
- Target directory:
  - Windows: `F:\Work\master\CodingLearning\JobApplicationProject\GitHubProjects\cpp-memory-pool`
  - WSL: `/mnt/f/Work/master/CodingLearning/JobApplicationProject/GitHubProjects/cpp-memory-pool`

## Migration Method

The reference project content was copied into this repository with `rsync`, excluding Git metadata, build output, CMake caches, editor settings, object files, executables, and generated test result files. The target repository's existing `.git` directory and remote configuration were preserved.

## Project Direction

This repository will be maintained as my C++ memory pool learning and optimization project. Future changes will focus on understanding allocator internals, improving implementation quality, documenting debugging work, and presenting the project as part of my AI Infra job application portfolio.

## Follow-up Plan

1. Fix the v2 `ThreadCache` `freeListSize_` counting issue.
2. Add v2/v3 debugging notes.
3. Add benchmark results.
4. Rewrite the README so it fits my AI Infra job application project presentation.
