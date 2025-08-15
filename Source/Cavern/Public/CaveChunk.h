#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "CaveChunk.generated.h"

USTRUCT()
struct FVertexKey
{
	GENERATED_BODY()
	
	int32 X;
	int32 Y; 
	int32 Z;
	
	FVertexKey() : X(0), Y(0), Z(0) {}
	
	FVertexKey(const FVector& Position, float GridSize)
	{
		// Quantize position to grid
		X = FMath::RoundToInt(Position.X / GridSize);
		Y = FMath::RoundToInt(Position.Y / GridSize);
		Z = FMath::RoundToInt(Position.Z / GridSize);
	}
	
	bool operator==(const FVertexKey& Other) const
	{
		return X == Other.X && Y == Other.Y && Z == Other.Z;
	}
	
	friend uint32 GetTypeHash(const FVertexKey& Key)
	{
		// Combine the three coordinates into a single hash
		uint32 Hash = FCrc::MemCrc32(&Key.X, sizeof(int32));
		Hash = HashCombine(Hash, FCrc::MemCrc32(&Key.Y, sizeof(int32)));
		Hash = HashCombine(Hash, FCrc::MemCrc32(&Key.Z, sizeof(int32)));
		return Hash;
	}
};

class UProceduralMeshComponent;

UCLASS()
class CAVERN_API ACaveChunk : public AActor
{
	GENERATED_BODY()
	
public:
	ACaveChunk();
	
	UFUNCTION(BlueprintCallable, Category = "Cave Chunk")
	void GenerateMesh(FIntVector ChunkCoordinate, float InVoxelSize, int32 InChunkSize);
	
	UFUNCTION(BlueprintCallable, Category = "Cave Chunk")
	void GenerateMeshAsync(FIntVector ChunkCoordinate, float InVoxelSize, int32 InChunkSize);
	
	UFUNCTION(BlueprintCallable, Category = "Cave Chunk")
	void BuildMeshOnGameThread();
	
	UFUNCTION(BlueprintCallable, Category = "Cave Chunk")
	void ModifyTerrain(FVector WorldLocation, float Radius, float Strength);
	
	UFUNCTION(BlueprintCallable, Category = "Cave Chunk")
	void ResetChunk();
	
	UFUNCTION(BlueprintCallable, Category = "Cave Chunk")
	void SetGenerationSettings(float InNoiseFrequency, int32 InNoiseOctaves, 
							  float InNoiseLacunarity, float InNoisePersistence, 
							  float InCaveThreshold);
	
	UFUNCTION(BlueprintCallable, Category = "Cave Chunk")
	void ClearMesh();
	
	UFUNCTION(BlueprintCallable, Category = "Cave Chunk")
	void SetLODLevel(int32 LODLevel);

	// Generation state accessor to prevent pooling/destroy during builds
	UFUNCTION(BlueprintCallable, Category = "Cave Chunk")
	bool IsGenerating() const { return bIsGenerating; }

	// Vertex deduplication settings
	UPROPERTY(EditAnywhere, Category = "Cave|Optimization", meta = (ClampMin = "0.001", ClampMax = "10.0"))
	float VertexMergeDistance = 0.1f;  // Merge vertices within 1mm by default
	
	UPROPERTY(EditAnywhere, Category = "Cave|Optimization")
	bool bEnableVertexDeduplication = true;
	
	UPROPERTY(EditAnywhere, Category = "Cave|Optimization")
	bool bAverageNormalsOnMerge = true;  // Average normals for smooth shading

	// Only run (costly) deduplication when vertex count exceeds this threshold
	UPROPERTY(EditAnywhere, Category = "Cave|Optimization", meta = (ClampMin = "0"))
	int32 MinVerticesForDeduplication = 0;

	// Prefer sort-based dedup (usually faster) over hash-based
	UPROPERTY(EditAnywhere, Category = "Cave|Optimization")
	bool bUseSortBasedDeduplication = true;
	
protected:
	virtual void BeginPlay() override;
	
private:
	UPROPERTY(VisibleAnywhere)
	UProceduralMeshComponent* ProceduralMesh;
	
	UPROPERTY(VisibleAnywhere)
	UStaticMeshComponent* StaticMeshComponent;
	
	// Optional material override applied to the procedural mesh
	UPROPERTY(EditAnywhere, Category = "Cave|Rendering")
	UMaterialInterface* CaveMaterialOverride = nullptr;
	
	FIntVector ChunkCoord;
	float VoxelSize;
	int32 ChunkSize;
	
	// Safety and state tracking
	bool bIsGenerating;
	FCriticalSection MeshDataMutex;  // ADD THIS!
	
	// Mesh data
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FColor> VertexColors;
	TArray<FProcMeshTangent> Tangents;
	
	// Density field for marching cubes
	TArray<float> DensityField;
	
	// Generation parameters (ADD THESE!)
	float NoiseFrequency = 0.001f;  // Large scale for big caves
	int32 NoiseOctaves = 2;         // Fewer octaves for smoother caves
	float NoiseLacunarity = 2.0f;
	float NoisePersistence = 0.3f;  // Lower for smoother transitions
	float CaveThreshold = 0.1f;    // More open space
	
	// Generation functions
	float SampleDensity(FVector LocalPosition);
	float GenerateDensityAt(FVector WorldPosition) const;  // ADD THIS!
	void GenerateDensityField();
	void GenerateMarchingCubes();
	void MarchCube(int32 X, int32 Y, int32 Z);  // CHANGE PARAMETER TYPE!
	void MarchCubeToBuffers(int32 X, int32 Y, int32 Z,
							const float* DensityData, int32 SampleSize, int32 LocalChunkSize, float LocalVoxelSize,
							TArray<FVector>& OutVertices, TArray<int32>& OutTriangles);
	
	// Nanite static mesh generation
	void BuildNaniteStaticMesh();
	
	// Rendering mode
	UPROPERTY(EditAnywhere, Category = "Cave|Rendering")
	bool bUseNaniteStaticMesh = false;
	
	// Helper functions (ADD THESE!)
	FVector InterpolateVertex(FVector P1, FVector P2, float V1, float V2) const;
	int32 GetCubeConfiguration(float Corners[8]) const;
	float SimplexNoise3D(FVector Position) const;
	float FractalNoise(FVector Position, int32 Octaves, float Frequency, 
					  float Lacunarity, float Persistence) const;
	void CalculateNormals();
	void GenerateUVs();

	// Deduplication methods
	void DeduplicateVertices();
	void DeduplicateVerticesWithNormalAveraging();
	void RemapTriangleIndices(const TArray<int32>& RemapTable);

	// Debug stats
	int32 VerticesBeforeDedup = 0;
	int32 VerticesAfterDedup = 0;
	float DeduplicationTime = 0.0f;
};