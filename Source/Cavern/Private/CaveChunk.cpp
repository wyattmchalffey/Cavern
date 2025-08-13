#include "CaveChunk.h"
#include "ProceduralMeshComponent.h"
#include "Materials/Material.h"
#include "StaticMeshResources.h"
#include "StaticMeshAttributes.h"
#include "Engine/StaticMesh.h"
#include "Rendering/NaniteResources.h"
#include "StaticMeshOperations.h"
#include "MarchingCubesTables.h"
#include "Engine/World.h"
#include "Engine/CollisionProfile.h"
#include "UObject/ConstructorHelpers.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"


float ACaveChunk::SimplexNoise3D(FVector Position) const
{
    // Better 3D noise implementation
    float X = Position.X * 0.01f;
    float Y = Position.Y * 0.01f;
    float Z = Position.Z * 0.01f;
    
    // Use Unreal's built-in Perlin noise for now
    float NoiseValue = FMath::PerlinNoise3D(FVector(X, Y, Z));
    
    // Add some octaves for more detail
    NoiseValue += FMath::PerlinNoise3D(FVector(X * 2, Y * 2, Z * 2)) * 0.5f;
    NoiseValue += FMath::PerlinNoise3D(FVector(X * 4, Y * 4, Z * 4)) * 0.25f;
    
    return NoiseValue;
}

ACaveChunk::ACaveChunk()
{
    PrimaryActorTick.bCanEverTick = false;
    
    ProceduralMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("ProceduralMesh"));
    RootComponent = ProceduralMesh;
    ProceduralMesh->bUseAsyncCooking = true;
    
    StaticMeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("StaticMesh"));
    StaticMeshComponent->SetupAttachment(RootComponent);
    StaticMeshComponent->SetMobility(EComponentMobility::Movable);
    StaticMeshComponent->SetVisibility(false);
    
    // Enable proper lighting
    ProceduralMesh->SetCastShadow(true);
    ProceduralMesh->SetReceivesDecals(true);
    ProceduralMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    ProceduralMesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
    ProceduralMesh->SetVisibility(true);
    ProceduralMesh->SetHiddenInGame(false);
    
    // Default cave material for all chunks (can be overridden per-instance)
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> DefaultCaveMat(TEXT("/Game/M_Cave.M_Cave"));
    if (DefaultCaveMat.Succeeded())
    {
        CaveMaterialOverride = DefaultCaveMat.Object;
    }
    
    // Initialize safety variables
    bIsGenerating = false;
}

void ACaveChunk::BeginPlay()
{
    Super::BeginPlay();
}

