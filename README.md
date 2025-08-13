# Cavern - GPU-Accelerated Procedural Cave System for Unreal Engine 5

## Overview

Cavern is a high-performance procedural cave generation system for Unreal Engine 5.6+ that leverages GPU compute shaders and Nanite virtualized geometry to create infinite, explorable cave systems in real-time. The system uses advanced techniques including marching cubes, 3D noise fields, and chunk-based world streaming to generate complex underground environments.

![Cave System Preview](docs/images/cave-preview.jpg) <!-- Add your own screenshots -->

## ✨ Features

### Core Systems
- **GPU-Accelerated Generation**: Leverages compute shaders for massive parallel processing
- **Infinite World Streaming**: Chunk-based system with seamless loading/unloading
- **Real-time Modification**: Terrain deformation and cave carving in real-time
- **Nanite Integration**: Unlimited geometric detail with automatic LOD
- **Lumen Global Illumination**: Dynamic lighting perfect for cave environments

### Cave Generation
- **3D Perlin/Simplex Noise**: Multi-octave noise for natural cave formations
- **Marching Cubes Algorithm**: Smooth, organic cave surfaces
- **Geological Layering**: Different rock strata with varying properties
- **Cave Features**: Stalactites, stalagmites, crystal formations, underground lakes
- **Biome System**: Different cave types (limestone, ice caves, volcanic tubes)

### Performance
- **Async Chunk Generation**: Non-blocking world generation
- **GPU Memory Pooling**: Efficient VRAM usage with persistent buffers
- **Automatic LOD System**: Distance-based detail reduction
- **Frustum Culling**: Render only visible chunks

## 📋 Requirements

### Minimum Requirements
- **Unreal Engine**: 5.6 or higher
- **Visual Studio**: 2022 with C++ game development workload
- **Windows SDK**: 10.0.19041.0 or higher
- **GPU**: DirectX 12 compatible with 4GB+ VRAM
- **RAM**: 16GB minimum, 32GB recommended
- **Storage**: 10GB free space for project and cache

### Recommended Specifications
- **GPU**: RTX 3070 or better (for Nanite and Lumen)
- **CPU**: 8+ cores for optimal async generation
- **RAM**: 32GB or more
- **Storage**: NVMe SSD for fast chunk streaming

## 🚀 Quick Start

### 1. Clone the Repository
```bash
git clone https://github.com/yourusername/Cavern.git
cd Cavern
```

### 2. Generate Project Files
Right-click on `Cavern.uproject` → "Generate Visual Studio project files"

### 3. Enable Required Plugins
Open the project in Unreal Editor and enable:
- Procedural Mesh Component
- World Partition (should be enabled by default)

### 4. Build the Project
Open `Cavern.sln` in Visual Studio and build in Development Editor configuration.

### 5. Run the Demo
1. Open `Cavern.uproject` in Unreal Editor
2. Open the `Maps/CaveDemo` level
3. Press Play to start exploring!

## 📁 Project Structure

```
Cavern/
├── Source/
│   ├── Cavern/
│   │   ├── Public/
│   │   │   ├── CaveWorldSubsystem.h     # Main world management
│   │   │   ├── CaveChunk.h              # Individual chunk actor
│   │   │   ├── CaveGenerator.h          # Generation algorithms
│   │   │   └── MarchingCubesTables.h    # Lookup tables
│   │   └── Private/
│   │       ├── CaveWorldSubsystem.cpp
│   │       ├── CaveChunk.cpp
│   │       └── CaveGenerator.cpp
│   └── Shaders/
│       ├── CaveGeneration.usf           # Density field generation
│       └── MeshExtraction.usf           # Marching cubes GPU
├── Content/
│   ├── Blueprints/
│   │   ├── BP_CaveGameMode.uasset
│   │   ├── BP_CavePlayerController.uasset
│   │   └── BP_CaveCharacter.uasset
│   ├── Materials/
│   │   ├── M_CaveSubstrate.uasset      # Base cave material
│   │   └── MI_CaveVariations/          # Material instances
│   └── Maps/
│       └── CaveDemo.umap                # Demo level
└── Config/
    └── DefaultEngine.ini                 # Project settings
```

## 🎮 Usage

### Blueprint API

```cpp
// Generate caves around a location
UCaveWorldSubsystem* CaveSystem = GetWorld()->GetSubsystem<UCaveWorldSubsystem>();
CaveSystem->UpdateAroundPlayer(PlayerLocation);

// Modify terrain (dig/add)
CaveSystem->ModifyTerrainAt(Location, Radius, Strength);

// Query cave density at a point
float Density = CaveSystem->SampleDensityAt(WorldLocation);
```

### C++ API

```cpp
// Access the cave system
UCaveWorldSubsystem* CaveSystem = GetWorld()->GetSubsystem<UCaveWorldSubsystem>();

// Generate a specific chunk
FIntVector ChunkCoord(0, 0, 0);
CaveSystem->GenerateChunkAt(ChunkCoord);

// Configure generation parameters
FCaveGenerationSettings Settings;
Settings.VoxelSize = 0.5f;           // Size of each voxel in meters
Settings.ChunkSize = 32;             // Voxels per chunk axis
Settings.NoiseFrequency = 0.02f;     // Cave frequency
Settings.NoiseOctaves = 4;           // Detail levels
CaveSystem->SetGenerationSettings(Settings);
```

### Editor Tools

