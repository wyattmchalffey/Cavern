// CaveWorldSubsystem.cpp
#include "CaveWorldSubsystem.h"
#include "CaveChunk.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "DrawDebugHelpers.h"
#include "TimerManager.h"
#include "Async/Async.h"
#include "HAL/RunnableThread.h"
#include "Engine/Engine.h"

// Constructor
UCaveWorldSubsystem::UCaveWorldSubsystem()
{
    // Default settings
    VoxelSize = 50.0f;
    ChunkSize = 64;
    ViewDistance = 5;
    MaxActiveChunks = 2000;
    ChunksPerFrame = 5;
    bUseAsyncGeneration = true;
    bDebugDrawChunkBounds = false;
    
    // Generation settings - Large open caves
    NoiseFrequency = 0.002f;  // Much larger scale for bigger caves
    NoiseOctaves = 2;         // Fewer octaves for smoother caves
    NoiseLacunarity = 2.0f;
    NoisePersistence = 0.3f;  // Lower persistence for smoother transitions
    CaveThreshold = 0.0f;    // Higher threshold for more open space
}

void UCaveWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    
    UE_LOG(LogTemp, Warning, TEXT("Cave World Subsystem Initialized"));
    
    // Initialize generation queue
    ChunkGenerationQueue.Empty();
    ChunksInQueue.Empty();
    ActiveChunks.Empty();
    ChunkPool.Empty();
    
    // Start the update timer
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().SetTimer(
            UpdateTimerHandle,
            this,
            &UCaveWorldSubsystem::TickUpdate,
            0.1f, // Update every 100ms
            true
        );
    }
}

void UCaveWorldSubsystem::Deinitialize()
{
    // Stop the update timer
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(UpdateTimerHandle);
    }
    
    // Clean up all chunks
    CleanupAllChunks();
    
    // Clear all containers
    ChunkGenerationQueue.Empty();
    ChunksInQueue.Empty();
    ActiveChunks.Empty();
    ChunkPool.Empty();
    
    UE_LOG(LogTemp, Warning, TEXT("Cave World Subsystem Deinitialized"));
    
    Super::Deinitialize();
}

void UCaveWorldSubsystem::GenerateChunkAt(FIntVector ChunkCoordinate)
{
    // Check if chunk already exists
    if (ActiveChunks.Contains(ChunkCoordinate))
    {
        return;
    }
    
    // Check if chunk is already queued
    if (ChunksInQueue.Contains(ChunkCoordinate))
    {
        return;
    }
    
    // Add to generation queue
    FChunkGenerationTask Task;
    Task.Coordinate = ChunkCoordinate;
    Task.Priority = CalculateChunkPriority(ChunkCoordinate);
    
    ChunkGenerationQueue.Add(Task);
    ChunksInQueue.Add(ChunkCoordinate);
    
    // Sort queue by priority (higher priority first)
    ChunkGenerationQueue.Sort([](const FChunkGenerationTask& A, const FChunkGenerationTask& B)
    {
        return A.Priority > B.Priority;
    });
}

void UCaveWorldSubsystem::UpdateAroundPlayer(FVector PlayerLocation)
{
    LastPlayerPosition = PlayerLocation;
    FIntVector PlayerChunk = WorldToChunkCoordinate(PlayerLocation);
    
    // Track chunks that should exist
    TSet<FIntVector> RequiredChunks;
    
    // Generate chunks in view distance
    for (int32 X = -ViewDistance; X <= ViewDistance; X++)
    {
        for (int32 Y = -ViewDistance; Y <= ViewDistance; Y++)
        {
            for (int32 Z = -2; Z <= 2; Z++) // Limit vertical range
            {
                FIntVector ChunkCoord = PlayerChunk + FIntVector(X, Y, Z);
                
                // Calculate distance from player chunk
                float Distance = FMath::Sqrt(static_cast<float>(X * X + Y * Y + Z * Z));
                
                // Only generate if within spherical distance
                if (Distance <= ViewDistance)
                {
                    RequiredChunks.Add(ChunkCoord);
                    
                    // Queue for generation if doesn't exist
                    if (!ActiveChunks.Contains(ChunkCoord))
                    {
                        GenerateChunkAt(ChunkCoord);
                    }
                }
            }
        }
    }
    
    // Clean up distant chunks
    CleanupDistantChunks(RequiredChunks);
}

