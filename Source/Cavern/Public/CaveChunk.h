#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralMeshComponent.h"
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
	void ResetChunk();
	
	UFUNCTION(BlueprintCallable, Category = "Cave Chunk")
	void SetGenerationSettings(float InNoiseFrequency, int32 InNoiseOctaves, 
							  float InNoiseLacunarity, float InNoisePersistence, 
							  float InCaveThreshold);
	
	UFUNCTION(BlueprintCallable, Category = "Cave Chunk")
	void ClearMesh();

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
	int32 MinVerticesForDeduplication = 80000; // Avoid dedup cost for smaller chunks

	// Prefer sort-based dedup (usually faster) over hash-based
	UPROPERTY(EditAnywhere, Category = "Cave|Optimization")
	bool bUseSortBasedDeduplication = true;

	// Optional material override applied to the procedural mesh
	UPROPERTY(EditAnywhere, Category = "Cave|Rendering")
	UMaterialInterface* CaveMaterialOverride = nullptr;

	// Memory retention options
	UPROPERTY(EditAnywhere, Category = "Cave|Memory")
	bool bKeepMeshDataCPU = true; // Keep CPU-side arrays after creating mesh section

	UPROPERTY(EditAnywhere, Category = "Cave|Memory")
	bool bKeepDensityField = true; // Keep density field after mesh build

	// Smoothing settings
    UPROPERTY(EditAnywhere, Category = "Cave|Smoothing", meta = (ClampMin = "0", ClampMax = "20"))
    int32 SmoothingIterations = 5;
    
    UPROPERTY(EditAnywhere, Category = "Cave|Smoothing")
    bool bEnableSmoothing = true;
    
    UPROPERTY(EditAnywhere, Category = "Cave|Smoothing", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float SmoothingLambda = 0.5f;
    
    UPROPERTY(EditAnywhere, Category = "Cave|Smoothing", meta = (ClampMin = "-1.0", ClampMax = "0.0"))
    float SmoothingMu = -0.53f;
	
protected:
	virtual void BeginPlay() override;
	
private:
	UPROPERTY(VisibleAnywhere)
	UProceduralMeshComponent* ProceduralMesh;
	
	FIntVector ChunkCoord;
	float VoxelSize;
	int32 ChunkSize;
	
	// Safety and state tracking
	bool bIsGenerating;
	FCriticalSection MeshDataMutex;
	
	// Mesh data
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	
	// Density field for marching cubes
	TArray<float> DensityField;
	
	// Generation parameters
	float NoiseFrequency = 0.001f;  // Large scale for big caves
	int32 NoiseOctaves = 2;         // Fewer octaves for smoother caves
	float NoiseLacunarity = 2.0f;
	float NoisePersistence = 0.3f;  // Lower for smoother transitions
	float CaveThreshold = 0.1f;    // More open space
	
	// Generation functions
	float GenerateDensityAt(FVector WorldPosition) const;
	void GenerateDensityField();
	void GenerateMarchingCubes();
	
	// Optimized mesh construction and normals
	void BuildMeshFromDensityFieldCached(const float* DensityData, int32 SampleSize, int32 LocalChunkSize, float LocalVoxelSize, const FVector& ActorLocation,
											 TArray<FVector>& OutVertices, TArray<int32>& OutTriangles, TArray<FVector>& OutNormals) const;
	FVector ComputeDensityGradient(const FVector& WorldPosition, float Epsilon) const;
	void CalculateNormalsFromDensity(float Epsilon);
	
	// Helper functions
	FVector InterpolateVertex(FVector P1, FVector P2, float V1, float V2) const;
	int32 GetCubeConfiguration(float Corners[8]) const;
	float PerlinNoise3D(FVector Position) const;
	void GenerateUVs();

	// Deduplication methods
	void DeduplicateVertices();
	void DeduplicateVerticesWithNormalAveraging();
	void RemapTriangleIndices(const TArray<int32>& RemapTable);

	void ApplyTaubinSmoothing(TArray<FVector>& InOutVertices, const TArray<int32>& InTriangles, 
		float Lambda = 0.5f, float Mu = -0.53f, int32 Iterations = 10);
		
	static void ApplyTaubinSmoothingStatic(TArray<FVector>& InOutVertices, const TArray<int32>& InTriangles,
					 float Lambda, float Mu, int32 Iterations);
};