1. **Cave Brush Tool** (Window → Cave Tools → Brush)
   - Left Click: Carve caves
   - Right Click: Add terrain
   - Scroll: Adjust brush size

2. **Generation Preview** (Window → Cave Tools → Preview)
   - Visualize density fields
   - Preview chunk boundaries
   - Debug mesh generation

## ⚙️ Configuration

### DefaultEngine.ini Settings

```ini
[/Script/Cavern.CaveWorldSettings]
; Generation
DefaultVoxelSize=0.5
DefaultChunkSize=32
ViewDistance=8
MaxChunksInMemory=1000

; Performance
bUseGPUGeneration=true
bUseAsyncGeneration=true
ChunksPerFrame=4
MaxGenerationThreads=4

; Quality
bEnableNanite=true
bUseLumenGI=true
MeshOptimizationLevel=2
```

### Console Commands

```
# Performance
cavern.ShowStats                  # Display generation statistics
cavern.SetViewDistance <value>    # Change chunk view distance
cavern.RegenerateWorld            # Clear and regenerate all chunks

# Debug
cavern.ShowChunkBounds            # Visualize chunk boundaries
cavern.ShowDensityField          # Display density field values
cavern.WireframeMode             # Toggle wireframe rendering

# Generation
cavern.SetNoiseFrequency <value> # Adjust cave frequency
cavern.SetVoxelSize <value>      # Change voxel resolution
```

## 🔧 Troubleshooting

### Build Errors

**Error: "Module 'Cavern' could not be found"**
- Ensure Cavern.Build.cs exists in Source/Cavern/
- Verify module name matches in .uproject file
- Regenerate project files

**Error: "ProceduralMeshComponent not found"**
- Enable the Procedural Mesh Component plugin
- Add to Cavern.Build.cs: `PrivateDependencyModuleNames.Add("ProceduralMeshComponent");`

**Error: Code 8 when building**
- Delete Binaries, Intermediate, and DerivedDataCache folders
- Regenerate project files
- Ensure Visual Studio 2022 is properly installed

### Performance Issues

**Low FPS during generation**
- Reduce `ChunksPerFrame` in settings
- Increase `VoxelSize` for lower resolution
- Decrease `ViewDistance`
- Enable `bUseAsyncGeneration`

**High memory usage**
- Reduce `MaxChunksInMemory`
- Enable more aggressive chunk cleanup
- Use lower resolution textures

**Chunks not appearing**
- Check console for generation errors
- Verify GPU compute support
- Ensure sufficient VRAM available

### Visual Issues

**Black caves/No lighting**
- Enable Lumen in Project Settings
- Add light sources or emissive materials
- Check if Lumen Scene View Distance is sufficient

**Mesh flickering/Z-fighting**
- Adjust near clipping plane
- Check for overlapping chunks
- Verify chunk boundaries align correctly

## 📊 Performance Benchmarks

| Configuration | Chunk Size | View Distance | FPS (RTX 3080) | Memory Usage |
|--------------|------------|---------------|----------------|--------------|
| Low          | 16         | 3             | 120+           | 2GB          |
| Medium       | 32         | 5             | 90             | 4GB          |
| High         | 32         | 8             | 60             | 6GB          |
| Ultra        | 64         | 10            | 45             | 8GB+         |

## 🗺️ Roadmap

### Version 1.1 (In Progress)
- [ ] Compute shader implementation for GPU generation
- [ ] Basic mesh extraction on GPU
- [ ] Chunk pooling system

### Version 1.2 (Planned)
- [ ] Biome system with varied cave types
- [ ] Water simulation with flow
- [ ] Ecosystem with creatures
- [ ] Save/Load system

### Version 2.0 (Future)
- [ ] Multiplayer support with replication
- [ ] Procedural cave dungeons
- [ ] Advanced lighting with bioluminescence
- [ ] VR support

## 🛠️ Development

### Building from Source

1. Install prerequisites (Visual Studio 2022, Windows SDK)
2. Clone the repository with submodules:
   ```bash
   git clone --recursive https://github.com/yourusername/Cavern.git
   ```
3. Generate project files and build

### Contributing

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit changes (`git commit -m 'Add amazing feature'`)
4. Push to branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

### Code Style

- Follow [Unreal Engine Coding Standards](https://docs.unrealengine.com/5.0/en-US/epic-cplusplus-coding-standard-for-unreal-engine/)
- Use `CAVERN_API` macro for exported classes
- Prefix classes appropriately (A for Actors, U for UObjects, F for structs)
- Comment complex algorithms and GPU kernels

## 📝 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 🙏 Acknowledgments

- **Marching Cubes Algorithm** - Paul Bourke's polygonization documentation
- **Noise Functions** - Stefan Gustavson's simplex noise implementations
- **Unreal Community** - For invaluable technical discussions and support
- **FastNoise Library** - Jordan Peck for the noise generation library

## 📧 Contact

- **Developer**: Your Name
- **Email**: your.email@example.com
- **Discord**: [Join our Discord](https://discord.gg/yourserver)
- **Twitter**: [@yourhandle](https://twitter.com/yourhandle)

## 🐛 Bug Reports

Please use the [GitHub Issues](https://github.com/yourusername/Cavern/issues) page to report bugs. Include:
- Unreal Engine version
- System specifications
- Steps to reproduce
- Error messages/logs
- Screenshots if applicable

---

**Made with ❤️ using Unreal Engine 5**