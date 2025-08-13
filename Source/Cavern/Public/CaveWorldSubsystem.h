// CaveWorldSubsystem.h
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Engine/EngineTypes.h"
#include "CaveWorldSubsystem.generated.h"

// Forward declarations
class ACaveChunk;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnChunkGenerated, FIntVector, ChunkCoordinate);

USTRUCT(BlueprintType)
struct FChunkData
{
    GENERATED_BODY()
    
    UPROPERTY(BlueprintReadOnly)
    FIntVector Coordinate;
    
    UPROPERTY(BlueprintReadOnly)
    bool bIsGenerated = false;
    
    UPROPERTY(BlueprintReadOnly)
    bool bNeedsRebuild = false;
    
    UPROPERTY()
    class ACaveChunk* ChunkActor = nullptr;
    
    UPROPERTY()
    float GenerationTime = 0.0f;
    
    UPROPERTY()
    float LastAccessTime = 0.0f;
    
    UPROPERTY()
    int32 CurrentLOD = 0;
};

USTRUCT()
struct FChunkGenerationTask
{
    GENERATED_BODY()
    
    FIntVector Coordinate;
    float Priority;
    
    FChunkGenerationTask()
    {
        Coordinate = FIntVector::ZeroValue;
        Priority = 0.0f;
    }
};

UCLASS(BlueprintType, Blueprintable)
class CAVERN_API UCaveWorldSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()
    
public:
    UCaveWorldSubsystem();
    
    // ===== Subsystem Lifecycle =====
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    
    // ===== Core Functionality =====
    
    UFUNCTION(BlueprintCallable, Category = "Cave System")
    void GenerateChunkAt(FIntVector ChunkCoordinate);
    
    UFUNCTION(BlueprintCallable, Category = "Cave System")
    void UpdateAroundPlayer(FVector PlayerLocation);
    
    UFUNCTION(BlueprintCallable, Category = "Cave System")
    void ModifyTerrainAt(FVector WorldLocation, float Radius, float Strength);
    
    UFUNCTION(BlueprintPure, Category = "Cave System")
    float SampleDensityAt(FVector WorldLocation) const;
    
    UFUNCTION(BlueprintCallable, Category = "Cave System")
    void RegenerateAllChunks();
    
    // ===== Coordinate Conversion =====
    
    UFUNCTION(BlueprintPure, Category = "Cave System")
    FIntVector WorldToChunkCoordinate(FVector WorldLocation) const;
    
    UFUNCTION(BlueprintPure, Category = "Cave System")
    FVector ChunkToWorldPosition(FIntVector ChunkCoordinate) const;
    
    // ===== Statistics =====
    
    UFUNCTION(BlueprintPure, Category = "Cave System|Statistics")
    void GetChunkStatistics(int32& OutTotalChunks, int32& OutActiveChunks, 
                          int32& OutPooledChunks, int32& OutQueuedChunks) const;
    
    // ===== Debug =====
    
    UFUNCTION(BlueprintCallable, Category = "Cave System|Debug")
    void SetDebugDrawEnabled(bool bEnabled);
    
    // ===== Settings =====
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Settings|Generation")
    float VoxelSize;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Settings|Generation")
    int32 ChunkSize;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Settings|Generation")
    int32 ViewDistance;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Settings|Performance")
    int32 MaxActiveChunks;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Settings|Performance")
    int32 ChunksPerFrame;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Settings|Performance")
    bool bUseAsyncGeneration;
    
    // Noise parameters
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Settings|Noise", meta = (ClampMin = "0.001", ClampMax = "1.0"))
    float NoiseFrequency;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Settings|Noise", meta = (ClampMin = "1", ClampMax = "8"))
    int32 NoiseOctaves;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Settings|Noise", meta = (ClampMin = "1.0", ClampMax = "4.0"))
    float NoiseLacunarity;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Settings|Noise", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float NoisePersistence;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cave Settings|Noise", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
    float CaveThreshold;
    
    // ===== Events =====
    
    UPROPERTY(BlueprintAssignable, Category = "Cave System|Events")
    FOnChunkGenerated OnChunkGeneratedDelegate;
    
protected:
    // Internal update functions
    void TickUpdate();
    void ProcessGenerationQueue();
    void UpdateChunkLODs();
    void CleanupDistantChunks(const TSet<FIntVector>& RequiredChunks);
    void CleanupAllChunks();
    
    // Chunk management
    ACaveChunk* GetOrCreateChunkActor();
    void ReturnChunkToPool(ACaveChunk* ChunkActor);
    void OnChunkGenerated(FIntVector Coordinate, ACaveChunk* ChunkActor);
    
    // Utility functions
    float CalculateChunkPriority(FIntVector ChunkCoordinate) const;
    void DrawDebugChunks();
    
private:
    // Chunk storage
    UPROPERTY()
    TMap<FIntVector, FChunkData> ActiveChunks;
    
    // Chunk pooling
    UPROPERTY()
    TArray<ACaveChunk*> ChunkPool;
    
    // Generation queue
    TArray<FChunkGenerationTask> ChunkGenerationQueue;
    TSet<FIntVector> ChunksInQueue;
    
    // State tracking
    TOptional<FVector> LastPlayerPosition;
    FTimerHandle UpdateTimerHandle;
    
    // Constants
    static constexpr int32 MaxPoolSize = 50;
    
    // Debug
    bool bDebugDrawChunkBounds;
};