void ACaveChunk::GenerateMesh(FIntVector ChunkCoordinate, float InVoxelSize, int32 InChunkSize)
{
    // Safety check
    if (!ProceduralMesh)
    {
        UE_LOG(LogTemp, Error, TEXT("ProceduralMesh is null in GenerateMesh!"));
        return;
    }
    
    if (bIsGenerating)
    {
        return;
    }
    
    bIsGenerating = true;
    ChunkCoord = ChunkCoordinate;
    VoxelSize = InVoxelSize;
    ChunkSize = InChunkSize;
    
    // Validate chunk size
    if (ChunkSize <= 0 || ChunkSize > 128)
    {
        UE_LOG(LogTemp, Error, TEXT("Invalid chunk size: %d"), ChunkSize);
        bIsGenerating = false;
        return;
    }
    
    // Clear previous mesh data
    ClearMesh();
    
    // Set chunk position in world
    FVector WorldPosition = FVector(ChunkCoord) * ChunkSize * VoxelSize;
    SetActorLocation(WorldPosition);
    
    // Generate density field first!
    GenerateDensityField();  // ADD THIS
    
    // Generate the mesh
    GenerateMarchingCubes();
    
    // Calculate normals and UVs if we have vertices
    if (Vertices.Num() > 0)
    {
        CalculateNormals();  // ADD THIS
        GenerateUVs();       // ADD THIS
        
        if (!bUseNaniteStaticMesh)
        {
            // Now create the mesh section on PMC
            ProceduralMesh->CreateMeshSection(
                0,
                Vertices,
                Triangles,
                Normals,
                UVs,
                VertexColors,
                Tangents,
                true
            );
        }
        
        // Apply user override material if provided; otherwise fallback to engine default
        if (CaveMaterialOverride)
        {
            ProceduralMesh->SetMaterial(0, CaveMaterialOverride);
        }
        else
        {
            UMaterial* FallbackMaterial = Cast<UMaterial>(
                StaticLoadObject(UMaterial::StaticClass(), nullptr, 
                TEXT("/Engine/BasicShapes/BasicShapeMaterial"))
            );
            if (FallbackMaterial)
            {
                ProceduralMesh->SetMaterial(0, FallbackMaterial);
            }
        }
        
        if (!bUseNaniteStaticMesh)
        {
            // Enable proper lighting on the procedural mesh
            ProceduralMesh->SetCastShadow(true);
            ProceduralMesh->SetReceivesDecals(true);
            ProceduralMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
            ProceduralMesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
        }
        
            UE_LOG(LogTemp, Warning, TEXT("Generated chunk with %d vertices"), Vertices.Num());
        }

        if (bUseNaniteStaticMesh)
        {
            BuildNaniteStaticMesh();
    }
    
    bIsGenerating = false;
}
void ACaveChunk::BuildNaniteStaticMesh()
{
    // Build a transient UStaticMesh from current vertex/index data and enable Nanite
    if (!StaticMeshComponent)
    {
        return;
    }

    // Hide PMC, show StaticMesh
    ProceduralMesh->SetVisibility(false);
    StaticMeshComponent->SetVisibility(true);

    UStaticMesh* NewMesh = NewObject<UStaticMesh>(this, NAME_None, RF_Transient);
    if (!NewMesh)
    {
        return;
    }

    FMeshDescription MeshDesc;
    FStaticMeshAttributes Attributes(MeshDesc);
    Attributes.Register();
    TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
    TPolygonGroupAttributesRef<FName> MaterialSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
    // Optional attributes not required for initial build
    FPolygonGroupID PolyGroup = MeshDesc.CreatePolygonGroup();
    MaterialSlotNames[PolyGroup] = FName("Cave");

    // Create vertices
    TArray<FVertexID> VertexIDs;
    VertexIDs.Reserve(Vertices.Num());
    for (const FVector& V : Vertices)
    {
        const FVertexID Vid = MeshDesc.CreateVertex();
        Positions[Vid] = (FVector3f)V;
        VertexIDs.Add(Vid);
    }

    // Create triangles
    for (int32 t = 0; t < Triangles.Num(); t += 3)
    {
        const int32 I0 = Triangles[t + 0];
        const int32 I1 = Triangles[t + 1];
        const int32 I2 = Triangles[t + 2];

        FVertexInstanceID VI0 = MeshDesc.CreateVertexInstance(FVertexID(I0));
        FVertexInstanceID VI1 = MeshDesc.CreateVertexInstance(FVertexID(I1));
        FVertexInstanceID VI2 = MeshDesc.CreateVertexInstance(FVertexID(I2));

        TArray<FVertexInstanceID> Inst;
        Inst.SetNumUninitialized(3);
        Inst[0] = VI0;
        Inst[1] = VI2; // inward winding
        Inst[2] = VI1;
        MeshDesc.CreatePolygon(PolyGroup, Inst);
    }

    // Build mesh
    NewMesh->BuildFromMeshDescriptions({ &MeshDesc });
    NewMesh->NaniteSettings.bEnabled = true;
    NewMesh->NaniteSettings.bPreserveArea = true;
    NewMesh->CommitMeshDescription(0);

    // Assign
    StaticMeshComponent->SetStaticMesh(NewMesh);
    StaticMeshComponent->SetCastShadow(true);
    StaticMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    if (CaveMaterialOverride)
    {
        StaticMeshComponent->SetMaterial(0, CaveMaterialOverride);
    }
}