void UCaveWorldSubsystem::ModifyTerrainAt(FVector WorldLocation, float Radius, float Strength)
{
    // Find affected chunks
    float ChunkWorldSize = ChunkSize * VoxelSize;
    int32 ChunkRadius = FMath::CeilToInt(Radius / ChunkWorldSize) + 1;
    
    FIntVector CenterChunk = WorldToChunkCoordinate(WorldLocation);
    
    // Queue affected chunks for regeneration
    for (int32 X = -ChunkRadius; X <= ChunkRadius; X++)
    {
        for (int32 Y = -ChunkRadius; Y <= ChunkRadius; Y++)
        {
            for (int32 Z = -ChunkRadius; Z <= ChunkRadius; Z++)
            {
                FIntVector ChunkCoord = CenterChunk + FIntVector(X, Y, Z);
                
                // Check if chunk is active
                if (FChunkData* ChunkData = ActiveChunks.Find(ChunkCoord))
                {
                    if (ChunkData->ChunkActor)
                    {
                        // Apply modification to chunk
                        ChunkData->ChunkActor->ModifyTerrain(WorldLocation, Radius, Strength);
                        
                        // Mark chunk as dirty
                        ChunkData->bNeedsRebuild = true;
                    }
                }
            }
        }
    }
}

float UCaveWorldSubsystem::SampleDensityAt(FVector WorldLocation) const
{
    // This would normally sample from the density field
    // For now, return a simple noise-based value
    float Noise = FMath::PerlinNoise3D(WorldLocation * NoiseFrequency);
    return Noise;
}

FIntVector UCaveWorldSubsystem::WorldToChunkCoordinate(FVector WorldLocation) const
{
    float ChunkWorldSize = ChunkSize * VoxelSize;
    return FIntVector(
        FMath::FloorToInt(WorldLocation.X / ChunkWorldSize),
        FMath::FloorToInt(WorldLocation.Y / ChunkWorldSize),
        FMath::FloorToInt(WorldLocation.Z / ChunkWorldSize)
    );
}

FVector UCaveWorldSubsystem::ChunkToWorldPosition(FIntVector ChunkCoordinate) const
{
    float ChunkWorldSize = ChunkSize * VoxelSize;
    return FVector(ChunkCoordinate) * ChunkWorldSize;
}

void UCaveWorldSubsystem::TickUpdate()
{
    ProcessGenerationQueue();
    UpdateChunkLODs();
    
    // Debug visualization
    if (bDebugDrawChunkBounds)
    {
        DrawDebugChunks();
    }
}

void UCaveWorldSubsystem::ProcessGenerationQueue()
{
    if (ChunkGenerationQueue.Num() == 0)
    {
        return;
    }
    
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }
    
    int32 ChunksProcessed = 0;
    
    while (ChunkGenerationQueue.Num() > 0 && ChunksProcessed < ChunksPerFrame)
    {
        // Get highest priority chunk
        FChunkGenerationTask Task = ChunkGenerationQueue[0];
        ChunkGenerationQueue.RemoveAt(0);
        ChunksInQueue.Remove(Task.Coordinate);
        
        // Skip if chunk was already created
        if (ActiveChunks.Contains(Task.Coordinate))
        {
            continue;
        }
        
        // Create or reuse chunk actor
        ACaveChunk* ChunkActor = GetOrCreateChunkActor();
        
        if (!ChunkActor)
        {
            UE_LOG(LogTemp, Error, TEXT("Failed to create chunk actor"));
            continue;
        }
        
        // Make sure the actor is valid
        if (!IsValid(ChunkActor))
        {
            UE_LOG(LogTemp, Error, TEXT("Chunk actor is invalid"));
            continue;
        }
        
        // Position the chunk
        FVector WorldPos = ChunkToWorldPosition(Task.Coordinate);
        ChunkActor->SetActorLocation(WorldPos);
        
        // Use async or sync generation based on setting
        if (bUseAsyncGeneration)
        {
            ChunkActor->GenerateMeshAsync(Task.Coordinate, VoxelSize, ChunkSize);
        }
        else
        {
            ChunkActor->GenerateMesh(Task.Coordinate, VoxelSize, ChunkSize);
        }
        OnChunkGenerated(Task.Coordinate, ChunkActor);
        
        ChunksProcessed++;
    }
}

