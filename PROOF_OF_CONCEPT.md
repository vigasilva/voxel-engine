# Voxel Engine — proof of concept

**AI disclosure: This project was made with AI using OpenAI Codex. AI generated substantial portions of the code, shaders, tests, and documentation under the project creator's direction.**

This project is a **proof-of-concept voxel engine aiming to simulate reality**, inspired by the idea of a Matrix-like simulated world. The goal is to make a world where matter has composition and physical properties, everything can be created or changed by hand, and useful objects emerge from performing the processes that produce them.

Earth is the current demonstration, not the boundary of the project. The long-term aim is a general simulator capable of supporting environments from tiny material structures to planets and space.

## What exists today

- A C17 engine with Vulkan rendering and an editable, physically scaled Earth demonstration.
- Millimeter-addressable edits, sparse persistent voxel storage, and material definitions with elemental fractions and physical properties.
- Mesh-backed terrain, greedy meshing, dynamic detail, background meshing, and GPU occlusion culling.
- Engine-generated distant terrain textures, measured elevation data, and procedural surface detail.
- Gravity derived from planetary mass and distance, plus an approximate nine-body Solar System with orbiting planets.
- Special-relativistic helpers and leading weak-field general-relativistic corrections.
- Editable water material, a water surface shading pipeline, and computer-resource and material visualizers.

The engine represents unmodified terrain implicitly and stores changes locally. It does not allocate every millimeter of a planet as an individual voxel. Detailed meshes serve nearby views; distant views use simpler representations while stored edits remain authoritative.

## Where this is heading

The intended direction is physical interaction between materials: heat transfer, phase changes, chemistry, fluids, manufacturing, construction, and destruction. A player should eventually be able to gather matter, combine it, and perform the steps needed to create things instead of relying on abstract crafting recipes.

These are ambitions, not completed features. The current project does not simulate all of reality, consciousness, full Einstein field equations, or complete fluid dynamics. Other planets currently have procedural renderings and orbital physics; Earth is the editable planetary world. Millimeter editing precision is not millimeter-accurate surveyed geography.

## Status

This is an experimental foundation. Architecture, save formats, performance, and physical models may change as the engine develops. The included tests exercise storage, editing, terrain generation, rendering, and approximate physics; they do not establish universal physical accuracy.

Build instructions, controls, verification commands, and specific limitations are in [README.md](README.md). Terrain sources and their separate attribution and licensing terms are documented in [data/README.md](data/README.md).