float ACaveChunk::SampleDensity(FVector LocalPosition)
{
    FVector WorldPos = GetActorLocation() + LocalPosition;
    
    // Create a simple cave shape using noise
    float noise = SimplexNoise3D(WorldPos * 0.005f);
    
    // Add a second octave
    noise += SimplexNoise3D(WorldPos * 0.01f) * 0.5f;
    
    // Create cave by inverting
    float density = -noise;
    
    // Add a ground plane
    density += (WorldPos.Z / 1000.0f);
    
    return density;
}

float ACaveChunk::GenerateDensityAt(FVector WorldPosition) const
{
    // Large open cave generation
    // Use much larger scale for bigger cave systems
    FVector NoisePos = WorldPosition * 0.002f;  // Much larger scale
    
    // Generate large-scale cave structure
    float Density = 0.0f;
    
    // Primary cave structure - very large scale
    Density += FMath::PerlinNoise3D(NoisePos * 0.3f) * 1.0f;
    
    // Secondary structure - medium scale for some variation
    Density += FMath::PerlinNoise3D(NoisePos * 0.8f) * 0.3f;
    
    // Create cave by inverting (negative values = caves)
    Density = -Density;
    
    // Add bias for more open space
    Density += 0.2f;
    
    // Add gentle vertical gradient (less aggressive)
    float HeightGradient = (WorldPosition.Z - 5000.0f) / 20000.0f;
    Density += HeightGradient * 0.2f;
    
    // Create large open chambers more frequently
    float ChamberNoise = FMath::PerlinNoise3D(NoisePos * 0.05f);
    if (ChamberNoise < -0.1f)  // More frequent large chambers
    {
        Density -= 1.5f;  // Create very large open chambers
    }
    
    // Add some large tunnel systems
    float TunnelNoise = FMath::PerlinNoise3D(NoisePos * 0.1f);
    if (TunnelNoise < -0.2f)
    {
        Density -= 0.8f;  // Create large tunnels
    }
    
    return Density;
}

void ACaveChunk::GenerateDensityField()
{
    int32 SampleSize = ChunkSize + 1;  // Need N+1 samples for N voxels
    int32 FieldSize = SampleSize * SampleSize * SampleSize;
    DensityField.SetNum(FieldSize);
    
    for (int32 Z = 0; Z <= ChunkSize; Z++)
    {
        for (int32 Y = 0; Y <= ChunkSize; Y++)
        {
            for (int32 X = 0; X <= ChunkSize; X++)
            {
                FVector LocalPos = FVector(X, Y, Z) * VoxelSize;
                FVector WorldPos = GetActorLocation() + LocalPos;
                
                int32 Index = X + Y * SampleSize + Z * SampleSize * SampleSize;
                DensityField[Index] = GenerateDensityAt(WorldPos);
            }
        }
    }
    
    UE_LOG(LogTemp, Warning, TEXT("Generated density field for chunk %s with %d samples"), 
           *ChunkCoord.ToString(), FieldSize);
}