void UCaveWorldSubsystem::OnChunkGenerated(FIntVector Coordinate, ACaveChunk* ChunkActor)
{
    // Add to active chunks
    FChunkData& ChunkData = ActiveChunks.FindOrAdd(Coordinate);
    ChunkData.Coordinate = Coordinate;
    ChunkData.bIsGenerated = true;
    ChunkData.ChunkActor = ChunkActor;
    ChunkData.GenerationTime = GetWorld()->GetTimeSeconds();
    ChunkData.LastAccessTime = ChunkData.GenerationTime;
    
    // Notify listeners
    OnChunkGeneratedDelegate.Broadcast(Coordinate);
    
    UE_LOG(LogTemp, Log, TEXT("Chunk generated at %s. Total active chunks: %d"), 
           *Coordinate.ToString(), ActiveChunks.Num());
}

ACaveChunk* UCaveWorldSubsystem::GetOrCreateChunkActor()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return nullptr;
    }
    
    ACaveChunk* ChunkActor = nullptr;
    
    // Try to get from pool first
    if (ChunkPool.Num() > 0)
    {
        ChunkActor = ChunkPool.Pop();
        ChunkActor->SetActorHiddenInGame(false);
        ChunkActor->SetActorEnableCollision(true);
        ChunkActor->ResetChunk();
    }
    else
    {
        // Create new chunk actor
        FActorSpawnParameters SpawnParams;
        SpawnParams.Name = MakeUniqueObjectName(World, ACaveChunk::StaticClass(), 
                                                TEXT("CaveChunk"));
        SpawnParams.SpawnCollisionHandlingOverride = 
            ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        
        ChunkActor = World->SpawnActor<ACaveChunk>(ACaveChunk::StaticClass(), 
                                                    FVector::ZeroVector, 
                                                    FRotator::ZeroRotator, 
                                                    SpawnParams);
        
        if (ChunkActor)
        {
            // Configure chunk settings
            ChunkActor->SetGenerationSettings(NoiseFrequency, NoiseOctaves, 
                                             NoiseLacunarity, NoisePersistence, 
                                             CaveThreshold);
        }
    }
    
    return ChunkActor;
}

void UCaveWorldSubsystem::ReturnChunkToPool(ACaveChunk* ChunkActor)
{
    if (!ChunkActor)
    {
        return;
    }
    
    // Avoid clearing/destroying while generation is in progress
    if (ChunkActor->IsGenerating())
    {
        return;
    }

    // Hide and disable the chunk
    ChunkActor->SetActorHiddenInGame(true);
    ChunkActor->SetActorEnableCollision(false);
    ChunkActor->ClearMesh();
    
    // Add to pool if not at capacity
    if (ChunkPool.Num() < MaxPoolSize)
    {
        ChunkPool.Add(ChunkActor);
    }
    else
    {
        // Destroy if pool is full
        ChunkActor->Destroy();
    }
}

void UCaveWorldSubsystem::CleanupDistantChunks(const TSet<FIntVector>& RequiredChunks)
{
    TArray<FIntVector> ChunksToRemove;
    
    // Find chunks that are no longer needed
    for (const auto& Pair : ActiveChunks)
    {
        if (!RequiredChunks.Contains(Pair.Key))
        {
            ChunksToRemove.Add(Pair.Key);
        }
    }
    
    // Remove distant chunks
    for (const FIntVector& Coord : ChunksToRemove)
    {
        if (FChunkData* ChunkData = ActiveChunks.Find(Coord))
        {
            if (ChunkData->ChunkActor)
            {
                if (ChunkData->ChunkActor->IsGenerating())
                {
                    continue;
                }
                ReturnChunkToPool(ChunkData->ChunkActor);
            }
            ActiveChunks.Remove(Coord);
            
            UE_LOG(LogTemp, Verbose, TEXT("Removed distant chunk at %s"), 
                   *Coord.ToString());
        }
    }
}

void UCaveWorldSubsystem::CleanupAllChunks()
{
    // Destroy all active chunks
    for (auto& Pair : ActiveChunks)
    {
        if (Pair.Value.ChunkActor)
        {
            Pair.Value.ChunkActor->Destroy();
        }
    }
    ActiveChunks.Empty();
    
    // Destroy pooled chunks
    for (ACaveChunk* ChunkActor : ChunkPool)
    {
        if (ChunkActor)
        {
            ChunkActor->Destroy();
        }
    }
    ChunkPool.Empty();
}

