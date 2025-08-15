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

// Forward declaration for async dedup helper
static void DeduplicateVerticesAsync(TArray<FVector>& Vertices, TArray<int32>& Triangles, float MergeDistance);
static void DeduplicateVerticesAsync_Sort(TArray<FVector>& Vertices, TArray<int32>& Triangles, float MergeDistance);

// Helper for edge indexing within a slice for cached marching cubes
static FORCEINLINE int32 EdgeKey(int32 x, int32 y, int32 z, int32 edge, int32 sampleSize)
{
    // Pack x,y,z,edge into an int key; sampleSize <= 129 so 8 bits per coord is enough, but we keep safe shifts
    return (((z * sampleSize) + y) * sampleSize + x) * 16 + edge;
}


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

	// NEW: Apply vertex deduplication before creating the mesh section
	if (bEnableVertexDeduplication && Vertices.Num() > 0)
	{
		double StartTime = FPlatformTime::Seconds();
		VerticesBeforeDedup = Vertices.Num();
		if (bAverageNormalsOnMerge)
		{
			DeduplicateVerticesWithNormalAveraging();
		}
		else
		{
			DeduplicateVertices();
		}
		VerticesAfterDedup = Vertices.Num();
		DeduplicationTime = (FPlatformTime::Seconds() - StartTime) * 1000.0f;
		UE_LOG(LogTemp, Warning, TEXT("Vertex Deduplication: %d -> %d vertices (%.1f%% reduction) in %.2fms"),
			VerticesBeforeDedup,
			VerticesAfterDedup,
			(1.0f - (float)VerticesAfterDedup / (float)VerticesBeforeDedup) * 100.0f,
			DeduplicationTime);
	}
	
	// Calculate normals and UVs if we have vertices
	if (Vertices.Num() > 0)
	{
		// Ensure normals exist; prefer gradient-based from density
		if (Normals.Num() != Vertices.Num())
		{
			CalculateNormalsFromDensity(VoxelSize * 0.5f);
		}
		GenerateUVs();
		
		if (bUseNaniteStaticMesh)
		{
			// Use Nanite path
			BuildNaniteStaticMesh();
		}
		else
		{
			// Use procedural mesh path
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
			
			if (CaveMaterialOverride)
			{
				ProceduralMesh->SetMaterial(0, CaveMaterialOverride);
			}
			
			// Enable proper lighting on the procedural mesh
			ProceduralMesh->SetCastShadow(true);
			ProceduralMesh->SetReceivesDecals(true);
			ProceduralMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			ProceduralMesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		}
		
		// Optionally free CPU-side buffers to reduce memory
		if (!bKeepMeshDataCPU)
		{
			Vertices.Empty();
			Triangles.Empty();
			Normals.Empty();
			UVs.Empty();
			VertexColors.Empty();
			Tangents.Empty();
		}

		if (!bKeepDensityField)
		{
			DensityField.Empty();
		}

		UE_LOG(LogTemp, Warning, TEXT("Generated chunk with %d vertices"), Vertices.Num());
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

	// Configure Nanite before build
	NewMesh->NaniteSettings.bEnabled = true;
	NewMesh->NaniteSettings.bPreserveArea = true;
	
	// Commit and build static mesh
	NewMesh->CommitMeshDescription(0);
	TArray<FText> BuildErrors;
	NewMesh->Build(false, &BuildErrors);
	NewMesh->CalculateExtendedBounds();

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
	// Optimized cached edge vertex construction
	const int32 SampleSize = ChunkSize + 1;
	const int32 LocalChunkSize = ChunkSize;
	const float LocalVoxelSize = VoxelSize;
	const float* DensityPtr = DensityField.GetData();

	TArray<FVector> OutVerts;
	TArray<int32> OutTris;
	TArray<FVector> OutNormals; // will be recomputed from density later
	BuildMeshFromDensityFieldCached(DensityPtr, SampleSize, LocalChunkSize, LocalVoxelSize, GetActorLocation(), OutVerts, OutTris, OutNormals);

	// Commit under lock
	{
		FScopeLock Lock(&MeshDataMutex);
		Vertices = MoveTemp(OutVerts);
		Triangles = MoveTemp(OutTris);
		Normals = MoveTemp(OutNormals);
	}

	UE_LOG(LogTemp, Warning, TEXT("Marching Cubes (cached) generated %d vertices, %d triangles for chunk %s"),
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
	const float LocalCaveThreshold = CaveThreshold;
	const float LocalMergeDistance = VertexMergeDistance;
	const bool bLocalEnableDedup = bEnableVertexDeduplication;
	const FVector CapturedActorLocation = GetActorLocation();
	TWeakObjectPtr<ACaveChunk> WeakThis(this);

	Async(EAsyncExecution::ThreadPool, [WeakThis, LocalChunkSize, LocalVoxelSize, LocalCaveThreshold, LocalMergeDistance, bLocalEnableDedup, CapturedActorLocation]()
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

		// Build mesh off-thread from density field using cached edges
		TArray<FVector> TempVertices;
		TArray<int32> TempTriangles;
		TArray<FVector> TempNormals;

		// Local static helpers to avoid touching UObject state
		auto InterpolateVertexLocal = [LocalCaveThreshold](const FVector& P1, const FVector& P2, float V1, float V2) -> FVector
		{
			if (FMath::Abs(LocalCaveThreshold - V1) < 0.00001f) return P1;
			if (FMath::Abs(LocalCaveThreshold - V2) < 0.00001f) return P2;
			if (FMath::Abs(V1 - V2) < 0.00001f) return P1;
			const float T = (LocalCaveThreshold - V1) / (V2 - V1);
			return P1 + T * (P2 - P1);
		};

		const float* DensityPtr = TempDensity.GetData();
		if (WeakThis.IsValid())
		{
			WeakThis->BuildMeshFromDensityFieldCached(DensityPtr, SampleSize, LocalChunkSize, LocalVoxelSize, CapturedActorLocation, TempVertices, TempTriangles, TempNormals);
		}

		// Optional dedup on background thread with simple timing
		int32 DedupBeforeVerts = TempVertices.Num();
		int32 DedupAfterVerts = TempVertices.Num();
		double DedupMs = 0.0;
		bool bDidDedup = false;
		if (bLocalEnableDedup && TempVertices.Num() > 0)
		{
			// Skip if below threshold captured from object on game-thread (0 means always)
			int32 Threshold = 0;
			if (WeakThis.IsValid())
			{
				Threshold = WeakThis->MinVerticesForDeduplication;
			}
			if (Threshold == 0 || DedupBeforeVerts >= Threshold)
			{
				const double StartTime = FPlatformTime::Seconds();
				bool bSortBased = true;
				if (WeakThis.IsValid())
				{
					bSortBased = WeakThis->bUseSortBasedDeduplication;
				}
				if (bSortBased)
				{
					DeduplicateVerticesAsync_Sort(TempVertices, TempTriangles, LocalMergeDistance);
				}
				else
				{
					DeduplicateVerticesAsync(TempVertices, TempTriangles, LocalMergeDistance);
				}
				DedupMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
				DedupAfterVerts = TempVertices.Num();
				bDidDedup = true;
			}
		}

		// Enqueue GPU upload and component updates on game thread
		AsyncTask(ENamedThreads::GameThread, [WeakThis, TempDen = MoveTemp(TempDensity), SampleSize, Verts = MoveTemp(TempVertices), Tris = MoveTemp(TempTriangles), Norms = MoveTemp(TempNormals), bDidDedup, DedupBeforeVerts, DedupAfterVerts, DedupMs]() mutable
		{
			if (!WeakThis.IsValid())
			{
				return;
			}

			ACaveChunk* Chunk = WeakThis.Get();
			{
				FScopeLock Lock(&Chunk->MeshDataMutex);
				Chunk->DensityField = MoveTemp(TempDen);
			}

			// Assign mesh data from background thread
			Chunk->Vertices = MoveTemp(Verts);
			Chunk->Triangles = MoveTemp(Tris);
			Chunk->Normals = MoveTemp(Norms);

			if (bDidDedup)
			{
				const float ReductionPct = (DedupBeforeVerts > 0) ? (1.0f - (float)DedupAfterVerts / (float)DedupBeforeVerts) * 100.0f : 0.0f;
				UE_LOG(LogTemp, Warning, TEXT("Vertex Deduplication (Async): %d -> %d vertices (%.1f%% reduction) in %.2fms"),
					DedupBeforeVerts, DedupAfterVerts, ReductionPct, (float)DedupMs);
			}

			if (Chunk->Vertices.Num() > 0)
			{
				// Normals produced from density gradients; fallback if empty/mismatched
				if (Chunk->Normals.Num() != Chunk->Vertices.Num())
				{
					Chunk->CalculateNormalsFromDensity(Chunk->VoxelSize * 0.5f);
				}
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

				// Optionally free CPU buffers after upload
				if (!Chunk->bKeepMeshDataCPU)
				{
					Chunk->Vertices.Empty();
					Chunk->Triangles.Empty();
					Chunk->Normals.Empty();
					Chunk->UVs.Empty();
					Chunk->VertexColors.Empty();
					Chunk->Tangents.Empty();
				}
				if (!Chunk->bKeepDensityField)
				{
					Chunk->DensityField.Empty();
				}
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

FVector ACaveChunk::ComputeDensityGradient(const FVector& WorldPosition, float Epsilon) const
{
    // Central differences sampling GenerateDensityAt in world space
    const FVector Ex(Epsilon, 0, 0);
    const FVector Ey(0, Epsilon, 0);
    const FVector Ez(0, 0, Epsilon);

    const float dx = GenerateDensityAt(WorldPosition + Ex) - GenerateDensityAt(WorldPosition - Ex);
    const float dy = GenerateDensityAt(WorldPosition + Ey) - GenerateDensityAt(WorldPosition - Ey);
    const float dz = GenerateDensityAt(WorldPosition + Ez) - GenerateDensityAt(WorldPosition - Ez);

    const FVector g(dx, dy, dz);
    return -g.GetSafeNormal(); // negative gradient points from solid to empty for iso-surface
}

void ACaveChunk::CalculateNormalsFromDensity(float Epsilon)
{
    Normals.SetNum(Vertices.Num());
    const FVector ActorLoc = GetActorLocation();
    ParallelFor(Vertices.Num(), [this, ActorLoc, Epsilon](int32 i)
    {
        const FVector WorldV = ActorLoc + Vertices[i];
        Normals[i] = ComputeDensityGradient(WorldV, Epsilon);
    });
}

void ACaveChunk::BuildMeshFromDensityFieldCached(const float* DensityData, int32 SampleSize, int32 LocalChunkSize, float LocalVoxelSize, const FVector& ActorLocation,
                                                TArray<FVector>& OutVertices, TArray<int32>& OutTriangles, TArray<FVector>& OutNormals) const
{
    OutVertices.Reset();
    OutTriangles.Reset();
    OutNormals.Reset();
    const int32 CellCount = LocalChunkSize * LocalChunkSize * LocalChunkSize;
    OutVertices.Reserve(CellCount * 3);
    OutTriangles.Reserve(CellCount * 6);

    // Edge caches for a single Z slice: 3 edges per voxel (X-edge, Y-edge, Z-edge)
    TMap<int32, int32> EdgeVertexCache; // key -> vertex index
    EdgeVertexCache.Reserve(LocalChunkSize * LocalChunkSize * 3);

    auto Sample = [DensityData, SampleSize, LocalChunkSize](int32 x, int32 y, int32 z) -> float
    {
        // Clamp to bounds to avoid OOB
        x = FMath::Clamp(x, 0, LocalChunkSize);
        y = FMath::Clamp(y, 0, LocalChunkSize);
        z = FMath::Clamp(z, 0, LocalChunkSize);
        return DensityData[x + y * SampleSize + z * SampleSize * SampleSize];
    };

    const float LocalThreshold = CaveThreshold;
    auto Interp = [LocalThreshold](const FVector& P1, const FVector& P2, float V1, float V2) -> FVector
    {
        if (FMath::Abs(LocalThreshold - V1) < 0.00001f) return P1;
        if (FMath::Abs(LocalThreshold - V2) < 0.00001f) return P2;
        if (FMath::Abs(V1 - V2) < 0.00001f) return P1;
        const float T = (LocalThreshold - V1) / (V2 - V1);
        return P1 + T * (P2 - P1);
    };

    for (int32 z = 0; z < LocalChunkSize; ++z)
    {
        for (int32 y = 0; y < LocalChunkSize; ++y)
        {
            for (int32 x = 0; x < LocalChunkSize; ++x)
            {
                float c[8];
                for (int i = 0; i < 8; ++i)
                {
                    const int32 cx = x + MarchingCubes::VertexOffsets[i].X;
                    const int32 cy = y + MarchingCubes::VertexOffsets[i].Y;
                    const int32 cz = z + MarchingCubes::VertexOffsets[i].Z;
                    c[i] = Sample(cx, cy, cz);
                }

                const int32 cubeIndex = GetCubeConfiguration(c);
                const int32 edges = MarchingCubes::EdgeTable[cubeIndex];
                if (edges == 0) { continue; }

                int32 vlist[12];

                auto GetOrCreateEdgeVertex = [&](int ex, int ey, int ez, int axis, const FVector& P1, const FVector& P2, float V1, float V2) -> int32
                {
                    const int32 key = EdgeKey(ex, ey, ez, axis, SampleSize);
                    if (int32* found = EdgeVertexCache.Find(key))
                    {
                        return *found;
                    }
                    const FVector pos = Interp(P1, P2, V1, V2);
                    const int32 idx = OutVertices.Add(pos);
                    EdgeVertexCache.Add(key, idx);
                    return idx;
                };

                // Compute up to 12 edge vertices
                for (int i = 0; i < 12; ++i)
                {
                    if (edges & (1 << i))
                    {
                        const int e1 = MarchingCubes::EdgeConnections[i][0];
                        const int e2 = MarchingCubes::EdgeConnections[i][1];
                        FVector P1 = FVector(x, y, z) + FVector(MarchingCubes::VertexOffsets[e1]);
                        FVector P2 = FVector(x, y, z) + FVector(MarchingCubes::VertexOffsets[e2]);
                        P1 *= LocalVoxelSize;
                        P2 *= LocalVoxelSize;

                        const float V1 = c[e1];
                        const float V2 = c[e2];

                        // Determine cache anchor (lowest of the two corner coords) for stability
                        const FIntVector v1o = MarchingCubes::VertexOffsets[e1];
                        const FIntVector v2o = MarchingCubes::VertexOffsets[e2];
                        const FIntVector d = v2o - v1o;
                        const int axis = (d.X != 0) ? 0 : ((d.Y != 0) ? 1 : 2);
                        const FIntVector a = FIntVector(x, y, z) + FIntVector(FMath::Min(v1o.X, v2o.X), FMath::Min(v1o.Y, v2o.Y), FMath::Min(v1o.Z, v2o.Z));
                        const int32 vidx = GetOrCreateEdgeVertex(a.X, a.Y, a.Z, axis, P1, P2, V1, V2);
                        vlist[i] = vidx;
                    }
                }

                // Emit triangles
                for (int i = 0; MarchingCubes::TriTable[cubeIndex][i] != -1; i += 3)
                {
                    const int ai = vlist[MarchingCubes::TriTable[cubeIndex][i + 0]];
                    const int bi = vlist[MarchingCubes::TriTable[cubeIndex][i + 1]];
                    const int ci = vlist[MarchingCubes::TriTable[cubeIndex][i + 2]];
                    // inward-facing winding
                    OutTriangles.Add(ai);
                    OutTriangles.Add(ci);
                    OutTriangles.Add(bi);
                }
            }
        }
    }

    // Compute normals from density gradient in world-space
    if (OutVertices.Num() == 0)
    {
        OutNormals.Reset();
        return;
    }
    OutNormals.SetNum(OutVertices.Num());
    const float eps = LocalVoxelSize * 0.5f;

    auto DensityAtLocal = [](const FVector& WorldPosition) -> float
    {
        // Match GenerateDensityAt without accessing 'this'
        const FVector NoisePos = WorldPosition * 0.002f;
        float Density = 0.0f;
        Density += FMath::PerlinNoise3D(NoisePos * 0.3f) * 1.0f;
        Density += FMath::PerlinNoise3D(NoisePos * 0.8f) * 0.3f;
        Density = -Density;
        Density += 0.2f;
        const float HeightGradient = (WorldPosition.Z - 5000.0f) / 20000.0f;
        Density += HeightGradient * 0.2f;
        const float ChamberNoise = FMath::PerlinNoise3D(NoisePos * 0.05f);
        if (ChamberNoise < -0.1f) { Density -= 1.5f; }
        const float TunnelNoise = FMath::PerlinNoise3D(NoisePos * 0.1f);
        if (TunnelNoise < -0.2f) { Density -= 0.8f; }
        return Density;
    };

    for (int32 i = 0; i < OutVertices.Num(); ++i)
    {
        const FVector worldPos = ActorLocation + OutVertices[i];
        const FVector Ex(eps, 0, 0), Ey(0, eps, 0), Ez(0, 0, eps);
        const float dx = DensityAtLocal(worldPos + Ex) - DensityAtLocal(worldPos - Ex);
        const float dy = DensityAtLocal(worldPos + Ey) - DensityAtLocal(worldPos - Ey);
        const float dz = DensityAtLocal(worldPos + Ez) - DensityAtLocal(worldPos - Ez);
        OutNormals[i] = -FVector(dx, dy, dz).GetSafeNormal();
    }
}

void ACaveChunk::GenerateUVs()
{
	UVs.SetNum(Vertices.Num());
	
	// Simple triplanar UV mapping
	for (int32 i = 0; i < Vertices.Num(); i++)
	{
		FVector Vertex = Vertices[i];
		
		// For now, simple UV based on world position (avoid seams across chunks)
		const FVector WorldV = GetActorLocation() + Vertex;
		UVs[i] = FVector2D(WorldV.X * 0.01f, WorldV.Y * 0.01f);
	}
}

// =========================
// Vertex deduplication impl
// =========================

void ACaveChunk::DeduplicateVertices()
{
	if (Vertices.Num() == 0 || Triangles.Num() == 0)
	{
		return;
	}

	TMap<FVertexKey, int32> UniqueVertexMap;
	TArray<FVector> UniqueVertices;
	TArray<int32> RemapTable;

	UniqueVertices.Reserve(Vertices.Num() / 3);
	RemapTable.SetNum(Vertices.Num());

	for (int32 OrigIndex = 0; OrigIndex < Vertices.Num(); OrigIndex++)
	{
		const FVector& Vertex = Vertices[OrigIndex];
		FVertexKey Key(Vertex, VertexMergeDistance);
		if (int32* ExistingIndex = UniqueVertexMap.Find(Key))
		{
			RemapTable[OrigIndex] = *ExistingIndex;
		}
		else
		{
			int32 NewIndex = UniqueVertices.Add(Vertex);
			UniqueVertexMap.Add(Key, NewIndex);
			RemapTable[OrigIndex] = NewIndex;
		}
	}

	RemapTriangleIndices(RemapTable);
	Vertices = MoveTemp(UniqueVertices);

	Normals.Empty();
	UVs.Empty();
	VertexColors.Empty();
	Tangents.Empty();
}

void ACaveChunk::DeduplicateVerticesWithNormalAveraging()
{
	if (Vertices.Num() == 0 || Triangles.Num() == 0)
	{
		return;
	}

	// First, calculate face normals for each triangle
	TArray<FVector> FaceNormals;
	FaceNormals.Reserve(Triangles.Num() / 3);
	for (int32 TriIdx = 0; TriIdx < Triangles.Num(); TriIdx += 3)
	{
		const FVector& V0 = Vertices[Triangles[TriIdx]];
		const FVector& V1 = Vertices[Triangles[TriIdx + 1]];
		const FVector& V2 = Vertices[Triangles[TriIdx + 2]];
		FVector FaceNormal = FVector::CrossProduct(V2 - V0, V1 - V0).GetSafeNormal();
		FaceNormals.Add(FaceNormal);
	}

	struct FVertexData
	{
		FVector Position;
		FVector AccumulatedNormal;
		int32 NormalCount;
		int32 NewIndex;
		
		FVertexData()
			: Position(FVector::ZeroVector)
			, AccumulatedNormal(FVector::ZeroVector)
			, NormalCount(0)
			, NewIndex(-1)
		{}
	};

	TMap<FVertexKey, FVertexData> UniqueVertexMap;
	TArray<int32> RemapTable;
	RemapTable.SetNum(Vertices.Num());

	// First pass: identify unique vertices and accumulate their normals
	for (int32 TriIdx = 0; TriIdx < Triangles.Num(); TriIdx += 3)
	{
		int32 FaceIndex = TriIdx / 3;
		const FVector& FaceNormal = FaceNormals[FaceIndex];
		for (int32 Corner = 0; Corner < 3; Corner++)
		{
			int32 VertexIndex = Triangles[TriIdx + Corner];
			const FVector& Vertex = Vertices[VertexIndex];
			FVertexKey Key(Vertex, VertexMergeDistance);
			FVertexData& Data = UniqueVertexMap.FindOrAdd(Key);
			if (Data.NormalCount == 0)
			{
				Data.Position = Vertex;
			}
			Data.AccumulatedNormal += FaceNormal;
			Data.NormalCount++;
		}
	}

	// Second pass: create final vertex array with averaged normals
	TArray<FVector> UniqueVertices;
	TArray<FVector> UniqueNormals;
	UniqueVertices.Reserve(UniqueVertexMap.Num());
	UniqueNormals.Reserve(UniqueVertexMap.Num());

	int32 NewIndex = 0;
	for (auto& Pair : UniqueVertexMap)
	{
		FVertexData& Data = Pair.Value;
		Data.NewIndex = NewIndex++;
		UniqueVertices.Add(Data.Position);
		UniqueNormals.Add(Data.AccumulatedNormal.GetSafeNormal());
	}

	// Third pass: build remap table
	for (int32 OrigIndex = 0; OrigIndex < Vertices.Num(); OrigIndex++)
	{
		const FVector& Vertex = Vertices[OrigIndex];
		FVertexKey Key(Vertex, VertexMergeDistance);
		if (FVertexData* Data = UniqueVertexMap.Find(Key))
		{
			RemapTable[OrigIndex] = Data->NewIndex;
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Vertex not found in deduplication map!"));
			RemapTable[OrigIndex] = 0;
		}
	}

	// Remap triangle indices
	RemapTriangleIndices(RemapTable);

	// Replace arrays with deduplicated versions
	Vertices = MoveTemp(UniqueVertices);
	Normals = MoveTemp(UniqueNormals);

	// Clear other arrays as they need to be recalculated
	UVs.Empty();
	VertexColors.Empty();
	Tangents.Empty();
}

void ACaveChunk::RemapTriangleIndices(const TArray<int32>& RemapTable)
{
	ParallelFor(Triangles.Num(), [this, &RemapTable](int32 Index)
	{
		Triangles[Index] = RemapTable[Triangles[Index]];
	});

	// Remove degenerates
	TArray<int32> CleanTriangles;
	CleanTriangles.Reserve(Triangles.Num());
	for (int32 TriIdx = 0; TriIdx < Triangles.Num(); TriIdx += 3)
	{
		int32 I0 = Triangles[TriIdx];
		int32 I1 = Triangles[TriIdx + 1];
		int32 I2 = Triangles[TriIdx + 2];
		if (I0 != I1 && I1 != I2 && I0 != I2)
		{
			CleanTriangles.Add(I0);
			CleanTriangles.Add(I1);
			CleanTriangles.Add(I2);
		}
	}
	if (CleanTriangles.Num() < Triangles.Num())
	{
		int32 DegenerateCount = (Triangles.Num() - CleanTriangles.Num()) / 3;
		UE_LOG(LogTemp, Log, TEXT("Removed %d degenerate triangles"), DegenerateCount);
		Triangles = MoveTemp(CleanTriangles);
	}
}

// Static helper function for async deduplication
static void DeduplicateVerticesAsync(TArray<FVector>& Vertices, TArray<int32>& Triangles, float MergeDistance)
{
	if (Vertices.Num() == 0 || Triangles.Num() == 0)
	{
		return;
	}

	struct FSpatialHash
	{
		TMap<uint64, TArray<int32>> Buckets;
		float GridSize;
		
		explicit FSpatialHash(float InGridSize) : GridSize(InGridSize) {}
		
		uint64 GetHash(const FVector& Pos) const
		{
			int32 X = FMath::FloorToInt(Pos.X / GridSize);
			int32 Y = FMath::FloorToInt(Pos.Y / GridSize);
			int32 Z = FMath::FloorToInt(Pos.Z / GridSize);
			uint64 Hash = 0;
			Hash |= (uint64(X & 0x1FFFFF) << 42);
			Hash |= (uint64(Y & 0x1FFFFF) << 21);
			Hash |= (uint64(Z & 0x1FFFFF) << 0);
			return Hash;
		}
		
		void Insert(int32 Index, const FVector& Pos)
		{
			uint64 Hash = GetHash(Pos);
			Buckets.FindOrAdd(Hash).Add(Index);
		}
		
		int32 FindDuplicate(const FVector& Pos, const TArray<FVector>& AllVertices) const
		{
			uint64 Hash = GetHash(Pos);
			for (int32 DX = -1; DX <= 1; DX++)
			{
				for (int32 DY = -1; DY <= 1; DY++)
				{
					for (int32 DZ = -1; DZ <= 1; DZ++)
					{
						FVector NeighborPos = Pos + FVector(DX * GridSize, DY * GridSize, DZ * GridSize);
						uint64 NeighborHash = GetHash(NeighborPos);
						if (const TArray<int32>* Bucket = Buckets.Find(NeighborHash))
						{
							for (int32 Index : *Bucket)
							{
								if (FVector::DistSquared(AllVertices[Index], Pos) < GridSize * GridSize)
								{
									return Index;
								}
							}
						}
					}
				}
			}
			return -1;
		}
	};

	FSpatialHash SpatialHash(MergeDistance);
	TArray<FVector> UniqueVertices;
	TArray<int32> RemapTable;
	RemapTable.SetNum(Vertices.Num());

	for (int32 i = 0; i < Vertices.Num(); i++)
	{
		int32 DuplicateIndex = SpatialHash.FindDuplicate(Vertices[i], UniqueVertices);
		if (DuplicateIndex >= 0)
		{
			RemapTable[i] = DuplicateIndex;
		}
		else
		{
			int32 NewIndex = UniqueVertices.Add(Vertices[i]);
			SpatialHash.Insert(NewIndex, Vertices[i]);
			RemapTable[i] = NewIndex;
		}
	}

	for (int32& Index : Triangles)
	{
		Index = RemapTable[Index];
	}

	Vertices = MoveTemp(UniqueVertices);
}

// Faster sort-based dedup suitable for async use (quantize + sort + unique)
static void DeduplicateVerticesAsync_Sort(TArray<FVector>& Vertices, TArray<int32>& Triangles, float MergeDistance)
{
    if (Vertices.Num() == 0 || Triangles.Num() == 0)
    {
        return;
    }

    const float InvGrid = (MergeDistance > 0.0f) ? (1.0f / MergeDistance) : 1e6f;

    struct FQuantizedVertex
    {
        int32 Qx;
        int32 Qy;
        int32 Qz;
        int32 OriginalIndex;
    };

    TArray<FQuantizedVertex> Q; Q.Reserve(Vertices.Num());
    for (int32 i = 0; i < Vertices.Num(); ++i)
    {
        const FVector& V = Vertices[i];
        FQuantizedVertex E{ FMath::RoundToInt(V.X * InvGrid), FMath::RoundToInt(V.Y * InvGrid), FMath::RoundToInt(V.Z * InvGrid), i };
        Q.Add(E);
    }

    Q.Sort([](const FQuantizedVertex& A, const FQuantizedVertex& B)
    {
        if (A.Qx != B.Qx) return A.Qx < B.Qx;
        if (A.Qy != B.Qy) return A.Qy < B.Qy;
        if (A.Qz != B.Qz) return A.Qz < B.Qz;
        return A.OriginalIndex < B.OriginalIndex;
    });

    TArray<int32> Remap; Remap.SetNumUninitialized(Vertices.Num());
    TArray<FVector> Unique; Unique.Reserve(Vertices.Num());

    int32 CurrentUniqueIndex = -1;
    int32 i = 0;
    while (i < Q.Num())
    {
        const int32 Start = i;
        const int32 Qx = Q[i].Qx, Qy = Q[i].Qy, Qz = Q[i].Qz;
        ++i;
        while (i < Q.Num() && Q[i].Qx == Qx && Q[i].Qy == Qy && Q[i].Qz == Qz)
        {
            ++i;
        }

        ++CurrentUniqueIndex;
        const FVector& Representative = Vertices[Q[Start].OriginalIndex];
        Unique.Add(Representative);
        for (int32 k = Start; k < i; ++k)
        {
            Remap[Q[k].OriginalIndex] = CurrentUniqueIndex;
        }
    }

    for (int32& Idx : Triangles)
    {
        Idx = Remap[Idx];
    }

    Vertices = MoveTemp(Unique);
}