void ACaveChunk::GenerateMarchingCubes()
{
    // Parallel implementation: split Z-slices and process in parallel, then merge
    TArray<FVector> CombinedVertices;
    TArray<int32> CombinedTriangles;
    CombinedVertices.Reserve(Vertices.Num());
    CombinedTriangles.Reserve(Triangles.Num());

    // Partition work by Z-slice (exact coverage, no gaps)
    const int32 NumTasks = FMath::Clamp(ChunkSize, 1, 8); // up to 8 slices parallel

    TArray<TArray<FVector>> TaskVertices;
    TArray<TArray<int32>> TaskTriangles;
    TaskVertices.SetNum(NumTasks);
    TaskTriangles.SetNum(NumTasks);

    // Snapshot immutable inputs for thread safety
    const int32 SampleSize = ChunkSize + 1;
    const int32 LocalChunkSize = ChunkSize;
    const float LocalVoxelSize = VoxelSize;
    const float* DensityPtr = DensityField.GetData();

    ParallelFor(NumTasks, [this, &TaskVertices, &TaskTriangles, NumTasks, DensityPtr, SampleSize, LocalChunkSize, LocalVoxelSize](int32 TaskIndex)
    {
        const int32 ZStart = (LocalChunkSize * TaskIndex) / NumTasks;
        const int32 ZEnd = (LocalChunkSize * (TaskIndex + 1)) / NumTasks;
        TArray<FVector>& LocalVerts = TaskVertices[TaskIndex];
        TArray<int32>& LocalTris = TaskTriangles[TaskIndex];
        
        for (int32 Z = ZStart; Z < ZEnd; Z++)
        {
            for (int32 Y = 0; Y < LocalChunkSize; Y++)
            {
                for (int32 X = 0; X < LocalChunkSize; X++)
                {
                    MarchCubeToBuffers(X, Y, Z, DensityPtr, SampleSize, LocalChunkSize, LocalVoxelSize, LocalVerts, LocalTris);
                }
            }
        }
    });

    // Merge results and fix indices (single-threaded to avoid races)
    for (int32 i = 0; i < NumTasks; i++)
    {
        const TArray<FVector>& LocalVerts = TaskVertices[i];
        const TArray<int32>& LocalTris = TaskTriangles[i];
        
        const int32 OldVertexCount = CombinedVertices.Num();
        CombinedVertices.Append(LocalVerts);
        
        for (int32 t = 0; t < LocalTris.Num(); t++)
        {
            CombinedTriangles.Add(LocalTris[t] + OldVertexCount);
        }
    }

    // Commit under lock
    {
        FScopeLock Lock(&MeshDataMutex);
        Vertices = MoveTemp(CombinedVertices);
        Triangles = MoveTemp(CombinedTriangles);
    }

    UE_LOG(LogTemp, Warning, TEXT("Marching Cubes (parallel) generated %d vertices, %d triangles for chunk %s"), 
           Vertices.Num(), Triangles.Num() / 3, *ChunkCoord.ToString());
}

void ACaveChunk::MarchCube(int32 X, int32 Y, int32 Z)
{
    // Get the 8 corner values of the cube from the density field
    float Corners[8];
    for (int32 i = 0; i < 8; i++)
    {
        int32 CornerX = X + MarchingCubes::VertexOffsets[i].X;
        int32 CornerY = Y + MarchingCubes::VertexOffsets[i].Y;
        int32 CornerZ = Z + MarchingCubes::VertexOffsets[i].Z;
        
        int32 Index = CornerX + CornerY * (ChunkSize + 1) + 
                     CornerZ * (ChunkSize + 1) * (ChunkSize + 1);
        
        if (DensityField.IsValidIndex(Index))
        {
            Corners[i] = DensityField[Index];
        }
        else
        {
            Corners[i] = 1.0f; // Solid by default if out of bounds
        }
    }
    
    // Get cube configuration
    int32 CubeIndex = GetCubeConfiguration(Corners);
    
    // Skip if cube is entirely inside or outside
    if (MarchingCubes::EdgeTable[CubeIndex] == 0)
    {
        return;
    }
    
    // Find the vertices where the surface intersects the cube
    FVector VertexList[12];
    
    for (int32 i = 0; i < 12; i++)
    {
        if (MarchingCubes::EdgeTable[CubeIndex] & (1 << i))
        {
            int32 Edge1 = MarchingCubes::EdgeConnections[i][0];
            int32 Edge2 = MarchingCubes::EdgeConnections[i][1];
            
            FVector P1 = FVector(X, Y, Z) + FVector(MarchingCubes::VertexOffsets[Edge1]);
            FVector P2 = FVector(X, Y, Z) + FVector(MarchingCubes::VertexOffsets[Edge2]);
            
            P1 *= VoxelSize;
            P2 *= VoxelSize;
            
            VertexList[i] = InterpolateVertex(P1, P2, Corners[Edge1], Corners[Edge2]);
        }
    }
    
    // Create triangles based on the configuration
    for (int32 i = 0; MarchingCubes::TriTable[CubeIndex][i] != -1; i += 3)
    {
        int32 V1 = Vertices.Add(VertexList[MarchingCubes::TriTable[CubeIndex][i]]);
        int32 V2 = Vertices.Add(VertexList[MarchingCubes::TriTable[CubeIndex][i + 1]]);
        int32 V3 = Vertices.Add(VertexList[MarchingCubes::TriTable[CubeIndex][i + 2]]);
        
        // Flip winding so triangles face inward (visible from inside caves)
        Triangles.Add(V1);
        Triangles.Add(V3);
        Triangles.Add(V2);
    }
}

