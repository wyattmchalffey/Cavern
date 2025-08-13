using UnrealBuildTool;

public class Cavern : ModuleRules
{
    public Cavern(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        
        PublicDependencyModuleNames.AddRange(new string[] { 
            "Core", 
            "CoreUObject", 
            "Engine", 
            "InputCore",
            "RenderCore",
            "RHI",
            "ProceduralMeshComponent",
            "Renderer",
            "Projects",
            "MeshDescription",
            "StaticMeshDescription"
        });
        
        PrivateDependencyModuleNames.AddRange(new string[] {
            "Slate",
            "SlateCore",
            "NaniteBuilder"
        });
    }
}