void UCaveWorldSubsystem::UpdateChunkLODs()
{
    if (!LastPlayerPosition.IsSet())
    {
        return;
    }
    
    FVector PlayerPos = LastPlayerPosition.GetValue();
    
    for (auto& Pair : ActiveChunks)
    {
        FChunkData& ChunkData = Pair.Value;
        if (ChunkData.ChunkActor)
        {
            FVector ChunkCenter = ChunkToWorldPosition(ChunkData.Coordinate) + 
                                 FVector(ChunkSize * VoxelSize * 50.0f);
            float Distance = FVector::Distance(PlayerPos, ChunkCenter);
            
            // Calculate LOD based on distance
            int32 TargetLOD = 0;
            if (Distance > 5000.0f) TargetLOD = 1;
            if (Distance > 10000.0f) TargetLOD = 2;
            if (Distance > 20000.0f) TargetLOD = 3;
            
            if (ChunkData.CurrentLOD != TargetLOD)
            {
                ChunkData.ChunkActor->SetLODLevel(TargetLOD);
                ChunkData.CurrentLOD = TargetLOD;
            }
        }
    }
}

float UCaveWorldSubsystem::CalculateChunkPriority(FIntVector ChunkCoordinate) const
{
    if (!LastPlayerPosition.IsSet())
    {
        return 0.0f;
    }
    
    FVector PlayerPos = LastPlayerPosition.GetValue();
    FVector ChunkCenter = ChunkToWorldPosition(ChunkCoordinate) + 
                         FVector(ChunkSize * VoxelSize * 50.0f);
    
    float Distance = FVector::Distance(PlayerPos, ChunkCenter);
    
    // Inverse distance for priority (closer = higher priority)
    return 10000.0f / (Distance + 1.0f);
}

void UCaveWorldSubsystem::DrawDebugChunks()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }
    
    float ChunkWorldSize = ChunkSize * VoxelSize;
    
    for (const auto& Pair : ActiveChunks)
    {
        const FChunkData& ChunkData = Pair.Value;
        FVector ChunkPos = ChunkToWorldPosition(ChunkData.Coordinate);
        FVector ChunkCenter = ChunkPos + FVector(ChunkWorldSize * 0.5f);
        
        // Color based on state
        FColor DebugColor = FColor::Green;
        if (!ChunkData.bIsGenerated)
        {
            DebugColor = FColor::Yellow;
        }
        else if (ChunkData.bNeedsRebuild)
        {
            DebugColor = FColor::Orange;
        }
        
        // Draw chunk bounds
        DrawDebugBox(World, ChunkCenter, FVector(ChunkWorldSize * 0.5f), 
                    DebugColor, false, 0.1f, 0, 2.0f);
        
        // Draw chunk coordinate
        DrawDebugString(World, ChunkCenter, ChunkData.Coordinate.ToString(), 
                       nullptr, DebugColor, 0.1f, true);
    }
    
    // Draw player position
    if (LastPlayerPosition.IsSet())
    {
        DrawDebugSphere(World, LastPlayerPosition.GetValue(), 100.0f, 
                       12, FColor::Red, false, 0.1f, 0, 2.0f);
    }
}

void UCaveWorldSubsystem::GetChunkStatistics(int32& OutTotalChunks, int32& OutActiveChunks, 
                                            int32& OutPooledChunks, int32& OutQueuedChunks) const
{
    OutTotalChunks = ActiveChunks.Num() + ChunkPool.Num();
    OutActiveChunks = ActiveChunks.Num();
    OutPooledChunks = ChunkPool.Num();
    OutQueuedChunks = ChunkGenerationQueue.Num();
}

void UCaveWorldSubsystem::SetDebugDrawEnabled(bool bEnabled)
{
    bDebugDrawChunkBounds = bEnabled;
}

void UCaveWorldSubsystem::RegenerateAllChunks()
{
    // Clear existing chunks
    CleanupAllChunks();
    
    // Clear generation queue
    ChunkGenerationQueue.Empty();
    ChunksInQueue.Empty();
    
    // Regenerate around player if position is known
    if (LastPlayerPosition.IsSet())
    {
        UpdateAroundPlayer(LastPlayerPosition.GetValue());
    }
    
    UE_LOG(LogTemp, Warning, TEXT("All chunks regenerated"));
}