void ACaveChunk::MarchCubeToBuffers(int32 X, int32 Y, int32 Z,
                                    const float* DensityData, int32 SampleSize, int32 LocalChunkSize, float LocalVoxelSize,
                                    TArray<FVector>& OutVertices, TArray<int32>& OutTriangles)
{
    float Corners[8];
    for (int32 i = 0; i < 8; i++)
    {
        const int32 CornerX = X + MarchingCubes::VertexOffsets[i].X;
        const int32 CornerY = Y + MarchingCubes::VertexOffsets[i].Y;
        const int32 CornerZ = Z + MarchingCubes::VertexOffsets[i].Z;

        const int32 Index = CornerX + CornerY * SampleSize + CornerZ * SampleSize * SampleSize;

        const bool bInBounds = (CornerX >= 0 && CornerX <= LocalChunkSize &&
                                 CornerY >= 0 && CornerY <= LocalChunkSize &&
                                 CornerZ >= 0 && CornerZ <= LocalChunkSize);
        Corners[i] = bInBounds ? DensityData[Index] : 1.0f;
    }

    const int32 CubeIndex = GetCubeConfiguration(Corners);
    if (MarchingCubes::EdgeTable[CubeIndex] == 0)
    {
        return;
    }

    FVector VertexList[12];
    for (int32 i = 0; i < 12; i++)
    {
        if (MarchingCubes::EdgeTable[CubeIndex] & (1 << i))
        {
            const int32 Edge1 = MarchingCubes::EdgeConnections[i][0];
            const int32 Edge2 = MarchingCubes::EdgeConnections[i][1];
            FVector P1 = FVector(X, Y, Z) + FVector(MarchingCubes::VertexOffsets[Edge1]);
            FVector P2 = FVector(X, Y, Z) + FVector(MarchingCubes::VertexOffsets[Edge2]);
            P1 *= LocalVoxelSize;
            P2 *= LocalVoxelSize;
            VertexList[i] = InterpolateVertex(P1, P2, Corners[Edge1], Corners[Edge2]);
        }
    }

    for (int32 i = 0; MarchingCubes::TriTable[CubeIndex][i] != -1; i += 3)
    {
        const int32 LocalBase = OutVertices.Num();
        OutVertices.Add(VertexList[MarchingCubes::TriTable[CubeIndex][i]]);
        OutVertices.Add(VertexList[MarchingCubes::TriTable[CubeIndex][i + 1]]);
        OutVertices.Add(VertexList[MarchingCubes::TriTable[CubeIndex][i + 2]]);

        // inward-facing winding
        OutTriangles.Add(LocalBase + 0);
        OutTriangles.Add(LocalBase + 2);
        OutTriangles.Add(LocalBase + 1);
    }
}


