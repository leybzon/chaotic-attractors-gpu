# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

This is a GPU-accelerated chaotic attractor visualization system written in C with OpenACC. It renders cinematic visualizations of various chaotic attractors (Aizawa, Thomas, Lorenz, Halvorsen, Chen) using particle systems and outputs raw RGB frames to stdout.

## Build & Run Commands

### Building
Compile with NVIDIA HPC compiler (required for OpenACC):
```bash
nvc -acc -fast -Minfo=accel -o attractor_cinematic attractor_cinematic.c -lm
```

### Running (Recommended: Use the Script)
```bash
./generate_video.sh                      # Use defaults (20 fragments, 300 frames each)
./generate_video.sh -n 10 -f 60          # Quick test (10 fragments, 60 frames each)
./generate_video.sh -o output.mp4        # Custom output filename
./generate_video.sh --help               # Show all options
```

### Manual Method
Generate video frames and pipe to ffmpeg:
```bash
./attractor_cinematic -n 20 -f 300 | ffmpeg -f rawvideo -pixel_format rgb24 -video_size 1920x1080 -framerate 60 -i - -c:v libx264 -preset fast -crf 18 -pix_fmt yuv420p -y cinematic.mp4
```

**Command line arguments:**
- `-n <fragments>`: Number of attractor fragments (default: 20)
- `-f <frames>`: Frames per fragment (default: 300)

### Viewing Output
```bash
ffplay cinematic.mp4
# or
mpv cinematic.mp4
```

## Architecture

### GPU Acceleration Model
The code uses OpenACC directives for GPU parallelization on NVIDIA GPUs:
- **Data Management**: Persistent GPU memory for particle arrays (h_x, h_y, h_z, velocities, buffers) via `#pragma acc enter data`
- **Parallel Regions**: Three main GPU kernels per frame:
  1. Physics update (lines 145-174): Particle integration using various attractor equations
  2. Statistical reduction (lines 178-200): Center-of-mass and spread calculations for dynamic camera
  3. Rendering (lines 225-261): Orthographic projection with atomic updates to accumulation buffer

### Rendering Pipeline
1. **Physics**: RK1 (Euler) integration of particles following attractor differential equations
2. **Camera**: Hybrid zoom system combining per-attractor base multipliers, velocity-based dynamic adjustments, and sinusoidal breathing animation
3. **Projection**: Orthographic projection with rotation around Y-axis (theta = frame * 0.005) - cam_scale directly maps units to pixels
4. **Accumulation**: Atomic RGB updates with velocity-based heatmap coloring
5. **Tone Mapping**: Logarithmic exposure compression
6. **Output**: Raw RGB24 frames written to stdout

### Attractor System
- **Type Switching**: Cycles through 5 attractor types every 6 fragments (lines 125-127)
- **Parameter Morphing**: Smooth interpolation (lerp=0.02) between parameter sets (lines 132-135)
- **Randomization**: Each attractor variant gets random parameter perturbations (lines 52-74)
- **Particle Reset**: Out-of-bounds particles (>80.0) are reset to prevent instability (lines 166-170)

### Configuration Constants (lines 10-44)
- `WIDTH/HEIGHT`: 1920x1080 resolution
- `NUM_PARTICLES`: 2,000,000 particles
- `DT`: 0.012 timestep for integration
- `EXPOSURE`: 2.5 for tone mapping
- `MIN_ZOOM/MAX_ZOOM`: 20.0 to 2000.0 camera scale range (orthographic)
- `ATTRACTOR_BASE_MULTIPLIERS[]`: Per-attractor framing multipliers [0.8, 0.8, 2.5, 1.2, 2.5]
- `ZOOM_OSCILLATION_AMPLITUDE`: 0.12 (±12% sinusoidal breathing effect)
- `DYNAMIC_ADJUSTMENT_RANGE`: 0.15 (±15% velocity-based zoom adjustment)
- `SCREEN_FILL_FACTOR`: 0.87 (87% screen coverage, increased from 70%)

## Key Implementation Details

- **Velocity Coloring**: Particle color determined by speed relative to max speed, using custom heatmap (lines 42-50)
- **Depth Fade**: Subtle fading based on Z-coordinate for visual interest (not tied to projection)
- **Orthographic Projection**: Direct pixel mapping without perspective division for predictable screen filling
- **Atomic Operations**: Required for parallel accumulation buffer writes
- **Reduction Operations**: OpenACC reductions for statistical measures (sum_x, sum_y, max_spd)

### Hybrid Camera System

The camera system uses a **three-component multiplier** to frame attractors optimally:

1. **Base Multiplier (Per-Attractor)**
   - Different attractors have vastly different natural scales
   - AIZAWA/THOMAS (±2 units): 0.8x multiplier for tight framing
   - LORENZ/CHEN (±20-30 units): 2.5x multiplier to fill screen properly
   - HALVORSEN (±3-5 units): 1.2x moderate multiplier
   - Smoothly transitions with 0.02f lerp when attractors change

2. **Dynamic Factor (Velocity-Based)**
   - Zooms out up to 15% during high-velocity particle events
   - Based on ratio: `max_spd / smooth_max_spd`
   - Clamped to [0.85, 1.15] range to prevent extreme adjustments

3. **Sinusoidal Factor (Breathing Animation)**
   - Creates cinematic breathing effect with ±12% oscillation
   - Completes one full cycle per fragment (300 frames = 5 sec at 60fps)
   - Adds visual interest and reveals attractor details at different scales

**Final Calculation:**
```
combined_multiplier = base × dynamic × sinusoidal
target_dimensions = mean_dist × combined_multiplier
camera_scale = (DIMENSION × 0.87) / target_dimensions

// Orthographic projection (lines 270-273)
px = (rx - cam_cx) × cam_scale + WIDTH/2
py = (ry - cam_cy) × cam_scale + HEIGHT/2
```

This hybrid approach with orthographic projection ensures attractors occupy 80-90% of screen space while adapting to particle behavior and providing smooth, cinematic motion. The cam_scale value directly maps attractor units to screen pixels.

## Platform Notes

- Requires NVIDIA GPU with OpenACC support
- Built for ARM64 (aarch64) architecture based on compiled binary
- Uses NVIDIA HPC SDK compiler (nvc) - GCC/Clang cannot compile OpenACC