void ACaveChunk::GenerateMeshAsync(FIntVector ChunkCoordinate, float InVoxelSize, int32 InChunkSize)
{
	if (!ProceduralMesh || bIsGenerating)
	{
		return;
	}

	bIsGenerating = true;
	ChunkCoord = ChunkCoordinate;
	VoxelSize = InVoxelSize;
	ChunkSize = InChunkSize;

	// Validate chunk size
	if (ChunkSize <= 0 || ChunkSize > 128)
	{
		UE_LOG(LogTemp, Error, TEXT("Invalid chunk size: %d"), ChunkSize);
		bIsGenerating = false;
		return;
	}

	// Clear previous mesh data and position the actor
	ClearMesh();
	FVector WorldPosition = FVector(ChunkCoord) * ChunkSize * VoxelSize;
	SetActorLocation(WorldPosition);

	// Capture values for background work to avoid touching UObjects off-thread
	const int32 LocalChunkSize = ChunkSize;
	const float LocalVoxelSize = VoxelSize;
	const FVector CapturedActorLocation = GetActorLocation();
	TWeakObjectPtr<ACaveChunk> WeakThis(this);

	Async(EAsyncExecution::ThreadPool, [WeakThis, LocalChunkSize, LocalVoxelSize, CapturedActorLocation]()
	{
		// Compute density field on background thread
		const int32 SampleSize = LocalChunkSize + 1;
		const int32 FieldSize = SampleSize * SampleSize * SampleSize;
		TArray<float> TempDensity;
		TempDensity.SetNum(FieldSize);

		for (int32 Z = 0; Z <= LocalChunkSize; Z++)
		{
			for (int32 Y = 0; Y <= LocalChunkSize; Y++)
			{
				for (int32 X = 0; X <= LocalChunkSize; X++)
				{
					const FVector LocalPos = FVector(X, Y, Z) * LocalVoxelSize;
					const FVector WorldPos = CapturedActorLocation + LocalPos;
					const int32 Index = X + Y * SampleSize + Z * SampleSize * SampleSize;

					// Safe call: GenerateDensityAt is const and pure math
					if (WeakThis.IsValid())
					{
						TempDensity[Index] = WeakThis->GenerateDensityAt(WorldPos);
					}
				}
			}
		}

		// Enqueue mesh build on game thread
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Temp = MoveTemp(TempDensity), SampleSize]() mutable
		{
			if (!WeakThis.IsValid())
			{
				return;
			}

			ACaveChunk* Chunk = WeakThis.Get();
			{
				FScopeLock Lock(&Chunk->MeshDataMutex);
				Chunk->DensityField = MoveTemp(Temp);
			}

			// Build mesh synchronously on game thread
			Chunk->Vertices.Empty();
			Chunk->Triangles.Empty();
			Chunk->GenerateMarchingCubes();

			if (Chunk->Vertices.Num() > 0)
			{
				Chunk->CalculateNormals();
				Chunk->GenerateUVs();
				Chunk->ProceduralMesh->CreateMeshSection(0, Chunk->Vertices, Chunk->Triangles, Chunk->Normals,
					Chunk->UVs, Chunk->VertexColors, Chunk->Tangents, true);

				if (Chunk->CaveMaterialOverride)
				{
					Chunk->ProceduralMesh->SetMaterial(0, Chunk->CaveMaterialOverride);
				}
				else
				{
					UMaterial* FallbackMaterial = Cast<UMaterial>(StaticLoadObject(UMaterial::StaticClass(), nullptr,
						TEXT("/Engine/BasicShapes/BasicShapeMaterial")));
					if (FallbackMaterial)
					{
						Chunk->ProceduralMesh->SetMaterial(0, FallbackMaterial);
					}
				}

				Chunk->ProceduralMesh->SetCastShadow(true);
				Chunk->ProceduralMesh->SetReceivesDecals(true);
				Chunk->ProceduralMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
				Chunk->ProceduralMesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
			}

			Chunk->bIsGenerating = false;
		});
	});
}

void ACaveChunk::BuildMeshOnGameThread()
{
    // This function is called when async generation is complete
    // For now, it's a no-op since we're using synchronous generation
}

void ACaveChunk::ModifyTerrain(FVector WorldLocation, float Radius, float Strength)
{
    // TODO: Implement terrain modification
    // This would modify the density field and regenerate the mesh
    UE_LOG(LogTemp, Warning, TEXT("ModifyTerrain called at %s with radius %f, strength %f"), 
           *WorldLocation.ToString(), Radius, Strength);
}

void ACaveChunk::ResetChunk()
{
    // Clear the mesh and reset chunk data
    if (ProceduralMesh)
    {
        ProceduralMesh->ClearAllMeshSections();
    }
    
    Vertices.Empty();
    Triangles.Empty();
    Normals.Empty();
    UVs.Empty();
    VertexColors.Empty();
    Tangents.Empty();
}

void ACaveChunk::SetGenerationSettings(float InNoiseFrequency, int32 InNoiseOctaves, 
                                      float InNoiseLacunarity, float InNoisePersistence, 
                                      float InCaveThreshold)
{
    NoiseFrequency = InNoiseFrequency;
    NoiseOctaves = InNoiseOctaves;
    NoiseLacunarity = InNoiseLacunarity;
    NoisePersistence = InNoisePersistence;
    CaveThreshold = InCaveThreshold;
}

void ACaveChunk::ClearMesh()
{
    if (ProceduralMesh)
    {
        ProceduralMesh->ClearAllMeshSections();
    }
    
    // Clear mesh data arrays
    Vertices.Empty();
    Triangles.Empty();
    Normals.Empty();
    UVs.Empty();
    VertexColors.Empty();
    Tangents.Empty();
    DensityField.Empty();
}

void ACaveChunk::SetLODLevel(int32 LODLevel)
{
    // TODO: Implement LOD system
    UE_LOG(LogTemp, Warning, TEXT("SetLODLevel called with LOD %d"), LODLevel);
}

FVector ACaveChunk::InterpolateVertex(FVector P1, FVector P2, float V1, float V2) const
{
    if (FMath::Abs(CaveThreshold - V1) < 0.00001f)
    {
        return P1;
    }
    if (FMath::Abs(CaveThreshold - V2) < 0.00001f)
    {
        return P2;
    }
    if (FMath::Abs(V1 - V2) < 0.00001f)
    {
        return P1;
    }
    
    float T = (CaveThreshold - V1) / (V2 - V1);
    return P1 + T * (P2 - P1);
}

int32 ACaveChunk::GetCubeConfiguration(float Corners[8]) const
{
    int32 CubeIndex = 0;
    for (int32 i = 0; i < 8; i++)
    {
        if (Corners[i] < CaveThreshold)
        {
            CubeIndex |= (1 << i);
        }
    }
    return CubeIndex;
}

float ACaveChunk::FractalNoise(FVector Position, int32 Octaves, float Frequency, 
                               float Lacunarity, float Persistence) const
{
    float Value = 0.0f;
    float Amplitude = 1.0f;
    float MaxValue = 0.0f;
    
    for (int32 i = 0; i < Octaves; i++)
    {
        Value += SimplexNoise3D(Position * Frequency) * Amplitude;
        MaxValue += Amplitude;
        
        Frequency *= Lacunarity;
        Amplitude *= Persistence;
    }
    
    return Value / MaxValue;
}

void ACaveChunk::CalculateNormals()
{
	Normals.SetNum(Vertices.Num());

	// Fast path: with our mesh generation each triangle has unique vertices.
	// Compute per-face normals in parallel and assign directly.
	const int32 NumTriangles = Triangles.Num() / 3;
	ParallelFor(NumTriangles, [this](int32 TriIdx)
	{
		const int32 Base = TriIdx * 3;
		const int32 I1 = Triangles[Base + 0];
		const int32 I2 = Triangles[Base + 1];
		const int32 I3 = Triangles[Base + 2];

		if (!Vertices.IsValidIndex(I1) || !Vertices.IsValidIndex(I2) || !Vertices.IsValidIndex(I3))
		{
			return;
		}

		const FVector V1 = Vertices[I1];
		const FVector V2 = Vertices[I2];
		const FVector V3 = Vertices[I3];
		const FVector FaceNormal = FVector::CrossProduct(V2 - V1, V3 - V1).GetSafeNormal();

		Normals[I1] = FaceNormal;
		Normals[I2] = FaceNormal;
		Normals[I3] = FaceNormal;
	});
}

void ACaveChunk::GenerateUVs()
{
    UVs.SetNum(Vertices.Num());
    
    // Simple triplanar UV mapping
    for (int32 i = 0; i < Vertices.Num(); i++)
    {
        FVector Vertex = Vertices[i];
        
        // For now, simple UV based on world position
        UVs[i] = FVector2D(Vertex.X * 0.01f, Vertex.Y * 0.01f);
    }
}