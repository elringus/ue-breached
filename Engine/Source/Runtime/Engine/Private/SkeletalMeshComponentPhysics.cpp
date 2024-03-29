﻿// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "EnginePrivate.h"
#include "PhysicsPublic.h"
#include "SkeletalRender.h"
#include "SkeletalRenderPublic.h"

#include "MessageLog.h"
#include "CollisionDebugDrawingPublic.h"

#if WITH_PHYSX
	#include "PhysicsEngine/PhysXSupport.h"
	#include "Collision/PhysXCollision.h"
#endif

#include "Collision/CollisionDebugDrawing.h"

#if WITH_APEX
	#include "NxParamUtils.h"
	#include "NxApex.h"

#if WITH_APEX_CLOTHING
	#include "NxClothingAsset.h"
	#include "NxClothingActor.h"
	#include "NxClothingCollision.h"
	// for cloth morph target	
	#include "Animation/VertexAnim/VertexAnimBase.h"
	#include "Animation/VertexAnim/MorphTarget.h"

#endif// #if WITH_APEX_CLOTHING

#endif//#if WITH_APEX
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/PhysicsAsset.h"

#define LOCTEXT_NAMESPACE "SkeletalMeshComponentPhysics"


extern TAutoConsoleVariable<int32> CVarEnableClothPhysics;

void FSkeletalMeshComponentPreClothTickFunction::ExecuteTick(float DeltaTime, enum ELevelTick TickType, ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
{
	QUICK_SCOPE_CYCLE_COUNTER(FSkeletalMeshComponentPreClothTickFunction_ExecuteTick);

	if ((TickType == LEVELTICK_All) && Target && !Target->HasAnyFlags(RF_PendingKill | RF_Unreachable))
	{
		Target->PreClothTick(DeltaTime, *this);
	}
}

FString FSkeletalMeshComponentPreClothTickFunction::DiagnosticMessage()
{
	return TEXT("FSkeletalMeshComponentPreClothTickFunction");
}


#if WITH_APEX_CLOTHING
//
//	FClothingActor
//
void FClothingActor::Clear(bool bReleaseResource)
{
	if(bReleaseResource)
	{
		GPhysCommandHandler->DeferredRelease(ApexClothingActor);
	}

	ParentClothingAsset = NULL;
	ApexClothingActor = NULL;
}

//
//	USkeletalMesh methods for clothing
//
void USkeletalMesh::LoadClothCollisionVolumes(int32 AssetIndex, NxClothingAsset* ApexClothingAsset)
{
	if(AssetIndex >= ClothingAssets.Num())
	{
		return;
	}

	FClothingAssetData& Asset = ClothingAssets[AssetIndex];

	check(ApexClothingAsset);

	const NxParameterized::Interface* AssetParams = ApexClothingAsset->getAssetNxParameterized();

	// load bone actors
	physx::PxI32 NumBoneActors;
	verify(NxParameterized::getParamArraySize(*AssetParams, "boneActors", NumBoneActors));

	// convexes are constructed with bone vertices
	physx::PxI32 NumBoneVertices;
	verify(NxParameterized::getParamArraySize(*AssetParams, "boneVertices", NumBoneVertices));

	Asset.ClothCollisionVolumes.Empty(NumBoneActors);

	char ParameterName[MAX_SPRINTF];

	for(int32 i=0; i<NumBoneActors; i++)
	{
		FApexClothCollisionVolumeData& CollisionData = Asset.ClothCollisionVolumes[Asset.ClothCollisionVolumes.AddZeroed()];

		FCStringAnsi::Sprintf(ParameterName, "boneActors[%d].boneIndex", i);
		verify(NxParameterized::getParamI32(*AssetParams, ParameterName, CollisionData.BoneIndex));
		FCStringAnsi::Sprintf(ParameterName, "boneActors[%d].convexVerticesCount", i);
		verify(NxParameterized::getParamU32(*AssetParams, ParameterName, CollisionData.ConvexVerticesCount));
		if(CollisionData.ConvexVerticesCount > 0)
		{
			CollisionData.BoneVertices.Empty(CollisionData.ConvexVerticesCount);
			FCStringAnsi::Sprintf(ParameterName, "boneActors[%d].convexVerticesStart", i);
			verify(NxParameterized::getParamU32(*AssetParams, ParameterName, CollisionData.ConvexVerticesStart));
			// read vertex data which compose a convex
			int32 NumMaxVertIndex = CollisionData.ConvexVerticesStart + CollisionData.ConvexVerticesCount;
			check( NumMaxVertIndex <= NumBoneVertices );

			for(int32 VertIdx=CollisionData.ConvexVerticesStart; VertIdx < NumMaxVertIndex; VertIdx++)
			{
				FCStringAnsi::Sprintf(ParameterName, "boneVertices[%d]", VertIdx);
				physx::PxVec3 BoneVertex;
				verify(NxParameterized::getParamVec3(*AssetParams, ParameterName, BoneVertex));
				CollisionData.BoneVertices.Add(P2UVector(BoneVertex));
			}
		}
		else
		{
			FCStringAnsi::Sprintf(ParameterName, "boneActors[%d].capsuleRadius", i);
			verify(NxParameterized::getParamF32(*AssetParams, ParameterName, CollisionData.CapsuleRadius));
			FCStringAnsi::Sprintf(ParameterName, "boneActors[%d].capsuleHeight", i);
			verify(NxParameterized::getParamF32(*AssetParams, ParameterName, CollisionData.CapsuleHeight));
			// local pose is only used for a capsule
			physx::PxMat44 PxLocalPose;
		    FCStringAnsi::Sprintf(ParameterName, "boneActors[%d].localPose", i);
		    verify(NxParameterized::getParamMat34(*AssetParams, ParameterName, PxLocalPose));
    
		    CollisionData.LocalPose = P2UMatrix(PxLocalPose);
		}
	}

	// load convex data
	physx::PxI32 NumConvexes;
	verify(NxParameterized::getParamArraySize(*AssetParams, "collisionConvexes", NumConvexes));

	Asset.ClothCollisionConvexPlaneIndices.Empty(NumConvexes);

	uint32 PlaneIndex;
	for(int32 i=0; i<NumConvexes; i++)
	{
		FCStringAnsi::Sprintf(ParameterName, "collisionConvexes[%d]", i);
		verify(NxParameterized::getParamU32(*AssetParams, ParameterName, PlaneIndex));
		Asset.ClothCollisionConvexPlaneIndices.Add(PlaneIndex);
	}

	// load plane data
	physx::PxI32 NumPlanes;
	verify(NxParameterized::getParamArraySize(*AssetParams, "bonePlanes", NumPlanes));

	physx::PxVec3 PlaneNormal;
	float		  PlaneDist;

	PxReal PlaneData[4];
	Asset.ClothCollisionVolumePlanes.Empty(NumPlanes);

	for(int32 PlaneIdx=0; PlaneIdx<NumPlanes; PlaneIdx++)
	{
		FClothBonePlane BonePlane;
		FCStringAnsi::Sprintf(ParameterName, "bonePlanes[%d].boneIndex", PlaneIdx);
		verify(NxParameterized::getParamI32(*AssetParams, ParameterName, BonePlane.BoneIndex));
		FCStringAnsi::Sprintf(ParameterName, "bonePlanes[%d].n", PlaneIdx);
		verify(NxParameterized::getParamVec3(*AssetParams, ParameterName, PlaneNormal));
		FCStringAnsi::Sprintf(ParameterName, "bonePlanes[%d].d", PlaneIdx);
		verify(NxParameterized::getParamF32(*AssetParams, ParameterName, PlaneDist));

		for(int i=0; i<3; i++)
		{
			PlaneData[i] = PlaneNormal[i];
		}

		PlaneData[3] = PlaneDist;
		BonePlane.PlaneData = P2UPlane(PlaneData);
		Asset.ClothCollisionVolumePlanes.Add(BonePlane);
	}

	// load bone spheres
	physx::PxI32 NumBoneSpheres;
	verify(NxParameterized::getParamArraySize(*AssetParams, "boneSpheres", NumBoneSpheres));

	Asset.ClothBoneSpheres.Empty(NumBoneSpheres);

	physx::PxVec3 LocalPosForBoneSphere;

	for(int32 i=0; i<NumBoneSpheres; i++)
	{
		FApexClothBoneSphereData& BoneSphere = Asset.ClothBoneSpheres[Asset.ClothBoneSpheres.AddZeroed()];

		FCStringAnsi::Sprintf(ParameterName, "boneSpheres[%d].boneIndex", i);
		verify(NxParameterized::getParamI32(*AssetParams, ParameterName, BoneSphere.BoneIndex));
		FCStringAnsi::Sprintf(ParameterName, "boneSpheres[%d].radius", i);
		verify(NxParameterized::getParamF32(*AssetParams, ParameterName, BoneSphere.Radius));
		FCStringAnsi::Sprintf(ParameterName, "boneSpheres[%d].localPos", i);		
		verify(NxParameterized::getParamVec3(*AssetParams, ParameterName, LocalPosForBoneSphere));
		BoneSphere.LocalPos = P2UVector(LocalPosForBoneSphere);
	}

	// load bone sphere connections, 2 bone spheres become a capsule by this connection info
	physx::PxI32 NumBoneSphereConnections;
	verify(NxParameterized::getParamArraySize(*AssetParams, "boneSphereConnections", NumBoneSphereConnections));

	Asset.BoneSphereConnections.Empty(NumBoneSphereConnections);

	for(int32 i=0; i<NumBoneSphereConnections; i++)
	{
		uint16 &ConnectionIndex = Asset.BoneSphereConnections[Asset.BoneSphereConnections.AddZeroed()];
		FCStringAnsi::Sprintf(ParameterName, "boneSphereConnections[%d]", i);
		verify(NxParameterized::getParamU16(*AssetParams, ParameterName, ConnectionIndex));
	}
}

bool USkeletalMesh::HasClothSectionsInAllLODs(int AssetIndex)
{
	bool bResult = false;

	for (int32 LODIndex = 0; LODIndex < LODInfo.Num(); LODIndex++)
	{
		bResult |= HasClothSections(LODIndex, AssetIndex);
	}

	return bResult;
}

bool USkeletalMesh::HasClothSections(int32 LODIndex, int AssetIndex)
{
	FSkeletalMeshResource* Resource = GetImportedResource();
	check(Resource->LODModels.IsValidIndex(LODIndex));

	FStaticLODModel& LODModel = Resource->LODModels[LODIndex];
	int32 NumSections = LODModel.Sections.Num();

	for(int32 SecIdx=0; SecIdx < NumSections; SecIdx++)
	{
		uint16 ChunkIdx = LODModel.Sections[SecIdx].ChunkIndex;

		if(LODModel.Chunks[ChunkIdx].CorrespondClothAssetIndex == AssetIndex)
		{
			return true;
		}
	}

	return false;
}

void USkeletalMesh::GetClothSectionIndices(int32 LODIndex, int32 AssetIndex, TArray<uint32>& OutSectionIndices)
{
	FSkeletalMeshResource* Resource = GetImportedResource();

	OutSectionIndices.Empty();

	check(Resource->LODModels.IsValidIndex(LODIndex));

	FStaticLODModel& LODModel = Resource->LODModels[LODIndex];
	int32 NumSections = LODModel.Sections.Num();

	for(int32 SecIdx=0; SecIdx < NumSections; SecIdx++)
	{
		if(LODModel.Chunks[LODModel.Sections[SecIdx].ChunkIndex].CorrespondClothAssetIndex == AssetIndex)
		{
			//add cloth sections
			OutSectionIndices.Add(SecIdx);
		}
	}
}

void USkeletalMesh::GetOriginSectionIndicesWithCloth(int32 LODIndex, TArray<uint32>& OutSectionIndices)
{
	FSkeletalMeshResource* Resource = GetImportedResource();

	OutSectionIndices.Empty();

	check(Resource->LODModels.IsValidIndex(LODIndex));

	FStaticLODModel& LODModel = Resource->LODModels[LODIndex];
	int32 NumSections = LODModel.Sections.Num();

	for(int32 SecIdx=0; SecIdx < NumSections; SecIdx++)
	{
		if(LODModel.Sections[SecIdx].CorrespondClothSectionIndex >= 0)
		{
			//add original sections
			OutSectionIndices.Add(SecIdx);
		}
	}
}

void USkeletalMesh::GetOriginSectionIndicesWithCloth(int32 LODIndex, int32 AssetIndex, TArray<uint32>& OutSectionIndices)
{
	FSkeletalMeshResource* Resource = GetImportedResource();

	OutSectionIndices.Empty();

	check(Resource->LODModels.IsValidIndex(LODIndex));

	FStaticLODModel& LODModel = Resource->LODModels[LODIndex];
	int32 NumSections = LODModel.Sections.Num();

	for(int32 SecIdx=0; SecIdx < NumSections; SecIdx++)
	{
		if(LODModel.Chunks[LODModel.Sections[SecIdx].ChunkIndex].CorrespondClothAssetIndex == AssetIndex)
		{
			//add original sections
			OutSectionIndices.Add(LODModel.Sections[SecIdx].CorrespondClothSectionIndex);
		}
	}
}

bool USkeletalMesh::IsMappedClothingLOD(int32 InLODIndex, int32 InAssetIndex)
{
	FSkeletalMeshResource* Resource = GetImportedResource();

	if (Resource && Resource->LODModels.IsValidIndex(InLODIndex))
	{
		FStaticLODModel& LODModel = Resource->LODModels[InLODIndex];

		int32 NumSections = LODModel.Sections.Num();

		// loop reversely for optimized search
		for (int32 SecIdx = NumSections-1; SecIdx >= 0; SecIdx--)
	{
			int32 ClothAssetIndex = LODModel.Chunks[LODModel.Sections[SecIdx].ChunkIndex].CorrespondClothAssetIndex;
			if (ClothAssetIndex == InAssetIndex)
		{
			return true;
		}
			else if (ClothAssetIndex == INDEX_NONE) // no more cloth sections
			{
				return false;
			}
		}
	}

	return false;
}

int32 USkeletalMesh::GetClothAssetIndex(int32 LODIndex, int32 SectionIndex)
{
	FSkeletalMeshResource* Resource = GetImportedResource();

	// no LODs
	if (!Resource || !Resource->LODModels.IsValidIndex(LODIndex))
	{
		return INDEX_NONE;
	}
	FStaticLODModel& LODModel = Resource->LODModels[LODIndex];

	//no sections
	if(!LODModel.Sections.IsValidIndex(SectionIndex))
	{
		return INDEX_NONE;
	}
	int16 ClothSecIdx = LODModel.Sections[SectionIndex].CorrespondClothSectionIndex;

	//no mapping
	if(ClothSecIdx < 0)
	{
		return INDEX_NONE;
	}

	int16 ChunkIdx = LODModel.Sections[ClothSecIdx].ChunkIndex;
	return LODModel.Chunks[ChunkIdx].CorrespondClothAssetIndex;
}
#endif//#if WITH_APEX_CLOTHING

void USkeletalMeshComponent::CreateBodySetup()
{
	if (BodySetup == NULL && SkeletalMesh)
	{
		BodySetup = NewObject<UBodySetup>(this);
	}

	UBodySetup* OriginalBodySetup = SkeletalMesh->GetBodySetup();
	
	BodySetup->CopyBodyPropertiesFrom(OriginalBodySetup);
	BodySetup->CollisionTraceFlag = CTF_UseComplexAsSimple;

	BodySetup->CookedFormatDataOverride = &OriginalBodySetup->CookedFormatData;

	//need to recreate meshes
	BodySetup->ClearPhysicsMeshes();
	BodySetup->CreatePhysicsMeshes();
}

//
//	USkeletalMeshComponent
//
UBodySetup* USkeletalMeshComponent::GetBodySetup()
{
	if (bEnablePerPolyCollision == false)
	{
		UPhysicsAsset * const PhysicsAsset = GetPhysicsAsset();
		if (SkeletalMesh && PhysicsAsset)
		{
			for (int32 i = 0; i < SkeletalMesh->RefSkeleton.GetNum(); i++)
			{
				int32 BodyIndex = PhysicsAsset->FindBodyIndex(SkeletalMesh->RefSkeleton.GetBoneName(i));
				if (BodyIndex != INDEX_NONE)
				{
					return PhysicsAsset->BodySetup[BodyIndex];
				}
			}
		}
	}
	else
	{
		if (BodySetup == NULL)
		{
			CreateBodySetup();
		}

		return BodySetup;
	}


	return NULL;
}

bool USkeletalMeshComponent::CanEditSimulatePhysics()
{
	return GetPhysicsAsset() != nullptr;
}

void USkeletalMeshComponent::SetSimulatePhysics(bool bSimulate)
{
	if ( !bEnablePhysicsOnDedicatedServer && IsRunningDedicatedServer() )
	{
		return;
	}

	BodyInstance.bSimulatePhysics = bSimulate;

	// enable blending physics
	bBlendPhysics = bSimulate;

	//Go through body setups and see which bodies should be turned on and off
	if (UPhysicsAsset * PhysAsset = GetPhysicsAsset())
	{
		for (int32 BodyIdx = 0; BodyIdx < Bodies.Num(); ++BodyIdx)
		{
			if (FBodyInstance* BodyInstance = Bodies[BodyIdx])
			{
				if (UBodySetup * PhysAssetBodySetup = PhysAsset->BodySetup[BodyIdx])
				{
					if (PhysAssetBodySetup->PhysicsType == EPhysicsType::PhysType_Default)
					{
						BodyInstance->SetInstanceSimulatePhysics(bSimulate);
					}
				}
			}
		}
	}

	UpdatePreClothTickRegisteredState();
}

void USkeletalMeshComponent::OnComponentCollisionSettingsChanged()
{
	for(int32 i=0; i<Bodies.Num(); i++)
	{
		Bodies[i]->UpdatePhysicsFilterData();
	}

	if (SceneProxy)
	{
		((FSkeletalMeshSceneProxy*)SceneProxy)->SetCollisionEnabled_GameThread(IsCollisionEnabled());
	}
}

void USkeletalMeshComponent::AddRadialImpulse(FVector Origin, float Radius, float Strength, ERadialImpulseFalloff Falloff, bool bVelChange)
{
	if(bIgnoreRadialImpulse)
	{
		return;
	}

	const float StrengthPerMass = Strength / FMath::Max(GetMass(), KINDA_SMALL_NUMBER);
	for(int32 i=0; i<Bodies.Num(); i++)
	{
		const float StrengthPerBody = bVelChange ? Strength : (StrengthPerMass * Bodies[i]->GetBodyMass());
		Bodies[i]->AddRadialImpulseToBody(Origin, Radius, StrengthPerBody, Falloff, bVelChange);
	}
}



void USkeletalMeshComponent::AddRadialForce(FVector Origin, float Radius, float Strength, ERadialImpulseFalloff Falloff, bool bAccelChange)
{
	if(bIgnoreRadialForce)
	{
		return;
	}

	const float StrengthPerMass = Strength / FMath::Max(GetMass(), KINDA_SMALL_NUMBER);
	for(int32 i=0; i<Bodies.Num(); i++)
	{
		const float StrengthPerBody = bAccelChange ? Strength : (StrengthPerMass * Bodies[i]->GetBodyMass());
		Bodies[i]->AddRadialForceToBody(Origin, Radius, StrengthPerBody, Falloff, bAccelChange);
	}

}

void USkeletalMeshComponent::WakeAllRigidBodies()
{
	for (int32 i=0; i < Bodies.Num(); i++)
	{
		FBodyInstance* BI = Bodies[i];
		check(BI);
		BI->WakeInstance();
	}
}

void USkeletalMeshComponent::PutAllRigidBodiesToSleep()
{
	for (int32 i=0; i < Bodies.Num(); i++)
	{
		FBodyInstance* BI = Bodies[i];
		check(BI);
		BI->PutInstanceToSleep();
	}
}


bool USkeletalMeshComponent::IsAnyRigidBodyAwake()
{
	bool bAwake = false;

	// ..iterate over each body to find any that are awak
	for(int32 i=0; i<Bodies.Num(); i++)
	{
		FBodyInstance* BI = Bodies[i];
		check(BI);
		if(BI->IsInstanceAwake())
		{
			// Found an awake one - so mesh is considered 'awake'
			bAwake = true;
			continue;
		}
	}

	return bAwake;
}


void USkeletalMeshComponent::SetAllPhysicsLinearVelocity(FVector NewVel, bool bAddToCurrent)
{
	for (int32 i=0; i < Bodies.Num(); i++)
	{
		FBodyInstance* BodyInstance = Bodies[i];
		check(BodyInstance);
		BodyInstance->SetLinearVelocity(NewVel, bAddToCurrent);
	}
}

void USkeletalMeshComponent::SetAllPhysicsAngularVelocity(FVector const& NewAngVel, bool bAddToCurrent)
{
	if(RootBodyData.BodyIndex < Bodies.Num())
	{
		// Find the root actor. We use its location as the center of the rotation.
		FBodyInstance* RootBodyInst = Bodies[ RootBodyData.BodyIndex ];
		check(RootBodyInst);
		FTransform RootTM = RootBodyInst->GetUnrealWorldTransform();

		FVector RootPos = RootTM.GetLocation();

		// Iterate over each bone, updating its velocity
		for (int32 i = 0; i < Bodies.Num(); i++)
		{
			FBodyInstance* const BI = Bodies[i];
			check(BI);

			BI->SetAngularVelocity(NewAngVel, bAddToCurrent);
		}
	}
}

void USkeletalMeshComponent::SetAllPhysicsPosition(FVector NewPos)
{
	if(RootBodyData.BodyIndex < Bodies.Num())
	{
		// calculate the deltas to get the root body to NewPos
		FBodyInstance* RootBI = Bodies[RootBodyData.BodyIndex];
		check(RootBI);
		if(RootBI->IsValidBodyInstance())
		{
			// move the root body
			FTransform RootBodyTM = RootBI->GetUnrealWorldTransform();
			FVector DeltaLoc = NewPos - RootBodyTM.GetLocation();
			RootBodyTM.SetTranslation(NewPos);
			RootBI->SetBodyTransform(RootBodyTM, ETeleportType::TeleportPhysics);

#if DO_CHECK
			FVector RelativeVector = (RootBI->GetUnrealWorldTransform().GetLocation() - NewPos);
			check(RelativeVector.SizeSquared() < 1.f);
#endif

			// apply the delta to all the other bodies
			for (int32 i = 0; i < Bodies.Num(); i++)
			{
				if (i != RootBodyData.BodyIndex)
				{
					FBodyInstance* BI = Bodies[i];
					check(BI);

					FTransform BodyTM = BI->GetUnrealWorldTransform();
					BodyTM.SetTranslation(BodyTM.GetTranslation() + DeltaLoc);
					BI->SetBodyTransform(BodyTM, ETeleportType::TeleportPhysics);
				}
			}

			// Move component to new physics location
			SyncComponentToRBPhysics();
		}
	}
}

void USkeletalMeshComponent::SetAllPhysicsRotation(FRotator NewRot)
{
	if(RootBodyData.BodyIndex < Bodies.Num())
	{
		// calculate the deltas to get the root body to NewRot
		FBodyInstance* RootBI = Bodies[RootBodyData.BodyIndex];
		check(RootBI);
		if(RootBI->IsValidBodyInstance())
		{
			// move the root body
			FQuat NewRotQuat = NewRot.Quaternion();
			FTransform RootBodyTM = RootBI->GetUnrealWorldTransform();
			FQuat DeltaQuat = RootBodyTM.GetRotation().Inverse() * NewRotQuat;
			RootBodyTM.SetRotation(NewRotQuat);
			RootBI->SetBodyTransform(RootBodyTM, ETeleportType::TeleportPhysics);

			// apply the delta to all the other bodies
			for (int32 i = 0; i < Bodies.Num(); i++)
			{
				if (i != RootBodyData.BodyIndex)
				{
					FBodyInstance* BI = Bodies[i];
					check(BI);

					FTransform BodyTM = BI->GetUnrealWorldTransform();
					BodyTM.SetRotation(BodyTM.GetRotation() * DeltaQuat);
					BI->SetBodyTransform( BodyTM, ETeleportType::TeleportPhysics );
				}
			}

			// Move component to new physics location
			SyncComponentToRBPhysics();
		}
	}
}

void USkeletalMeshComponent::ApplyDeltaToAllPhysicsTransforms(const FVector& DeltaLocation, const FQuat& DeltaRotation)
{
	if(RootBodyData.BodyIndex < Bodies.Num())
	{
		// calculate the deltas to get the root body to NewRot
		FBodyInstance* RootBI = Bodies[RootBodyData.BodyIndex];
		check(RootBI);
		if(RootBI->IsValidBodyInstance())
		{
			// move the root body
			FTransform RootBodyTM = RootBI->GetUnrealWorldTransform();
			RootBodyTM.SetRotation(RootBodyTM.GetRotation() * DeltaRotation);
			RootBodyTM.SetTranslation(RootBodyTM.GetTranslation() + DeltaLocation);
			RootBI->SetBodyTransform(RootBodyTM, ETeleportType::TeleportPhysics);

			// apply the delta to all the other bodies
			for (int32 i = 0; i < Bodies.Num(); i++)
			{
				if (i != RootBodyData.BodyIndex)
				{
					FBodyInstance* BI = Bodies[i];
					check(BI);

					FTransform BodyTM = BI->GetUnrealWorldTransform();
					BodyTM.SetRotation(BodyTM.GetRotation() * DeltaRotation);
					BodyTM.SetTranslation(BodyTM.GetTranslation() + DeltaLocation);
					BI->SetBodyTransform( BodyTM, ETeleportType::TeleportPhysics );
				}
			}

			// Move component to new physics location
			SyncComponentToRBPhysics();
		}
	}
}

void USkeletalMeshComponent::SetPhysMaterialOverride(UPhysicalMaterial* NewPhysMaterial)
{
	// Single-body case - just use PrimComp code.
	UPrimitiveComponent::SetPhysMaterialOverride(NewPhysMaterial);

	// Now update any child bodies
	for( int32 i = 0; i < Bodies.Num(); i++ )
	{
		FBodyInstance* BI = Bodies[i];
		BI->UpdatePhysicalMaterials();
	}
}

DEFINE_STAT(STAT_InitArticulated);

void USkeletalMeshComponent::InitArticulated(FPhysScene* PhysScene)
{
	SCOPE_CYCLE_COUNTER(STAT_InitArticulated);

	UPhysicsAsset* const PhysicsAsset = GetPhysicsAsset();

	if(PhysScene == NULL || PhysicsAsset == NULL || SkeletalMesh == NULL)
	{
		return;
	}

	if(Bodies.Num() > 0)
	{
		UE_LOG(LogSkeletalMesh, Log, TEXT("InitArticulated: Bodies already created (%s) - call TermArticulated first."), *GetPathName());
		return;
	}

	FVector Scale3D = ComponentToWorld.GetScale3D();
	float Scale = Scale3D.X;

	// Find root physics body
	int32 RootBodyIndex = INDEX_NONE;
	for(int32 i=0; i<SkeletalMesh->RefSkeleton.GetNum(); i++)
	{
		int32 BodyInstIndex = PhysicsAsset->FindBodyIndex( SkeletalMesh->RefSkeleton.GetBoneName(i) );
		if(BodyInstIndex != INDEX_NONE)
		{
			RootBodyIndex = BodyInstIndex;
			break;
		}
	}

	if(RootBodyIndex == INDEX_NONE)
	{
		UE_LOG(LogSkeletalMesh, Log, TEXT("UPhysicsAssetInstance::InitInstance : Could not find root physics body: %s"), *GetName() );
		return;
	}

	// Set up the map from skelmeshcomp ID to collision disable table
#if WITH_PHYSX
	uint32 SkelMeshCompID = GetUniqueID();
	PhysScene->DeferredAddCollisionDisableTable(SkelMeshCompID, &PhysicsAsset->CollisionDisableTable);

	int32 NumBodies = PhysicsAsset->BodySetup.Num();
	if(Aggregate == NULL && NumBodies > RagdollAggregateThreshold && NumBodies <= AggregateMaxSize)
	{
		Aggregate = GPhysXSDK->createAggregate(PhysicsAsset->BodySetup.Num(), true);
	}
	else if(Aggregate && NumBodies > AggregateMaxSize)
	{
		UE_LOG(LogSkeletalMesh, Log, TEXT("USkeletalMeshComponent::InitArticulated : Too many bodies to create aggregate, Max: %u, This: %d"), AggregateMaxSize, NumBodies);
	}
#endif //WITH_PHYSX

	// Create all the bodies.
	check(Bodies.Num() == 0);
	Bodies.AddZeroed(NumBodies);
	for(int32 i=0; i<NumBodies; i++)
	{
		UBodySetup* PhysicsAssetBodySetup = PhysicsAsset->BodySetup[i];
		Bodies[i] = new FBodyInstance;
		FBodyInstance* BodyInst = Bodies[i];
		check(BodyInst);

		// Get transform of bone by name.
		int32 BoneIndex = GetBoneIndex( PhysicsAssetBodySetup->BoneName );
		if(BoneIndex != INDEX_NONE)
		{
			// Copy body setup default instance properties
			BodyInst->CopyBodyInstancePropertiesFrom(&PhysicsAssetBodySetup->DefaultInstance);
			// we don't allow them to use this in editor. For physics asset, this set up is overriden by Physics Type. 
			// but before we hide in the detail customization, we saved with this being true, causing the simulate always happens for some bodies
			// so adding initialization here to disable this. 
			// to check, please check BodySetupDetails.cpp, if (ChildProperty->GetProperty()->GetName() == TEXT("bSimulatePhysics"))
			// we hide this property, so it should always be false initially. 
			// this is not true for all other BodyInstance, but for physics assets it is true. 
			BodyInst->bSimulatePhysics = false;
			BodyInst->InstanceBodyIndex = i; // Set body index 
			BodyInst->InstanceBoneIndex = BoneIndex; // Set bone index

			if (i == RootBodyIndex)
			{
				BodyInst->DOFMode = BodyInstance.DOFMode;
				BodyInst->CustomDOFPlaneNormal = BodyInstance.CustomDOFPlaneNormal;
				BodyInst->bLockXTranslation = BodyInstance.bLockXTranslation;
				BodyInst->bLockYTranslation = BodyInstance.bLockYTranslation;
				BodyInst->bLockZTranslation = BodyInstance.bLockZTranslation;
				BodyInst->bLockXRotation = BodyInstance.bLockXRotation;
				BodyInst->bLockYRotation = BodyInstance.bLockYRotation;
				BodyInst->bLockZRotation = BodyInstance.bLockZRotation;
				BodyInst->bLockTranslation = BodyInstance.bLockTranslation;
				BodyInst->bLockRotation = BodyInstance.bLockRotation;

				BodyInst->COMNudge = BodyInstance.COMNudge;
			}
			else
			{
				BodyInst->DOFMode = EDOFMode::None;
			}

#if WITH_PHYSX
			// Create physics body instance.
			FTransform BoneTransform = GetBoneTransform( BoneIndex );
			BodyInst->InitBody( PhysicsAssetBodySetup, BoneTransform, this, PhysScene, Aggregate);
#endif //WITH_PHYSX

			// Remember if we have bodies in sync/async scene, so we know which scene(s) to lock when moving bodies
			if(BodyInst->UseAsyncScene(PhysScene))
			{
				bHasBodiesInAsyncScene = true;
			}
			else
			{
				bHasBodiesInSyncScene = true;
			}
		}
	}

	// now update root body index because body has BodySetup now
	SetRootBodyIndex(RootBodyIndex);

#if WITH_PHYSX

	if (PhysScene)
	{
		// Get the scene type from the SkeletalMeshComponent's BodyInstance
		const uint32 SceneType = (bHasBodiesInAsyncScene && PhysScene->HasAsyncScene()) ? PST_Async : PST_Sync;
		PxScene* PScene = PhysScene->GetPhysXScene(SceneType);
		SCOPED_SCENE_WRITE_LOCK(PScene);
		// add Aggregate into the scene
		if (Aggregate && Aggregate->getNbActors() > 0)
		{
			PScene->addAggregate(*Aggregate);

			// If we've used an aggregate, InitBody would not be able to set awake status as we *must* have a scene
			// to do that, so we reconcile this here.
			AActor* Owner = GetOwner();
			bool bShouldSleep = !BodyInstance.bStartAwake && (Owner && Owner->GetVelocity().SizeSquared() <= KINDA_SMALL_NUMBER);

			for (FBodyInstance* Body : Bodies)
			{
				// Creates a DOF constraint if necessary for the body - also requires the scene to exist within the actor
				Body->CreateDOFLock();

				// Set to sleep if necessary
				if (bShouldSleep)
				{
					Body->GetPxRigidDynamic_AssumesLocked()->putToSleep();
				}
			}
		}
	}

#endif //WITH_PHYSX

	// Create all the constraints.
	check(Constraints.Num() == 0);
	int32 NumConstraints = PhysicsAsset->ConstraintSetup.Num();
	Constraints.AddZeroed(NumConstraints);
	for(int32 i=0; i<NumConstraints; i++)
	{
		UPhysicsConstraintTemplate* ConstraintSetup = PhysicsAsset->ConstraintSetup[i]; 
		Constraints[i] = new FConstraintInstance;
		FConstraintInstance* ConInst = Constraints[i];
		check( ConInst );
		ConInst->ConstraintIndex = i; // Set the ConstraintIndex property in the ConstraintInstance.
		ConInst->CopyConstraintParamsFrom(&ConstraintSetup->DefaultInstance);

		// Get bodies we want to joint
		FBodyInstance* Body1 = GetBodyInstance(ConInst->ConstraintBone1);
		FBodyInstance* Body2 = GetBodyInstance(ConInst->ConstraintBone2);

		// If we have 2, joint 'em
		if(Body1 != NULL && Body2 != NULL)
		{
			ConInst->InitConstraint(this, Body1, Body2, Scale);
		}
	}

	// Update Flag
	ResetAllBodiesSimulatePhysics();
#if WITH_APEX_CLOTHING
	PrevRootBoneMatrix = GetBoneMatrix(0); // save the root bone transform
	// pre-compute cloth teleport thresholds for performance
	ClothTeleportCosineThresholdInRad = FMath::Cos(FMath::DegreesToRadians(TeleportRotationThreshold));
	ClothTeleportDistThresholdSquared = TeleportDistanceThreshold * TeleportDistanceThreshold;
#endif // #if WITH_APEX_CLOTHING
}


void USkeletalMeshComponent::TermArticulated()
{
#if WITH_PHYSX
	uint32 SkelMeshCompID = GetUniqueID();
	FPhysScene * PhysScene = GetWorld()->GetPhysicsScene();
	if (PhysScene)
	{
		PhysScene->DeferredRemoveCollisionDisableTable(SkelMeshCompID);
	}

	// Get the scene type from the SkeletalMeshComponent's BodyInstance
	const uint32 SceneType = BodyInstance.UseAsyncScene(PhysScene) ? PST_Async : PST_Sync;
	PxScene* PScene = PhysScene->GetPhysXScene(SceneType);
	SCOPED_SCENE_WRITE_LOCK(PScene);

#endif	//#if WITH_PHYSX

	// We shut down the physics for each body and constraint here. 
	// The actual UObjects will get GC'd

	for(int32 i=0; i<Constraints.Num(); i++)
	{
		check( Constraints[i] );
		Constraints[i]->TermConstraint();
		delete Constraints[i];
	}

	Constraints.Empty();

	for(int32 i=0; i<Bodies.Num(); i++)
	{
		check( Bodies[i] );
		Bodies[i]->TermBody();
		delete Bodies[i];
	}
	
	Bodies.Empty();

#if WITH_PHYSX
	// releasing Aggregate, it shouldn't contain any Bodies now, because they are released above
	if(Aggregate)
	{
		check(!Aggregate->getNbActors());
		Aggregate->release();
		Aggregate = NULL;
	}
#endif //WITH_PHYSX

	// Reset bools for scenes
	bHasBodiesInAsyncScene = false;
	bHasBodiesInSyncScene = false;
}

void USkeletalMeshComponent::TermBodiesBelow(FName ParentBoneName)
{
	UPhysicsAsset* const PhysicsAsset = GetPhysicsAsset();
	if(PhysicsAsset && SkeletalMesh && Bodies.Num() > 0)
	{
		check(Bodies.Num() == PhysicsAsset->BodySetup.Num());

		// Get index of parent bone
		int32 ParentBoneIndex = GetBoneIndex(ParentBoneName);
		if(ParentBoneIndex == INDEX_NONE)
		{
			UE_LOG(LogSkeletalMesh, Log, TEXT("TermBodiesBelow: ParentBoneName '%s' is invalid"), *ParentBoneName.ToString());
			return;
		}

		// First terminate any constraints at below this bone
		for(int32 i=0; i<Constraints.Num(); i++)
		{
			// Get bone index of constraint
			FName JointName = Constraints[i]->JointName;
			int32 JointBoneIndex = GetBoneIndex(JointName);

			// If constraint has bone in mesh, and is either the parent or child of it, term it
			if(	JointBoneIndex != INDEX_NONE && (JointName == ParentBoneName ||	SkeletalMesh->RefSkeleton.BoneIsChildOf(JointBoneIndex, ParentBoneIndex)) )
			{
				Constraints[i]->TermConstraint();
			}
		}

		// Then iterate over bodies looking for any which are children of supplied parent
		for(int32 i=0; i<Bodies.Num(); i++)
		{
			// Get bone index of body
			if (Bodies[i]->IsValidBodyInstance())
			{
				FName BodyName = Bodies[i]->BodySetup->BoneName;
				int32 BodyBoneIndex = GetBoneIndex(BodyName);

				// If body has bone in mesh, and is either the parent or child of it, term it
				if(	BodyBoneIndex != INDEX_NONE && (BodyName == ParentBoneName ||	SkeletalMesh->RefSkeleton.BoneIsChildOf(BodyBoneIndex, ParentBoneIndex)) )
				{
					Bodies[i]->TermBody();
				}
			}
		}
	}
}

float USkeletalMeshComponent::GetTotalMassBelowBone(FName InBoneName)
{
	UPhysicsAsset* const PhysicsAsset = GetPhysicsAsset();
	if(!PhysicsAsset || !SkeletalMesh)
	{
		return 0.f;
	}

	// if physics state is invalid - i.e. collision is disabled - or it does not have a valid bodies, this will crash right away
	if (!IsPhysicsStateCreated()  || !bHasValidBodies)
	{
		return 0.f;
	}

	TArray<int32> BodyIndices;
	PhysicsAsset->GetBodyIndicesBelow(BodyIndices, InBoneName, SkeletalMesh);

	float TotalMass = 0.f;
	for(int32 i=0; i<BodyIndices.Num(); i++)
	{
		TotalMass += Bodies[BodyIndices[i]]->GetBodyMass();
	}

	return TotalMass;
}

void USkeletalMeshComponent::SetAllBodiesSimulatePhysics(bool bNewSimulate)
{
	for(int32 i=0; i<Bodies.Num(); i++)
	{
		Bodies[i]->SetInstanceSimulatePhysics(bNewSimulate);
	}

	UpdatePreClothTickRegisteredState();
}


void USkeletalMeshComponent::SetAllBodiesCollisionObjectType(ECollisionChannel NewChannel)
{
	for(int32 i=0; i<Bodies.Num(); i++)
	{
		Bodies[i]->SetObjectType(NewChannel);
	}
}

void USkeletalMeshComponent::SetAllBodiesNotifyRigidBodyCollision(bool bNewNotifyRigidBodyCollision)
{
	for(int32 i=0; i<Bodies.Num(); i++)
	{
		Bodies[i]->SetInstanceNotifyRBCollision(bNewNotifyRigidBodyCollision);
	}
}


void USkeletalMeshComponent::SetAllBodiesBelowSimulatePhysics( const FName& InBoneName, bool bNewSimulate  )
{
	UPhysicsAsset* const PhysicsAsset = GetPhysicsAsset();
	if( !PhysicsAsset || !SkeletalMesh )
	{
		return;
	}

	// if physics state is invalid - i.e. collision is disabled - or it does not have a valid bodies, this will crash right away
	if (!IsPhysicsStateCreated()  || !bHasValidBodies)
	{
		FMessageLog("PIE").Warning(LOCTEXT("InvalidBodies", "Invalid Bodies : Make sure collision is enabled or root bone has body in PhysicsAsset."));
		return;
	}

	TArray<int32> BodyIndices;
	PhysicsAsset->GetBodyIndicesBelow(BodyIndices, InBoneName, SkeletalMesh);

	for(int32 i=0; i<BodyIndices.Num(); i++)
	{
		//UE_LOG(LogSkeletalMesh, Warning, TEXT( "ForceAllBodiesBelowUnfixed %s" ), *InAsset->BodySetup(BodyIndices(i))->BoneName.ToString() );
		Bodies[BodyIndices[i]]->SetInstanceSimulatePhysics(bNewSimulate);
	}

	UpdatePreClothTickRegisteredState();
}


void USkeletalMeshComponent::SetAllMotorsAngularPositionDrive(bool bEnableSwingDrive, bool bEnableTwistDrive, bool bSkipCustomPhysicsType)
{
	UPhysicsAsset* const PhysicsAsset = GetPhysicsAsset();
	if( !PhysicsAsset )
	{
		return;
	}

	for(int32 i=0; i<Constraints.Num(); i++)
	{
		if( bSkipCustomPhysicsType )
		{
			int32 BodyIndex = PhysicsAsset->FindBodyIndex(Constraints[i]->JointName);
			if( BodyIndex != INDEX_NONE && PhysicsAsset->BodySetup[BodyIndex]->PhysicsType != PhysType_Default)
			{
				continue;
			}
		}

		Constraints[i]->SetAngularPositionDrive(bEnableSwingDrive, bEnableTwistDrive);
	}
}

void USkeletalMeshComponent::SetNamedMotorsAngularPositionDrive(bool bEnableSwingDrive, bool bEnableTwistDrive, const TArray<FName>& BoneNames, bool bSetOtherBodiesToComplement)
{
	UPhysicsAsset* const PhysicsAsset = GetPhysicsAsset();
	if( !PhysicsAsset )
	{
		return;
	}

	for(int32 i=0; i<Constraints.Num(); i++)
	{
		FConstraintInstance* Instance = Constraints[i];
		if( BoneNames.Contains(Instance->JointName) )
		{
			Constraints[i]->SetAngularPositionDrive(bEnableSwingDrive, bEnableTwistDrive);
		}
		else if( bSetOtherBodiesToComplement )
		{
			Constraints[i]->SetAngularPositionDrive(!bEnableSwingDrive, !bEnableTwistDrive);
		}
	}
}

void USkeletalMeshComponent::SetNamedMotorsAngularVelocityDrive(bool bEnableSwingDrive, bool bEnableTwistDrive, const TArray<FName>& BoneNames, bool bSetOtherBodiesToComplement)
{
	UPhysicsAsset* const PhysicsAsset = GetPhysicsAsset();
	if( !PhysicsAsset )
	{
		return;
	}

	for(int32 i=0; i<Constraints.Num(); i++)
	{
		FConstraintInstance* Instance = Constraints[i];
		if( BoneNames.Contains(Instance->JointName) )
		{
			Constraints[i]->SetAngularVelocityDrive(bEnableSwingDrive, bEnableTwistDrive);
		}
		else if( bSetOtherBodiesToComplement )
		{
			Constraints[i]->SetAngularVelocityDrive(!bEnableSwingDrive, !bEnableTwistDrive);
		}
	}
}

void USkeletalMeshComponent::SetAllMotorsAngularVelocityDrive(bool bEnableSwingDrive, bool bEnableTwistDrive, bool bSkipCustomPhysicsType)
{
	UPhysicsAsset* const PhysicsAsset = GetPhysicsAsset();
	if( !PhysicsAsset )
	{
		return;
	}

	for(int32 i=0; i<Constraints.Num(); i++)
	{
		if( bSkipCustomPhysicsType )
		{
			int32 BodyIndex = PhysicsAsset->FindBodyIndex(Constraints[i]->JointName);
			if( BodyIndex != INDEX_NONE && PhysicsAsset->BodySetup[BodyIndex]->PhysicsType != PhysType_Default )
			{
				continue;
			}
		}

		Constraints[i]->SetAngularVelocityDrive(bEnableSwingDrive, bEnableTwistDrive);
	}
}


void USkeletalMeshComponent::SetAllMotorsAngularDriveParams(float InSpring, float InDamping, float InForceLimit, bool bSkipCustomPhysicsType)
{
	UPhysicsAsset* const PhysicsAsset = GetPhysicsAsset();
	if( !PhysicsAsset )
	{
		return;
	}

	for(int32 i=0; i<Constraints.Num(); i++)
	{
		if( bSkipCustomPhysicsType )
		{
			int32 BodyIndex = PhysicsAsset->FindBodyIndex(Constraints[i]->JointName);
			if( BodyIndex != INDEX_NONE && PhysicsAsset->BodySetup[BodyIndex]->PhysicsType != PhysType_Default )
			{
				continue;
			}
		}
		Constraints[i]->SetAngularDriveParams(InSpring, InDamping, InForceLimit);
	}
}

void USkeletalMeshComponent::ResetAllBodiesSimulatePhysics()
{
	if ( !bEnablePhysicsOnDedicatedServer && IsRunningDedicatedServer() )
	{
		return;
	}

	UPhysicsAsset* const PhysicsAsset = GetPhysicsAsset();
	if( !PhysicsAsset )
	{
		return;
	}

	// Fix / Unfix bones
	for(int32 i=0; i<Bodies.Num(); i++)
	{
		FBodyInstance*	BodyInst	= Bodies[i];
		UBodySetup*	BodyInstSetup	= BodyInst->BodySetup.Get();

		// Set fixed on any bodies with bAlwaysFullAnimWeight set to true
		if(BodyInstSetup && BodyInstSetup->PhysicsType != PhysType_Default)
		{
			if (BodyInstSetup->PhysicsType == PhysType_Simulated)
			{
				BodyInst->SetInstanceSimulatePhysics(true);
			}
			else
			{
				BodyInst->SetInstanceSimulatePhysics(false);
			}
		}
	}
}

void USkeletalMeshComponent::SetEnablePhysicsBlending(bool bNewBlendPhysics)
{
	bBlendPhysics = bNewBlendPhysics;
}

void USkeletalMeshComponent::SetPhysicsBlendWeight(float PhysicsBlendWeight)
{
	bool bShouldSimulate = PhysicsBlendWeight > 0.f;
	if (bShouldSimulate != IsSimulatingPhysics())
	{
		SetSimulatePhysics(bShouldSimulate);
	}

	// if blend weight is not 1, set manual weight
	if ( PhysicsBlendWeight < 1.f )
	{
		bBlendPhysics = false;
		SetAllBodiesPhysicsBlendWeight (PhysicsBlendWeight, true);
	}
}

void USkeletalMeshComponent::SetAllBodiesPhysicsBlendWeight(float PhysicsBlendWeight, bool bSkipCustomPhysicsType )
{
	UPhysicsAsset* const PhysicsAsset = GetPhysicsAsset();
	if( !PhysicsAsset )
	{
		return;
	}

	// Fix / Unfix bones
	for(int32 i=0; i<Bodies.Num(); i++)
	{
		FBodyInstance*	BodyInst	= Bodies[i];
		UBodySetup*	BodyInstSetup	= BodyInst->BodySetup.Get();

		// Set fixed on any bodies with bAlwaysFullAnimWeight set to true
		if(BodyInstSetup && (!bSkipCustomPhysicsType || BodyInstSetup->PhysicsType == PhysType_Default) )
		{
			BodyInst->PhysicsBlendWeight = PhysicsBlendWeight;
		}
	}
}


void USkeletalMeshComponent::SetAllBodiesBelowPhysicsBlendWeight( const FName& InBoneName, float PhysicsBlendWeight, bool bSkipCustomPhysicsType )
{
	UPhysicsAsset* const PhysicsAsset = GetPhysicsAsset();
	if( !PhysicsAsset || !SkeletalMesh )
	{
		return;
	}

	// if physics state is invalid - i.e. collision is disabled - or it does not have a valid bodies, this will crash right away
	if (!IsPhysicsStateCreated()  || !bHasValidBodies)
	{
		FMessageLog("PIE").Warning(LOCTEXT("InvalidBodies", "Invalid Bodies : Make sure collision is enabled or root bone has body in PhysicsAsset."));
		return;
	}

	TArray<int32> BodyIndices;
	PhysicsAsset->GetBodyIndicesBelow(BodyIndices, InBoneName, SkeletalMesh);

	for(int32 i=0; i<BodyIndices.Num(); i++)
	{
		Bodies[BodyIndices[i]]->PhysicsBlendWeight = PhysicsBlendWeight;
	}
}


void USkeletalMeshComponent::AccumulateAllBodiesBelowPhysicsBlendWeight( const FName& InBoneName, float PhysicsBlendWeight, bool bSkipCustomPhysicsType )
{
	UPhysicsAsset* const PhysicsAsset = GetPhysicsAsset();
	if( !PhysicsAsset || !SkeletalMesh )
	{
		return;
	}

	// if physics state is invalid - i.e. collision is disabled - or it does not have a valid bodies, this will crash right away
	if (!IsPhysicsStateCreated()  || !bHasValidBodies)
	{
		FMessageLog("PIE").Warning(LOCTEXT("InvalidBodies", "Invalid Bodies : Make sure collision is enabled or root bone has body in PhysicsAsset."));
		return;
	}

	TArray<int32> BodyIndices;
	PhysicsAsset->GetBodyIndicesBelow(BodyIndices, InBoneName, SkeletalMesh);

	for(int32 i=0; i<BodyIndices.Num(); i++)
	{
		Bodies[BodyIndices[i]]->PhysicsBlendWeight = FMath::Min(Bodies[BodyIndices[i]]->PhysicsBlendWeight + PhysicsBlendWeight, 1.f);
	}
}

FConstraintInstance* USkeletalMeshComponent::FindConstraintInstance(FName ConName)
{
	UPhysicsAsset* const PhysicsAsset = GetPhysicsAsset();
	if(PhysicsAsset && PhysicsAsset->ConstraintSetup.Num() == Constraints.Num())
	{
		int32 ConIndex = PhysicsAsset->FindConstraintIndex(ConName);
		if(ConIndex != INDEX_NONE)
		{
			return Constraints[ConIndex];
		}
	}

	return NULL;
}

#ifndef OLD_FORCE_UPDATE_BEHAVIOR
#define OLD_FORCE_UPDATE_BEHAVIOR 0
#endif

void USkeletalMeshComponent::OnUpdateTransform(bool bSkipPhysicsMove, ETeleportType Teleport)
{
	// We are handling the physics move below, so don't handle it at higher levels
	Super::OnUpdateTransform(true, Teleport);

	// Always send new transform to physics
	if(bPhysicsStateCreated && !bSkipPhysicsMove)
	{
#if !OLD_FORCE_UPDATE_BEHAVIOR
		UpdateKinematicBonesToAnim(GetSpaceBases(), Teleport, false);
#else
		UpdateKinematicBonesToAnim(GetSpaceBases(), ETeleportType::TeleportPhysics, false);
#endif
	}

#if WITH_APEX_CLOTHING
	if(ClothingActors.Num() > 0)
	{
		//@todo: Should cloth know whether we're teleporting?
		// Updates cloth animation states because transform is updated
		UpdateClothTransform();
	}
#endif //#if WITH_APEX_CLOTHING
}

void USkeletalMeshComponent::UpdateOverlaps(TArray<FOverlapInfo> const* PendingOverlaps, bool bDoNotifies, const TArray<FOverlapInfo>* OverlapsAtEndLocation)
{
	// Parent class (USkinnedMeshComponent) routes only to children, but we really do want to test our own bodies for overlaps.
	UPrimitiveComponent::UpdateOverlaps(PendingOverlaps, bDoNotifies, OverlapsAtEndLocation);
}

void USkeletalMeshComponent::CreatePhysicsState()
{
	//	UE_LOG(LogSkeletalMesh, Warning, TEXT("Creating Physics State (%s : %s)"), *GetNameSafe(GetOuter()),  *GetName());
	// Init physics
	if (bEnablePerPolyCollision == false)
	{
		InitArticulated(World->GetPhysicsScene());
		USceneComponent::CreatePhysicsState(); // Need to route CreatePhysicsState, skip PrimitiveComponent
	}
	else
	{
		CreateBodySetup();
		BodySetup->CreatePhysicsMeshes();
		Super::CreatePhysicsState();	//If we're doing per poly we'll use the body instance of the primitive component
	}
}


void USkeletalMeshComponent::DestroyPhysicsState()
{
	if (bEnablePerPolyCollision == false)
	{
		UnWeldFromParent();
		UnWeldChildren();
		TermArticulated();
	}

	Super::DestroyPhysicsState();
}



#if 0 && !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
#define DEBUGBROKENCONSTRAINTUPDATE(x) { ##x }
#else
#define DEBUGBROKENCONSTRAINTUPDATE(x)
#endif


void USkeletalMeshComponent::UpdateMeshForBrokenConstraints()
{
	UPhysicsAsset * const PhysicsAsset = GetPhysicsAsset();
	// Needs to have a SkeletalMesh, and PhysicsAsset.
	if( !SkeletalMesh || !PhysicsAsset )
	{
		return;
	}

	DEBUGBROKENCONSTRAINTUPDATE(UE_LOG(LogSkeletalMesh, Log, TEXT("%3.3f UpdateMeshForBrokenConstraints"), GetWorld()->GetTimeSeconds());)

	// Iterate through list of constraints in the physics asset
	for(int32 ConstraintInstIndex = 0; ConstraintInstIndex < Constraints.Num(); ConstraintInstIndex++)
	{
		// See if we can find a constraint that has been terminated (broken)
		FConstraintInstance* ConstraintInst = Constraints[ConstraintInstIndex];
		if( ConstraintInst && ConstraintInst->IsTerminated() )
		{
			// Get the associated joint bone index.
			int32 JointBoneIndex = GetBoneIndex(ConstraintInst->JointName);
			if( JointBoneIndex == INDEX_NONE )
			{
				continue;
			}

			DEBUGBROKENCONSTRAINTUPDATE(UE_LOG(LogSkeletalMesh, Log, TEXT("  Found Broken Constraint: (%d) %s"), JointBoneIndex, *PhysicsAsset->ConstraintSetup(ConstraintInstIndex)->JointName.ToString());)

			// Get child bodies of this joint
			for(int32 BodySetupIndex = 0; BodySetupIndex < PhysicsAsset->BodySetup.Num(); BodySetupIndex++)
			{
				UBodySetup* PhysicsAssetBodySetup = PhysicsAsset->BodySetup[BodySetupIndex];
				int32 BoneIndex = GetBoneIndex(PhysicsAssetBodySetup->BoneName);
				if( BoneIndex != INDEX_NONE && 
					(BoneIndex == JointBoneIndex || SkeletalMesh->RefSkeleton.BoneIsChildOf(BoneIndex, JointBoneIndex)) )
				{
					DEBUGBROKENCONSTRAINTUPDATE(UE_LOG(LogSkeletalMesh, Log, TEXT("    Found Child Bone: (%d) %s"), BoneIndex, *PhysicsAssetBodySetup->BoneName.ToString());)

					FBodyInstance* ChildBodyInst = Bodies[BodySetupIndex];
					if( ChildBodyInst )
					{
						// Unfix Body so, it is purely physical, not kinematic.
						if( !ChildBodyInst->IsInstanceSimulatingPhysics() )
						{
							DEBUGBROKENCONSTRAINTUPDATE(UE_LOG(LogSkeletalMesh, Log, TEXT("      Unfixing body."));)
							ChildBodyInst->SetInstanceSimulatePhysics(true);
						}
					}

					FConstraintInstance* ChildConstraintInst = FindConstraintInstance(PhysicsAssetBodySetup->BoneName);
					if( ChildConstraintInst )
					{
						if( ChildConstraintInst->bLinearPositionDrive )
						{
							DEBUGBROKENCONSTRAINTUPDATE(UE_LOG(LogSkeletalMesh, Log, TEXT("      Turning off LinearPositionDrive."));)
							ChildConstraintInst->SetLinearPositionDrive(false, false, false);
						}
						if( ChildConstraintInst->bLinearVelocityDrive )
						{
							DEBUGBROKENCONSTRAINTUPDATE(UE_LOG(LogSkeletalMesh, Log, TEXT("      Turning off LinearVelocityDrive."));)
							ChildConstraintInst->SetLinearVelocityDrive(false, false, false);
						}
						if( ChildConstraintInst->bAngularOrientationDrive )
						{
							DEBUGBROKENCONSTRAINTUPDATE(UE_LOG(LogSkeletalMesh, Log, TEXT("      Turning off AngularPositionDrive."));)
							ChildConstraintInst->SetAngularPositionDrive(false, false);
						}
						if( ChildConstraintInst->bAngularVelocityDrive )
						{
							DEBUGBROKENCONSTRAINTUPDATE(UE_LOG(LogSkeletalMesh, Log, TEXT("      Turning off AngularVelocityDrive."));)
							ChildConstraintInst->SetAngularVelocityDrive(false, false);
						}
					}
				}
			}
		}
	}
}


int32 USkeletalMeshComponent::FindConstraintIndex( FName ConstraintName )
{
	UPhysicsAsset * const PhysicsAsset = GetPhysicsAsset();
	return PhysicsAsset ? PhysicsAsset->FindConstraintIndex(ConstraintName) : INDEX_NONE;
}


FName USkeletalMeshComponent::FindConstraintBoneName( int32 ConstraintIndex )
{
	UPhysicsAsset * const PhysicsAsset = GetPhysicsAsset();
	return PhysicsAsset ? PhysicsAsset->FindConstraintBoneName(ConstraintIndex) : NAME_None;
}


FBodyInstance* USkeletalMeshComponent::GetBodyInstance(FName BoneName, bool) const
{
	UPhysicsAsset * const PhysicsAsset = GetPhysicsAsset();
	FBodyInstance* BodyInst = NULL;

	if(PhysicsAsset != NULL)
	{
		// A name of NAME_None indicates 'root body'
		if(BoneName == NAME_None)
		{
			if(Bodies.IsValidIndex(RootBodyData.BodyIndex))
			{
				BodyInst = Bodies[RootBodyData.BodyIndex];
			}
		}
		// otherwise, look for the body
		else
		{
			int32 BodyIndex = PhysicsAsset->FindBodyIndex(BoneName);
			if(Bodies.IsValidIndex(BodyIndex))
			{
				BodyInst = Bodies[BodyIndex];
			}
		}

	}

	return BodyInst;
}

void USkeletalMeshComponent::GetWeldedBodies(TArray<FBodyInstance*> & OutWeldedBodies, TArray<FName> & OutLabels)
{
	UPhysicsAsset* PhysicsAsset = GetPhysicsAsset();

	for (int32 BodyIdx = 0; BodyIdx < Bodies.Num(); ++BodyIdx)
	{
		FBodyInstance* BI = Bodies[BodyIdx];
		if (BI && BI->bWelded)
		{
			OutWeldedBodies.Add(&BodyInstance);
			if (PhysicsAsset)
			{
				if (UBodySetup * PhysicsAssetBodySetup = PhysicsAsset->BodySetup[BodyIdx])
				{
					OutLabels.Add(PhysicsAssetBodySetup->BoneName);
				}
				else
				{
					OutLabels.Add(NAME_None);
				}
			}
			else
			{
				OutLabels.Add(NAME_None);
			}

			for (USceneComponent * Child : AttachChildren)
			{
				if (UPrimitiveComponent * PrimChild = Cast<UPrimitiveComponent>(Child))
				{
					PrimChild->GetWeldedBodies(OutWeldedBodies, OutLabels);
				}
			}
		}
	}
}


void USkeletalMeshComponent::BreakConstraint(FVector Impulse, FVector HitLocation, FName InBoneName)
{
	// you can enable/disable the instanced weights by calling
	int32 ConstraintIndex = FindConstraintIndex(InBoneName);
	if( ConstraintIndex == INDEX_NONE || ConstraintIndex >= Constraints.Num() )
	{
		return;
	}

	FConstraintInstance* Constraint = Constraints[ConstraintIndex];
	// If already broken, our job has already been done. Bail!
	if( Constraint->IsTerminated() )
	{
		return;
	}

	UPhysicsAsset * const PhysicsAsset = GetPhysicsAsset();

	// Figure out if Body is fixed or not
	FBodyInstance* Body = GetBodyInstance(Constraint->JointName);

	if( Body != NULL && Body->IsInstanceSimulatingPhysics() )
	{
		// Unfix body so it can be broken.
		Body->SetInstanceSimulatePhysics(true);
	}

	// Break Constraint
	Constraint->TermConstraint();
	// Make sure child bodies and constraints are released and turned to physics.
	UpdateMeshForBrokenConstraints();
	// Add impulse to broken limb
	AddImpulseAtLocation(Impulse, HitLocation, InBoneName);
}


void USkeletalMeshComponent::SetPhysicsAsset(UPhysicsAsset* InPhysicsAsset, bool bForceReInit)
{
	// If this is different from what we have now, or we should have an instance but for whatever reason it failed last time, teardown/recreate now.
	if(bForceReInit || InPhysicsAsset != GetPhysicsAsset())
	{
		// SkelComp had a physics instance, then terminate it.
		TermArticulated();

		// Need to update scene proxy, because it keeps a ref to the PhysicsAsset.
		Super::SetPhysicsAsset(InPhysicsAsset, bForceReInit);
		MarkRenderStateDirty();

		// Update bHasValidBodies flag
		UpdateHasValidBodies();

		// Component should be re-attached here, so create physics.
		if( SkeletalMesh )
		{
			// Because we don't know what bones the new PhysicsAsset might want, we have to force an update to _all_ bones in the skeleton.
			RequiredBones.Reset(SkeletalMesh->RefSkeleton.GetNum());
			RequiredBones.AddUninitialized( SkeletalMesh->RefSkeleton.GetNum() );
			for(int32 i=0; i<SkeletalMesh->RefSkeleton.GetNum(); i++)
			{
				RequiredBones[i] = (FBoneIndexType)i;
			}
			RefreshBoneTransforms();

			// Initialize new Physics Asset
			if(World->GetPhysicsScene() != NULL && ShouldCreatePhysicsState())
			{
			//	UE_LOG(LogSkeletalMesh, Warning, TEXT("Creating Physics State (%s : %s)"), *GetNameSafe(GetOuter()),  *GetName());			
				InitArticulated(World->GetPhysicsScene());
			}
		}
		else
		{
			// If PhysicsAsset hasn't been instanced yet, just update the template.
			Super::SetPhysicsAsset(InPhysicsAsset, bForceReInit);

			// Update bHasValidBodies flag
			UpdateHasValidBodies();
		}

		// Indicate that 'required bones' array will need to be recalculated.
		bRequiredBonesUpToDate = false;
	}
}


void USkeletalMeshComponent::UpdateHasValidBodies()
{
	// First clear out old data
	bHasValidBodies = false;

	const UPhysicsAsset* const PhysicsAsset = GetPhysicsAsset();

	// If we have a physics asset..
	if(PhysicsAsset != NULL)
	{
		// For each body in physics asset..
		for( int32 BodyIndex = 0; BodyIndex < PhysicsAsset->BodySetup.Num(); BodyIndex++ )
		{
			// .. find the matching graphics bone index
			int32 BoneIndex = GetBoneIndex( PhysicsAsset->BodySetup[ BodyIndex ]->BoneName );

			// If we found a valid graphics bone, set the 'valid' flag
			if(BoneIndex != INDEX_NONE)
			{
				bHasValidBodies = true;
				break;
			}
		}
	}
}

void USkeletalMeshComponent::UpdatePhysicsToRBChannels()
{
	// Iterate over each bone/body.
	for (int32 i = 0; i < Bodies.Num(); i++)
	{
		FBodyInstance* BI = Bodies[i];
		check(BI);
		BI->UpdatePhysicsFilterData();
	}
}

FVector USkeletalMeshComponent::GetSkinnedVertexPosition(int32 VertexIndex) const
{
#if WITH_APEX_CLOTHING
	// only if this component has clothing and is showing simulated results	
	if (SkeletalMesh &&
		SkeletalMesh->ClothingAssets.Num() > 0 &&
		MeshObject &&
		!bDisableClothSimulation &&
		ClothBlendWeight > 0.0f // if cloth blend weight is 0.0, only showing skinned vertices regardless of simulation positions
		)
	{
		FStaticLODModel& Model = MeshObject->GetSkeletalMeshResource().LODModels[0];

		// Find the chunk and vertex within that chunk, and skinning type, for this vertex.
		int32 ChunkIndex;
		int32 VertIndexInChunk;
		bool bSoftVertex;
		bool bHasExtraBoneInfluences;
		Model.GetChunkAndSkinType(VertexIndex, ChunkIndex, VertIndexInChunk, bSoftVertex, bHasExtraBoneInfluences);

		bool bClothVertex = false;
		int32 ClothAssetIndex = -1;

		// if this chunk has cloth data
		if (Model.Chunks[ChunkIndex].HasApexClothData())
		{
			bClothVertex = true;
			ClothAssetIndex = Model.Chunks[ChunkIndex].CorrespondClothAssetIndex;
		}
		else
		{
			// if this chunk corresponds to a cloth section, returns corresponding cloth section's info instead
			for (int32 SectionIdx = 0; SectionIdx < Model.Sections.Num(); SectionIdx++)
			{
				const FSkelMeshSection& Section = Model.Sections[SectionIdx];

				// find a section which has this chunk index
				if (Section.ChunkIndex == ChunkIndex)
				{
					// if current section is disabled and the corresponding cloth section is visible
					if (Section.bDisabled && Section.CorrespondClothSectionIndex >= 0)
					{
						bClothVertex = true;

						const FSkelMeshSection& ClothSection = Model.Sections[Section.CorrespondClothSectionIndex];
						const FSkelMeshChunk& ClothChunk = Model.Chunks[ClothSection.ChunkIndex];

						ClothAssetIndex = ClothChunk.CorrespondClothAssetIndex;

						// the index can exceed the range because this vertex index is based on the corresponding original section
						// the number of cloth chunk's vertices is not always same as the corresponding one 
						// cloth chunk has only soft vertices
						if (VertIndexInChunk >= ClothChunk.GetNumSoftVertices())
						{
							// if the index exceeds, re-assign a random vertex index for this chunk
							VertIndexInChunk = FMath::TruncToInt(FMath::SRand() * (ClothChunk.GetNumSoftVertices() - 1));
						}
					}
					// if found, quit this loop quickly
					break;
				}
			}
		}

		if (bClothVertex)
		{
			FVector SimulatedPos;
			if (GetClothSimulatedPosition(ClothAssetIndex, VertIndexInChunk, SimulatedPos))
			{
				// a simulated position is in world space and convert this to local space
				// because SkinnedMeshComponent::GetSkinnedVertexPosition() returns the position in local space
				SimulatedPos = ComponentToWorld.InverseTransformPosition(SimulatedPos);

				// if blend weight is 1.0, doesn't need to blend with a skinned position
				if (ClothBlendWeight < 1.0f) 
				{
					// blend with a skinned position
					FVector SkinnedPos = Super::GetSkinnedVertexPosition(VertexIndex);
					SimulatedPos = SimulatedPos*ClothBlendWeight + SkinnedPos*(1.0f - ClothBlendWeight);
				}
				return SimulatedPos;
			}
		}
	}
#endif // #if WITH_APEX_CLOTHING
	return Super::GetSkinnedVertexPosition(VertexIndex);
}

//////////////////////////////////////////////////////////////////////////
// COLLISION

extern float DebugLineLifetime;

float USkeletalMeshComponent::GetDistanceToCollision(const FVector& Point, FVector& ClosestPointOnCollision) const
{
	ClosestPointOnCollision = Point;
	float ClosestPointDistance = -1.f;
	bool bHasResult = false;

	for (int32 BodyIdx = 0; BodyIdx < Bodies.Num(); ++BodyIdx)
	{
		FBodyInstance* BodyInstance = Bodies[BodyIdx];
		if (BodyInstance && BodyInstance->IsValidBodyInstance() && (BodyInstance->GetCollisionEnabled() != ECollisionEnabled::NoCollision))
		{
			FVector ClosestPoint;
			const float Distance = Bodies[BodyIdx]->GetDistanceToBody(Point, ClosestPoint);

			if (Distance < 0.f)
			{
				// Invalid result, impossible to be better than ClosestPointDistance
				continue;
			}

			if (!bHasResult || (Distance < ClosestPointDistance))
			{
				bHasResult = true;
				ClosestPointDistance = Distance;
				ClosestPointOnCollision = ClosestPoint;

				// If we're inside collision, we're not going to find anything better, so abort search we've got our best find.
				if (Distance <= KINDA_SMALL_NUMBER)
				{
					break;
				}
			}
		}
	}

	return ClosestPointDistance;
}

bool USkeletalMeshComponent::LineTraceComponent(struct FHitResult& OutHit, const FVector Start, const FVector End, const struct FCollisionQueryParams& Params)
{
	UWorld* const World = GetWorld();
	bool bHaveHit = false;

	float MinTime = MAX_FLT;
	FHitResult Hit;
	for (int32 BodyIdx=0; BodyIdx < Bodies.Num(); ++BodyIdx)
	{
		if (Bodies[BodyIdx]->LineTrace(Hit, Start, End, Params.bTraceComplex, Params.bReturnPhysicalMaterial))
		{
			bHaveHit = true;
			if(MinTime > Hit.Time)
			{
				MinTime = Hit.Time;
				OutHit = Hit;
			}
		}
	}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if(World && (World->DebugDrawTraceTag != NAME_None) && (World->DebugDrawTraceTag == Params.TraceTag))
	{
		TArray<FHitResult> Hits;
		if (bHaveHit)
		{
			Hits.Add(OutHit);
		}
		DrawLineTraces(GetWorld(), Start, End, Hits, DebugLineLifetime);
	}
#endif //!(UE_BUILD_SHIPPING || UE_BUILD_TEST)

	return bHaveHit;
}

bool USkeletalMeshComponent::SweepComponent( FHitResult& OutHit, const FVector Start, const FVector End, const FCollisionShape& CollisionShape, bool bTraceComplex)
{
	bool bHaveHit = false;

	for (int32 BodyIdx=0; BodyIdx < Bodies.Num(); ++BodyIdx)
	{
		if (Bodies[BodyIdx]->Sweep(OutHit, Start, End, CollisionShape, bTraceComplex))
		{
			bHaveHit = true;
		}
	}

	return bHaveHit;
}

bool USkeletalMeshComponent::ComponentOverlapComponentImpl(class UPrimitiveComponent* PrimComp,const FVector Pos,const FQuat& Quat,const struct FCollisionQueryParams& Params)
{
	//we do not support skeletal mesh vs skeletal mesh overlap test
	if (PrimComp->IsA<USkeletalMeshComponent>())
	{
		UE_LOG(LogCollision, Log, TEXT("ComponentOverlapComponent : (%s) Does not support skeletalmesh with Physics Asset"), *PrimComp->GetPathName());
		return false;
	}

	if (FBodyInstance* BI = PrimComp->GetBodyInstance())
	{
		return BI->OverlapTestForBodies(Pos, Quat, Bodies);
	}

	return false;
}

bool USkeletalMeshComponent::OverlapComponent(const FVector& Pos, const FQuat& Rot, const FCollisionShape& CollisionShape)
{
	for (FBodyInstance* Body : Bodies)
	{
		if (Body->OverlapTest(Pos, Rot, CollisionShape))
		{
			return true;
		}
	}

	return false;
}

bool USkeletalMeshComponent::ComponentOverlapMultiImpl(TArray<struct FOverlapResult>& OutOverlaps, const UWorld* World, const FVector& Pos, const FQuat& Quat, ECollisionChannel TestChannel, const struct FComponentQueryParams& Params, const struct FCollisionObjectQueryParams& ObjectQueryParams) const
{
	OutOverlaps.Reset();

	if (!Bodies.IsValidIndex(RootBodyData.BodyIndex))
	{
		return false;
	}

	const FTransform WorldToComponent(ComponentToWorld.Inverse());
	const FCollisionResponseParams ResponseParams(GetCollisionResponseToChannels());

	FComponentQueryParams ParamsWithSelf = Params;
	ParamsWithSelf.AddIgnoredComponent(this);

	bool bHaveBlockingHit = false;
	for (const FBodyInstance* Body : Bodies)
	{
		checkSlow(Body);
		if (Body->OverlapMulti(OutOverlaps, World, &WorldToComponent, Pos, Quat, TestChannel, ParamsWithSelf, ResponseParams, ObjectQueryParams))
		{
			bHaveBlockingHit = true;
		}
	}

	return bHaveBlockingHit;
}


#if WITH_APEX_CLOTHING

// convert a bone name from APEX stype to FBX style
static FName GetConvertedBoneName(NxClothingAsset* ApexClothingAsset, int32 BoneIndex)
{
	return *FString(ApexClothingAsset->getBoneName(BoneIndex)).Replace(TEXT(" "), TEXT("-"));
}

void USkeletalMeshComponent::AddClothingBounds(FBoxSphereBounds& InOutBounds) const
{
	int32 NumAssets = ClothingActors.Num();

	for(int32 i=0; i < NumAssets; i++)
	{
		if(IsValidClothingActor(i))
		{
			NxClothingActor* Actor = ClothingActors[i].ApexClothingActor;

			if(Actor)
			{
				physx::PxBounds3 ApexClothingBounds = Actor->getBounds();

				if (!ApexClothingBounds.isEmpty())
				{
					FBoxSphereBounds BoxBounds = FBox( P2UVector(ApexClothingBounds.minimum), P2UVector(ApexClothingBounds.maximum) );
					InOutBounds = InOutBounds + BoxBounds;
				}
			}
		}
	}
}

bool USkeletalMeshComponent::HasValidClothingActors()
{
	for(int32 i=0; i < ClothingActors.Num(); i++)
	{
		if(IsValidClothingActor(i))
		{
			return true;
		}
	}

	return false;
}

//if any changes in clothing assets, will create new actors 
void USkeletalMeshComponent::ValidateClothingActors()
{
	//newly spawned component's tick group could be "specified tick group + 1" for the first few frames
	//but it comes back to original tick group soon
	if(PrimaryComponentTick.GetActualTickGroup() != PrimaryComponentTick.TickGroup)
	{
		return;
	}

	if(!SkeletalMesh)
	{
		return;
	}

	int32 NumAssets = SkeletalMesh->ClothingAssets.Num();

	//if clothing assets are added or removed, re-create after removing all
	if(ClothingActors.Num() != NumAssets)
	{
		RemoveAllClothingActors();

		if(NumAssets > 0)
		{
			ClothingActors.Empty(NumAssets);
			ClothingActors.AddZeroed(NumAssets);
		}
	}

	bool bNeedUpdateLOD = false;

	for(int32 AssetIdx=0; AssetIdx < NumAssets; AssetIdx++)
	{
		FClothingAssetData& ClothAsset = SkeletalMesh->ClothingAssets[AssetIdx];

		// if there exist mapped sections for all LODs, create a clothing actor
		if (SkeletalMesh->HasClothSectionsInAllLODs(AssetIdx))
		{
			if(CreateClothingActor(AssetIdx, ClothAsset.ApexClothingAsset))
			{
				bNeedUpdateLOD = true;
			}
		}
		else 
		{	// don't have cloth sections but a clothing actor is alive
			if(IsValidClothingActor(AssetIdx))
			{
				// clear this clothing actor because mapped sections are removed
				ClothingActors[AssetIdx].Clear(true);
			}
		}
	}

	if(bNeedUpdateLOD)
	{
		SetClothingLOD(PredictedLODLevel);
	}
}

/** 
 * APEX clothing actor is created from APEX clothing asset for cloth simulation 
 * If this is invalid, re-create actor , but if valid ,just skip to create
*/
bool USkeletalMeshComponent::CreateClothingActor(int32 AssetIndex, physx::apex::NxClothingAsset* ClothingAsset, TArray<FVector>* BlendedDelta)
{	
	int32 NumActors = ClothingActors.Num();
	int32 ActorIndex = -1;

	//check we need to expand ClothingActors array 
	//actor is totally corresponding to asset, 1-on-1, so asset index should be same as actor index
	if(AssetIndex < NumActors)
	{
		ActorIndex = AssetIndex;

		if(IsValidClothingActor(ActorIndex))
		{
			// a valid actor already exists
			return false;
		}
		else
		{
			//removed or changed or not-created yet
			ClothingActors[ActorIndex].Clear();
		}
	}

	if(ActorIndex < 0)
	{
		ActorIndex = ClothingActors.AddZeroed();
	}
	 
	// Get the (singleton!) default actor descriptor.
	NxParameterized::Interface* ActorDesc = ClothingAsset->getDefaultActorDesc();
	PX_ASSERT(ActorDesc != NULL);

	// Run Cloth on the GPU
	verify(NxParameterized::setParamBool(*ActorDesc, "useHardwareCloth", true));
	verify(NxParameterized::setParamBool(*ActorDesc, "updateStateWithGlobalMatrices", true));

	FVector ScaleVector = ComponentToWorld.GetScale3D();

	//support only uniform scale
	verify(NxParameterized::setParamF32(*ActorDesc, "actorScale", ScaleVector.X));

	bool bUseInternalBoneOrder = true;

	verify(NxParameterized::setParamBool(*ActorDesc,"useInternalBoneOrder",bUseInternalBoneOrder));

	verify(NxParameterized::setParamF32(*ActorDesc,"maxDistanceBlendTime",1.0f));
	verify(NxParameterized::setParamF32(*ActorDesc,"lodWeights.maxDistance",10000));
	verify(NxParameterized::setParamF32(*ActorDesc,"lodWeights.distanceWeight",1.0f));
	verify(NxParameterized::setParamF32(*ActorDesc,"lodWeights.bias",0));
	verify(NxParameterized::setParamF32(*ActorDesc,"lodWeights.benefitsBias",0));

	verify(NxParameterized::setParamBool(*ActorDesc, "localSpaceSim", bLocalSpaceSimulation));

	// Initialize the global pose

	FMatrix UGlobalPose = ComponentToWorld.ToMatrixWithScale();

	physx::PxMat44 PxGlobalPose = U2PMatrix(UGlobalPose);

	PxGlobalPose = physx::PxMat44::createIdentity();

	verify(NxParameterized::setParamMat44(*ActorDesc, "globalPose", PxGlobalPose));

	// set max distance scale 
	// if set "maxDistanceScale.Multipliable" to true, scaled result looks more natural
	// @TODO : need to expose "Multipliable"?
	verify(NxParameterized::setParamBool(*ActorDesc, "maxDistanceScale.Multipliable", true));
	verify(NxParameterized::setParamF32(*ActorDesc, "maxDistanceScale.Scale", ClothMaxDistanceScale));

	// apply delta positions of cloth morph target
	if (BlendedDelta)
	{
		int32 NumBlendData = BlendedDelta->Num();
		TArray<PxVec3> PxBlendedData;
		PxBlendedData.AddUninitialized(NumBlendData);
		for (int32 Index = 0; Index < NumBlendData; Index++)
		{
			PxBlendedData[Index] = U2PVector((*BlendedDelta)[Index]);
		}
		NxParameterized::Handle md(*ActorDesc, "morphDisplacements");
		md.resizeArray(PxBlendedData.Num());
		md.setParamVec3Array(PxBlendedData.GetData(), PxBlendedData.Num());
	}

	FPhysScene* PhysScene = NULL;

	if(GetWorld())
	{
		PhysScene = GetWorld()->GetPhysicsScene();
	}

	//this clothing actor would be crated later because this becomes invalid
	if(!PhysScene)
	{
		return false;
	}

	physx::apex::NxApexScene* ScenePtr = PhysScene->GetApexScene(PST_Cloth);

	if(!ScenePtr)
	{
		// can't create clothing actor
		UE_LOG(LogSkeletalMesh, Log, TEXT("CreateClothingActor: Failed to create an actor becauase PhysX Scene doesn't exist"));
		return false;
	}

	physx::apex::NxApexActor* apexActor = ClothingAsset->createApexActor(*ActorDesc, *ScenePtr);
	physx::apex::NxClothingActor* ClothingActor = static_cast<physx::apex::NxClothingActor*>(apexActor);

	ClothingActors[ActorIndex].ApexClothingActor = ClothingActor;
	ClothingActors[ActorIndex].SceneType = PST_Cloth;
	ClothingActors[ActorIndex].PhysScene = PhysScene;

	if(!ClothingActor)
	{
		UE_LOG(LogSkeletalMesh, Log, TEXT("CreateClothingActor: Failed to create an clothing actor (%s)"), ANSI_TO_TCHAR(ClothingAsset->getName()));
		return false;
	}

	//set parent pointer to verify later whether became invalid or not
	ClothingActors[ActorIndex].ParentClothingAsset = ClothingAsset;

	// budget is millisecond units
	ScenePtr->setLODResourceBudget(100); // for temporary, 100ms

	ClothingActor->setGraphicalLOD(PredictedLODLevel);

	// 0 means no simulation
	ClothingActor->forcePhysicalLod(1); // 1 will be changed to "GetActivePhysicalLod()" later
	ClothingActor->setFrozen(false);

#if WITH_CLOTH_COLLISION_DETECTION
	// process clothing collisions once for the case that this component doesn't move
	if(bCollideWithEnvironment)
	{
		ProcessClothCollisionWithEnvironment();
	}
#endif // WITH_CLOTH_COLLISION_DETECTION

	return true;
}

void USkeletalMeshComponent::SetClothingLOD(int32 LODIndex)
{
	int32 NumActors = ClothingActors.Num();

	bool bFrozen = false;

	for (int32 AssetIndex = 0; AssetIndex<NumActors; AssetIndex++)
	{
		if (IsValidClothingActor(AssetIndex))
		{			
			FClothingActor& Actor = ClothingActors[AssetIndex];

			int32 CurLODIndex = (int32)Actor.ApexClothingActor->getGraphicalLod();

			// check whether clothing LOD is mapped for this LOD index
			bool IsMappedClothLOD = SkeletalMesh->IsMappedClothingLOD(LODIndex, AssetIndex);

			// Change Clothing LOD if a new LOD index is different from the current index
			if (IsMappedClothLOD && CurLODIndex != LODIndex)
			{
				//physical LOD is changed by graphical LOD
				Actor.ApexClothingActor->setGraphicalLOD(LODIndex);

				if (Actor.ApexClothingActor->isFrozen())
				{
					bFrozen = true;
				}
			}

			int32 NumClothLODs = Actor.ParentClothingAsset->getNumGraphicalLodLevels();

			// NOTE: forcePhysicalLod() is an deferred operation.   getActivePhysicalLod() may return previous state.
			// decide whether should enable or disable
			if (!IsMappedClothLOD || (LODIndex >= NumClothLODs))
			{
				//disable clothing simulation
				Actor.ApexClothingActor->forcePhysicalLod(0);
			}
			else
			{	
				// enable clothing simulation
				Actor.ApexClothingActor->forcePhysicalLod(1);
			}
		}
	}

	if(bFrozen)
	{
#if WITH_EDITOR
		FMessageDialog::Open(EAppMsgType::Ok, NSLOCTEXT("SkelmeshComponent", "Warning_FrozenCloth", "Clothing will be melted from frozen state"));
#endif
		// melt it because rendering mesh will be broken if frozen when changing LODs
		FreezeClothSection(false);
	}

}

void USkeletalMeshComponent::RemoveAllClothingActors()
{
	int32 NumActors = ClothingActors.Num();

	for(int32 i=0; i<NumActors; i++)
	{
		ClothingActors[i].Clear(IsValidClothingActor(i));
	}

	ClothingActors.Empty();
}

void USkeletalMeshComponent::ReleaseAllClothingResources()
{
#if WITH_CLOTH_COLLISION_DETECTION
	ReleaseAllParentCollisions();
	ReleaseAllChildrenCollisions();
	// should be called before removing clothing actors
	RemoveAllOverlappedComponentMap();
#endif // #if WITH_CLOTH_COLLISION_DETECTION

#if WITH_APEX_CLOTHING
	RemoveAllClothingActors();
#endif// #if WITH_APEX_CLOTHING
}

void USkeletalMeshComponent::ApplyWindForCloth(FClothingActor& ClothingActor)
{
	//to convert from normalized value( usually 0.0 to 1.0 ) to Apex clothing wind value
	const float WindUnitAmout = 2500.0f;

	NxClothingActor* ApexClothingActor = ClothingActor.ApexClothingActor;

	if(!ApexClothingActor)
	{
		return;
	}

	if(World && World->Scene)
	{
		// set wind
		if(IsWindEnabled())
		{
			FVector Position = ComponentToWorld.GetTranslation();

			FVector WindDirection;
			float WindSpeed;
			float WindMinGust;
			float WindMaxGust;
			World->Scene->GetWindParameters(Position, WindDirection, WindSpeed, WindMinGust, WindMaxGust);

			physx::PxVec3 WindVelocity(WindDirection.X, WindDirection.Y, WindDirection.Z);

			WindVelocity *= WindUnitAmout * WindSpeed;
			float WindAdaption = rand()%20 * 0.1f; // make range from 0 to 2

			NxParameterized::Interface* ActorDesc = ApexClothingActor->getActorDesc();

			if(ActorDesc)
			{
				verify(NxParameterized::setParamVec3(*ActorDesc, "windParams.Velocity", WindVelocity));
				verify(NxParameterized::setParamF32(*ActorDesc, "windParams.Adaption", WindAdaption));
			}
		}
		else
		{
			physx::PxVec3 WindVelocity(0.0f);
			NxParameterized::Interface* ActorDesc = ApexClothingActor->getActorDesc();
			if(ActorDesc)
			{
				verify(NxParameterized::setParamVec3(*ActorDesc, "windParams.Velocity", WindVelocity));
				// if turned off the wind, will do fast adaption
				verify(NxParameterized::setParamF32(*ActorDesc, "windParams.Adaption", 2.0f));
			}
		}
	}
}

#endif// #if WITH_APEX_CLOTHING


#if WITH_CLOTH_COLLISION_DETECTION

void USkeletalMeshComponent::DrawDebugConvexFromPlanes(FClothCollisionPrimitive& CollisionPrimitive, FColor& Color, bool bDrawWithPlanes)
{
	int32 NumPlanes = CollisionPrimitive.ConvexPlanes.Num();

	//draw with planes
	if(bDrawWithPlanes)
	{
		for(int32 PlaneIdx=0; PlaneIdx < NumPlanes; PlaneIdx++)
		{
			FPlane& Plane = CollisionPrimitive.ConvexPlanes[PlaneIdx];
			DrawDebugSolidPlane(GetWorld(), Plane, CollisionPrimitive.Origin, 10, Color);
		}
	}
	else
	{
		FVector UniquePoint;
		TArray<FVector> UniqueIntersectPoints;

		TArray<FPlane>& Planes = CollisionPrimitive.ConvexPlanes;

		//find all unique intersected points
		for(int32 i=0; i < NumPlanes; i++)
		{
			FPlane Plane1 = Planes[i];

			for(int32 j=i+1; j < NumPlanes; j++)
			{
				FPlane Plane2 = Planes[j];

				for(int32 k=j+1; k < NumPlanes; k++)
				{
					FPlane Plane3 = Planes[k];
					
					if(FMath::IntersectPlanes3(UniquePoint, Plane1, Plane2, Plane3))
					{
						UniqueIntersectPoints.Add(UniquePoint);
					}
				}
			}
		}

		int32 NumPts = UniqueIntersectPoints.Num();

		//shows all connected lines for just debugging
		for(int32 i=0; i < NumPts; i++)
		{
			for(int32 j=i+1; j < NumPts; j++)
			{
				DrawDebugLine(GetWorld(), UniqueIntersectPoints[i], UniqueIntersectPoints[j], Color, false, -1, SDPG_World, 2.0f);
			}
		}
	}
}

void USkeletalMeshComponent::DrawDebugClothCollisions()
{
	FColor Colors[6] = { FColor::Red, FColor::Green, FColor::Blue, FColor::Cyan, FColor::Yellow, FColor::Magenta };

	for( auto It = ClothOverlappedComponentsMap.CreateConstIterator(); It; ++It )
	{
		TWeakObjectPtr<UPrimitiveComponent> PrimComp = It.Key();

		TArray<FClothCollisionPrimitive> CollisionPrims;
		ECollisionChannel Channel = PrimComp->GetCollisionObjectType();
		if (Channel == ECollisionChannel::ECC_WorldStatic)
		{
			if (!GetClothCollisionDataFromStaticMesh(PrimComp.Get(), CollisionPrims))
		{
			return;
		}

		for(int32 PrimIndex=0; PrimIndex < CollisionPrims.Num(); PrimIndex++)
		{
			switch(CollisionPrims[PrimIndex].PrimType)
			{
			case FClothCollisionPrimitive::SPHERE:
				DrawDebugSphere(GetWorld(), CollisionPrims[PrimIndex].Origin, CollisionPrims[PrimIndex].Radius, 10, FColor::Red);
				break;

			case FClothCollisionPrimitive::CAPSULE:
				{
					FVector DiffVec = CollisionPrims[PrimIndex].SpherePos2 - CollisionPrims[PrimIndex].SpherePos1;
					float HalfHeight = DiffVec.Size() * 0.5f;
					FQuat Rotation = PrimComp->ComponentToWorld.GetRotation();
					DrawDebugCapsule(GetWorld(), CollisionPrims[PrimIndex].Origin, HalfHeight, CollisionPrims[PrimIndex].Radius, Rotation, FColor::Red );
				}

				break;
			case FClothCollisionPrimitive::CONVEX:	
				DrawDebugConvexFromPlanes(CollisionPrims[PrimIndex], Colors[PrimIndex%6]);
				break;

			case FClothCollisionPrimitive::PLANE:
				break;
			}

			//draw a bounding box for double checking
			DrawDebugBox(GetWorld(), PrimComp->Bounds.Origin, PrimComp->Bounds.BoxExtent, FColor::Red );
		}
	}
		else if (Channel == ECollisionChannel::ECC_PhysicsBody) // supports an interaction between the character and other clothing such as a curtain
		{
			bool bCreatedCollisions = false;
			// if a component is a skeletal mesh component with clothing, add my collisions to the component
			USkeletalMeshComponent* SkelMeshComp = Cast<USkeletalMeshComponent>(PrimComp.Get());
			if (SkelMeshComp && SkelMeshComp->SkeletalMesh)
			{
				if (SkelMeshComp->ClothingActors.Num() > 0)
				{
					TArray<FApexClothCollisionVolumeData> NewCollisions;
					FindClothCollisions(NewCollisions);

					for (int32 ColIdx = 0; ColIdx < NewCollisions.Num(); ColIdx++)
					{
						if (NewCollisions[ColIdx].IsCapsule())
						{
							FVector Origin = NewCollisions[ColIdx].LocalPose.GetOrigin();
							// apex uses y-axis as the up-axis of capsule
							FVector UpAxis = NewCollisions[ColIdx].LocalPose.GetScaledAxis(EAxis::Y);
							float Radius = NewCollisions[ColIdx].CapsuleRadius*UpAxis.Size();
							float HalfHeight = NewCollisions[ColIdx].CapsuleHeight*0.5f;

							FMatrix Pose = NewCollisions[ColIdx].LocalPose;
							FMatrix RotMat(Pose.GetScaledAxis(EAxis::X), Pose.GetScaledAxis(EAxis::Z), Pose.GetScaledAxis(EAxis::Y), FVector(0,0,0));
							FQuat Rotation = RotMat.ToQuat();
							DrawDebugCapsule(GetWorld(), Origin, HalfHeight, Radius, Rotation, FColor::Red);
						}
					}
				}
			}
		}
	}
	//draw this skeletal mesh component's bounding box

	DrawDebugBox(GetWorld(), Bounds.Origin, Bounds.BoxExtent, FColor::Red);

}

bool USkeletalMeshComponent::GetClothCollisionDataFromStaticMesh(UPrimitiveComponent* PrimComp, TArray<FClothCollisionPrimitive>& ClothCollisionPrimitives)
{
	//make sure Num of collisions should be 0 in the case this returns false
	ClothCollisionPrimitives.Empty();

	if(!PrimComp)
	{
		return false;
	}

	ECollisionChannel Channel = PrimComp->GetCollisionObjectType();
	// accept only a static mesh
	if (Channel != ECollisionChannel::ECC_WorldStatic)
	{
		return false;
	}

	if (!PrimComp->BodyInstance.IsValidBodyInstance())
	{
		return false;
	}

	bool bSuccess = false;
	PrimComp->BodyInstance.ExecuteOnPhysicsReadOnly([&]
	{
		TArray<PxShape*> AllShapes;
		const int32 NumSyncShapes = PrimComp->BodyInstance.GetAllShapes_AssumesLocked(AllShapes);

		if (NumSyncShapes == 0 || NumSyncShapes > 3) //skipping complicated object because of collision limitation
	{
			return;
	}

	FVector Center = PrimComp->Bounds.Origin;
	FTransform Transform = PrimComp->ComponentToWorld;
	FMatrix TransMat = Transform.ToMatrixWithScale();

		for (int32 ShapeIdx = 0; ShapeIdx < NumSyncShapes; ShapeIdx++)
	{
		PxGeometryType::Enum GeomType = AllShapes[ShapeIdx]->getGeometryType();

			switch (GeomType)
		{
		case PxGeometryType::eSPHERE:
			{
				FClothCollisionPrimitive ClothPrimData;

				PxSphereGeometry SphereGeom;
				AllShapes[ShapeIdx]->getSphereGeometry(SphereGeom);

				ClothPrimData.Origin = Center;
				ClothPrimData.Radius = SphereGeom.radius;
				ClothPrimData.PrimType = FClothCollisionPrimitive::SPHERE;

				ClothCollisionPrimitives.Add(ClothPrimData);
			}
			break;

		case PxGeometryType::eCAPSULE:
			{
				FClothCollisionPrimitive ClothPrimData;

				PxCapsuleGeometry CapsuleGeom;
				AllShapes[ShapeIdx]->getCapsuleGeometry(CapsuleGeom);

				ClothPrimData.Origin = Center;
				ClothPrimData.Radius = CapsuleGeom.radius;

				FVector ZAxis = TransMat.GetUnitAxis(EAxis::Z);
				float Radius = CapsuleGeom.radius;
				float HalfHeight = CapsuleGeom.halfHeight;
				ClothPrimData.SpherePos1 = Center + (HalfHeight * ZAxis);
				ClothPrimData.SpherePos2 = Center - (HalfHeight * ZAxis);

				ClothPrimData.PrimType = FClothCollisionPrimitive::CAPSULE;

				ClothCollisionPrimitives.Add(ClothPrimData);
			}
			break;
		case PxGeometryType::eBOX:
			{
				FClothCollisionPrimitive ClothPrimData;
				ClothPrimData.Origin = Center;
				ClothPrimData.Radius = 0;

				PxBoxGeometry BoxGeom;

				AllShapes[ShapeIdx]->getBoxGeometry(BoxGeom);

				ClothPrimData.ConvexPlanes.Empty(6); //box has 6 planes

				FPlane UPlane1(1, 0, 0, Center.X + BoxGeom.halfExtents.x);
				UPlane1 = UPlane1.TransformBy(TransMat);
				ClothPrimData.ConvexPlanes.Add(UPlane1);

				FPlane UPlane2(-1, 0, 0, Center.X - BoxGeom.halfExtents.x);
				UPlane2 = UPlane2.TransformBy(TransMat);
				ClothPrimData.ConvexPlanes.Add(UPlane2);

				FPlane UPlane3(0, 1, 0, Center.Y + BoxGeom.halfExtents.y);
				UPlane3 = UPlane3.TransformBy(TransMat);
				ClothPrimData.ConvexPlanes.Add(UPlane3);

				FPlane UPlane4(0, -1, 0, Center.Y - BoxGeom.halfExtents.y);
				UPlane4 = UPlane4.TransformBy(TransMat);
				ClothPrimData.ConvexPlanes.Add(UPlane4);

				FPlane UPlane5(0, 0, 1, Center.Z + BoxGeom.halfExtents.z);
				UPlane5 = UPlane5.TransformBy(TransMat);
				ClothPrimData.ConvexPlanes.Add(UPlane5);

				FPlane UPlane6(0, 0, -1, Center.Z - BoxGeom.halfExtents.z);
				UPlane6 = UPlane6.TransformBy(TransMat);
				ClothPrimData.ConvexPlanes.Add(UPlane6);

				ClothPrimData.PrimType = FClothCollisionPrimitive::CONVEX;
				ClothCollisionPrimitives.Add(ClothPrimData);
			}
			break;
		case PxGeometryType::eCONVEXMESH:
			{
				PxConvexMeshGeometry ConvexGeom;

				AllShapes[ShapeIdx]->getConvexMeshGeometry(ConvexGeom);

				if (ConvexGeom.convexMesh)
				{
					FClothCollisionPrimitive ClothPrimData;
					ClothPrimData.Origin = Center;
					ClothPrimData.Radius = 0;

					uint32 NumPoly = ConvexGeom.convexMesh->getNbPolygons();

					ClothPrimData.ConvexPlanes.Empty(NumPoly);

					for (uint32 Poly = 0; Poly < NumPoly; Poly++)
					{
						PxHullPolygon HullData;
						ConvexGeom.convexMesh->getPolygonData(Poly, HullData);
						physx::PxPlane PPlane(HullData.mPlane[0], HullData.mPlane[1], HullData.mPlane[2], HullData.mPlane[3]);
						FPlane UPlane = P2UPlane(PPlane);
						UPlane = UPlane.TransformBy(TransMat);
						ClothPrimData.ConvexPlanes.Add(UPlane);
					}

					ClothPrimData.PrimType = FClothCollisionPrimitive::CONVEX;
					ClothCollisionPrimitives.Add(ClothPrimData);
				}
			}
			break;
		}
	}
		bSuccess = true;
	});

	return bSuccess;
}

void USkeletalMeshComponent::FindClothCollisions(TArray<FApexClothCollisionVolumeData>& OutCollisions)
{
	if (!SkeletalMesh)
	{
		return;
	}

	int32 NumAssets = SkeletalMesh->ClothingAssets.Num();

	// find all new collisions passing to children
	for (int32 AssetIdx = 0; AssetIdx < NumAssets; AssetIdx++)
	{
		FClothingAssetData& Asset = SkeletalMesh->ClothingAssets[AssetIdx];
		int32 NumCollisions = Asset.ClothCollisionVolumes.Num();

		int32 ConvexCount = 0;
		for (int32 ColIdx = 0; ColIdx < NumCollisions; ColIdx++)
		{
			FApexClothCollisionVolumeData& Collision = Asset.ClothCollisionVolumes[ColIdx];

			if (Collision.BoneIndex < 0)
			{
				continue;
			}

			FName BoneName = GetConvertedBoneName(Asset.ApexClothingAsset, Collision.BoneIndex);

			int32 BoneIndex = GetBoneIndex(BoneName);

			if (BoneIndex < 0)
			{
				continue;
			}

			FMatrix BoneMat = GetBoneMatrix(BoneIndex);

			FMatrix LocalToWorld = Collision.LocalPose * BoneMat;

			if (Collision.IsCapsule())
			{
				FApexClothCollisionVolumeData NewCollision = Collision;
				NewCollision.LocalPose = LocalToWorld;
				OutCollisions.Add(NewCollision);
			}
			else
			{
				if (Asset.ClothCollisionConvexPlaneIndices.IsValidIndex(ConvexCount))
				{
					uint32 PlaneIndices = Asset.ClothCollisionConvexPlaneIndices[ConvexCount];

					uint32 BitShiftIndex = 1;
					for (uint32 PlaneIndex = 0; PlaneIndex < 32; PlaneIndex++)
					{
						if (PlaneIndices&BitShiftIndex)
						{
							FPlane UPlane = Asset.ClothCollisionVolumePlanes[PlaneIndex].PlaneData;

							check(Collision.BoneIndex == Asset.ClothCollisionVolumePlanes[PlaneIndex].BoneIndex);
							UPlane = UPlane.TransformBy(BoneMat);
							Collision.BonePlanes.Add(UPlane);
						}

						// shift 1 bit because a bit index means a plane's index 
						BitShiftIndex <<= 1;
					}
				}

				ConvexCount++;
			}
		}
	}
}

void USkeletalMeshComponent::CreateInternalClothCollisions(TArray<FApexClothCollisionVolumeData>& InCollisions, TArray<physx::apex::NxClothingCollision*>& OutCollisions)
{
	int32 NumCollisions = InCollisions.Num();

	const int32 MaxNumCapsules = 16;
	// because sphere number can not be larger than 32 and 1 capsule takes 2 spheres 
	NumCollisions = FMath::Min(NumCollisions, MaxNumCapsules);
	int32 NumActors = ClothingActors.Num();

		for(int32 ActorIdx=0; ActorIdx < NumActors; ActorIdx++)
		{
			if (!IsValidClothingActor(ActorIdx))
			{
				continue;
			}

		NxClothingActor* Actor = ClothingActors[ActorIdx].ApexClothingActor;
		int32 NumCurrentCapsules = SkeletalMesh->ClothingAssets[ActorIdx].ClothCollisionVolumes.Num(); // # of capsules 

			for(int32 ColIdx=0; ColIdx < NumCollisions; ColIdx++)
			{
				// capsules
				if (InCollisions[ColIdx].IsCapsule())
				{
					if(NumCurrentCapsules < MaxNumCapsules)
					{
				        FVector Origin = InCollisions[ColIdx].LocalPose.GetOrigin();
					        // apex uses y-axis as the up-axis of capsule
				        FVector UpAxis = InCollisions[ColIdx].LocalPose.GetScaledAxis(EAxis::Y);
					        
				        float Radius = InCollisions[ColIdx].CapsuleRadius*UpAxis.Size();
        
				        float HalfHeight = InCollisions[ColIdx].CapsuleHeight*0.5f;
					        const FVector TopEnd = Origin + (HalfHeight * UpAxis);
					        const FVector BottomEnd = Origin - (HalfHeight * UpAxis);
        
					        NxClothingSphere* Sphere1 = Actor->createCollisionSphere(U2PVector(TopEnd), Radius);
					        NxClothingSphere* Sphere2 = Actor->createCollisionSphere(U2PVector(BottomEnd), Radius);
        
					        NxClothingCapsule* Capsule = Actor->createCollisionCapsule(*Sphere1, *Sphere2);
        
				        OutCollisions.Add(Capsule);
				        NumCurrentCapsules++;
					}
				}
				else // convexes
				{
					int32 NumPlanes = InCollisions[ColIdx].BonePlanes.Num();

					TArray<NxClothingPlane*> ClothingPlanes;

					//can not exceed 32 planes
					NumPlanes = FMath::Min(NumPlanes, 32);

					ClothingPlanes.AddUninitialized(NumPlanes);

					for(int32 PlaneIdx=0; PlaneIdx < NumPlanes; PlaneIdx++)
					{
						PxPlane PPlane = U2PPlane(InCollisions[ColIdx].BonePlanes[PlaneIdx]);

						ClothingPlanes[PlaneIdx] = Actor->createCollisionPlane(PPlane);
					}

					NxClothingConvex* Convex = Actor->createCollisionConvex(ClothingPlanes.GetData(), ClothingPlanes.Num());

					OutCollisions.Add(Convex);
				}
			}
		}
	}

void USkeletalMeshComponent::CopyClothCollisionsToChildren()
{
	// 3 steps
	// 1. release all previous parent collisions
	// 2. find new collisions from parent(this class)
	// 3. add new collisions to children

	int32 NumChildren = AttachChildren.Num();
	TArray<USkeletalMeshComponent*> ClothChildren;

	for(int32 i=0; i < NumChildren; i++)
	{
		USkeletalMeshComponent* pChild = Cast<USkeletalMeshComponent>(AttachChildren[i]);
		if(pChild)
		{
			int32 NumActors = pChild->ClothingActors.Num();
			if(NumActors > 0)
			{
				ClothChildren.Add(pChild);
				// release all parent collisions
				pChild->ReleaseAllParentCollisions();
			}
		}
	}

	int32 NumClothChildren = ClothChildren.Num();

	if(NumClothChildren == 0)
	{
		return;
	}

	TArray<FApexClothCollisionVolumeData> NewCollisions;

	FindClothCollisions(NewCollisions);

	for(int32 ChildIdx=0; ChildIdx < NumClothChildren; ChildIdx++)
	{
		ClothChildren[ChildIdx]->CreateInternalClothCollisions(NewCollisions, ClothChildren[ChildIdx]->ParentCollisions);
	}
}

void USkeletalMeshComponent::ReleaseAllChildrenCollisions()
{
	for(int32 i=0; i<ChildrenCollisions.Num(); i++)
	{
		ReleaseClothingCollision(ChildrenCollisions[i]);
	}

	ChildrenCollisions.Empty();
}

// children's collisions can affect to parent's cloth reversely
void USkeletalMeshComponent::CopyChildrenClothCollisionsToParent()
{
	// 3 steps
	// 1. release all previous children collisions
	// 2. find new collisions from children
	// 3. add new collisions to parent (this component)

	ReleaseAllChildrenCollisions();

	int32 NumChildren = AttachChildren.Num();
	TArray<USkeletalMeshComponent*> ClothCollisionChildren;

	TArray<FApexClothCollisionVolumeData> NewCollisions;

	for(int32 i=0; i < NumChildren; i++)
	{
		USkeletalMeshComponent* pChild = Cast<USkeletalMeshComponent>(AttachChildren[i]);
		if(pChild)
		{
			pChild->FindClothCollisions(NewCollisions);
		}
	}

	CreateInternalClothCollisions(NewCollisions, ChildrenCollisions);
}

void USkeletalMeshComponent::ReleaseClothingCollision(NxClothingCollision* Collision)
{
	switch(Collision->getType())
	{
	case NxClothingCollisionType::Capsule:
		{
			NxClothingCapsule* Capsule = static_cast<NxClothingCapsule*>(Collision);

			check(Capsule);

			Capsule->releaseWithSpheres();
		}
		break;

	case NxClothingCollisionType::Convex:
		{
			NxClothingConvex* Convex = static_cast<NxClothingConvex*>(Collision);

			check(Convex);

			Convex->releaseWithPlanes();
		}
		break;

	default:
		Collision->release();
		break;
	}
}

FApexClothCollisionInfo* USkeletalMeshComponent::CreateNewClothingCollsions(UPrimitiveComponent* PrimitiveComponent)
{
	FApexClothCollisionInfo NewInfo;

	TArray<FClothCollisionPrimitive> CollisionPrims;

	ECollisionChannel Channel = PrimitiveComponent->GetCollisionObjectType();

	// crate new clothing collisions for a static mesh
	if (Channel == ECollisionChannel::ECC_WorldStatic)
	{
		if (!GetClothCollisionDataFromStaticMesh(PrimitiveComponent, CollisionPrims))
	{
		return NULL;
	}

		NewInfo.OverlapCompType = FApexClothCollisionInfo::OCT_STATIC;

	int32 NumActors = ClothingActors.Num();

	for(int32 ActorIdx=0; ActorIdx < NumActors; ActorIdx++)
	{
		NxClothingActor* Actor = ClothingActors[ActorIdx].ApexClothingActor;

		if(Actor)
		{	
			for(int32 PrimIndex=0; PrimIndex < CollisionPrims.Num(); PrimIndex++)
			{
				NxClothingCollision* ClothCol = NULL;

				switch(CollisionPrims[PrimIndex].PrimType)
				{
				case FClothCollisionPrimitive::SPHERE:
					ClothCol = Actor->createCollisionSphere(U2PVector(CollisionPrims[PrimIndex].Origin), CollisionPrims[PrimIndex].Radius);
					if(ClothCol)
					{
						NewInfo.ClothingCollisions.Add(ClothCol);
					}
					break;
				case FClothCollisionPrimitive::CAPSULE:
					{
						float Radius = CollisionPrims[PrimIndex].Radius;
						NxClothingSphere* Sphere1 = Actor->createCollisionSphere(U2PVector(CollisionPrims[PrimIndex].SpherePos1), Radius);
						NxClothingSphere* Sphere2 = Actor->createCollisionSphere(U2PVector(CollisionPrims[PrimIndex].SpherePos2), Radius);

						ClothCol = Actor->createCollisionCapsule(*Sphere1, *Sphere2);
						if(ClothCol)
						{
							NewInfo.ClothingCollisions.Add(ClothCol);
						}
					}
					break;
				case FClothCollisionPrimitive::CONVEX:

					int32 NumPlanes = CollisionPrims[PrimIndex].ConvexPlanes.Num();

					TArray<NxClothingPlane*> ClothingPlanes;

					//can not exceed 32 planes
					NumPlanes = FMath::Min(NumPlanes, 32);

					ClothingPlanes.AddUninitialized(NumPlanes);

					for(int32 PlaneIdx=0; PlaneIdx < NumPlanes; PlaneIdx++)
					{
						PxPlane PPlane = U2PPlane(CollisionPrims[PrimIndex].ConvexPlanes[PlaneIdx]);

						ClothingPlanes[PlaneIdx] = Actor->createCollisionPlane(PPlane);
					}

					ClothCol = Actor->createCollisionConvex(ClothingPlanes.GetData(), ClothingPlanes.Num());

					if(ClothCol)
					{
						NewInfo.ClothingCollisions.Add(ClothCol);
					}

					break;
				}
			}
		}
	}
	}
	else if (Channel == ECollisionChannel::ECC_PhysicsBody) // creates new clothing collisions for clothing objects
	{
		bool bCreatedCollisions = false;
		// if a component is a skeletal mesh component with clothing, add my collisions to the component
		USkeletalMeshComponent* SkelMeshComp = Cast<USkeletalMeshComponent>(PrimitiveComponent);

		if (SkelMeshComp && SkelMeshComp->SkeletalMesh)
		{
			// if a casted skeletal mesh component is myself, then skip
			if (SkelMeshComp == this)
			{
				return NULL;
			}

			if (SkelMeshComp->ClothingActors.Num() > 0)
			{
				TArray<FApexClothCollisionVolumeData> NewCollisions;
				// get collision infos computed by current bone transforms
				FindClothCollisions(NewCollisions);
				
				if (NewCollisions.Num() > 0)
				{
					NewInfo.OverlapCompType = FApexClothCollisionInfo::OCT_CLOTH;
					SkelMeshComp->CreateInternalClothCollisions(NewCollisions, NewInfo.ClothingCollisions);
					bCreatedCollisions = true;
				}
			}
		}

		if (bCreatedCollisions == false)
		{
			return NULL;
		}
	}

	return &ClothOverlappedComponentsMap.Add(PrimitiveComponent, NewInfo);
}

void USkeletalMeshComponent::RemoveAllOverlappedComponentMap()
{
	for( auto It = ClothOverlappedComponentsMap.CreateConstIterator(); It; ++It )
	{
		const FApexClothCollisionInfo& Info = It.Value();

		for(int32 i=0; i < Info.ClothingCollisions.Num(); i++)
		{
			ReleaseClothingCollision(Info.ClothingCollisions[i]);
		}

		ClothOverlappedComponentsMap.Remove(It.Key());
	}

	ClothOverlappedComponentsMap.Empty();
}

void USkeletalMeshComponent::ReleaseAllParentCollisions()
{
	for(int32 i=0; i<ParentCollisions.Num(); i++)
	{
		ReleaseClothingCollision(ParentCollisions[i]);
	}

	ParentCollisions.Empty();
}

void USkeletalMeshComponent::UpdateOverlappedComponent(UPrimitiveComponent* PrimComp, FApexClothCollisionInfo* Info)
{
	if (Info->OverlapCompType == FApexClothCollisionInfo::OCT_CLOTH)
	{
		int32 NumCollisions = Info->ClothingCollisions.Num();
		for (int32 i = 0; i < NumCollisions; i++)
		{
			ReleaseClothingCollision(Info->ClothingCollisions[i]);
		}

		Info->ClothingCollisions.Empty(NumCollisions);

		TArray<FApexClothCollisionVolumeData> NewCollisions;
		FindClothCollisions(NewCollisions);

		if (NewCollisions.Num() > 0)
		{
			USkeletalMeshComponent *SkelMeshComp = Cast<USkeletalMeshComponent>(PrimComp);
			if (SkelMeshComp)
			{
				SkelMeshComp->CreateInternalClothCollisions(NewCollisions, Info->ClothingCollisions);
			}
		}
	}
}

void USkeletalMeshComponent::ProcessClothCollisionWithEnvironment()
{
	// don't handle collision detection if this component is in editor
	if(!GetWorld()->IsGameWorld())
	{
		return;
	}

	// Increment the revision number
	ClothingCollisionRevision++;

	TArray<FOverlapResult> Overlaps;

	FCollisionObjectQueryParams ObjectParams;

	ObjectParams.AddObjectTypesToQuery(ECollisionChannel::ECC_WorldStatic);
	// to collide with other clothing objects
	ObjectParams.AddObjectTypesToQuery(ECollisionChannel::ECC_PhysicsBody);
	
	static FName ClothOverlapComponentsName(TEXT("ClothOverlapComponents"));
	FCollisionQueryParams Params(ClothOverlapComponentsName, false);

	GetWorld()->OverlapMultiByObjectType(Overlaps, Bounds.Origin, FQuat::Identity, ObjectParams, FCollisionShape::MakeBox(Bounds.BoxExtent), Params);

	for (int32 OverlapIdx=0; OverlapIdx<Overlaps.Num(); ++OverlapIdx)
	{
		const TWeakObjectPtr<UPrimitiveComponent>& Component = Overlaps[OverlapIdx].Component;
		if (Component.IsValid())
		{ 
			// add intersected cloth collision
			FApexClothCollisionInfo* Info = ClothOverlappedComponentsMap.Find(Component);

			if(!Info)
			{
				Info = CreateNewClothingCollsions(Component.Get());
			}

			// update new or valid entries to the current revision number
			if(Info)
			{
				Info->Revision = ClothingCollisionRevision;
				// if this component needs update every tick 
				UpdateOverlappedComponent(Component.Get(), Info);
			}
		}
	}

	// releases all invalid collisions in overlapped list 
	for( auto It = ClothOverlappedComponentsMap.CreateConstIterator(); It; ++It )
	{
		const FApexClothCollisionInfo& Info = It.Value();

		// Anything not updated above will have an old revision number.
		if( Info.Revision != ClothingCollisionRevision )
		{
			for(int32 i=0; i < Info.ClothingCollisions.Num(); i++)
			{
				ReleaseClothingCollision(Info.ClothingCollisions[i]);
			}

			ClothOverlappedComponentsMap.Remove(It.Key());
		}
	}
}

#endif// #if WITH_CLOTH_COLLISION_DETECTION

void USkeletalMeshComponent::PreClothTick(float DeltaTime, FTickFunction& ThisTickFunction)
{
	//IMPORTANT!
	//
	// The decision on whether to use PreClothTick or not is made by ShouldRunPreClothTick()
	// Any changes that are made to PreClothTick that effect whether it should be run or not
	// have to be reflected in ShouldRunPreClothTick() as well
	
	// if physics is disabled on dedicated server, no reason to be here. 
	if (!bEnablePhysicsOnDedicatedServer && IsRunningDedicatedServer())
	{
		FinalizeBoneTransform();
		return;
	}

	if (IsRegistered() && IsSimulatingPhysics())
	{
		SyncComponentToRBPhysics();
	}

	// this used to not run if not rendered, but that causes issues such as bounds not updated
	// causing it to not rendered, at the end, I think we should blend body positions
	// for example if you're only simulating, this has to happen all the time
	// whether looking at it or not, otherwise
	// @todo better solution is to check if it has moved by changing SyncComponentToRBPhysics to return true if anything modified
	// and run this if that is true or rendered
	// that will at least reduce the chance of mismatch
	// generally if you move your actor position, this has to happen to approximately match their bounds
	if (ShouldBlendPhysicsBones())
	{
		if (IsRegistered())
		{
			BlendInPhysics(ThisTickFunction);
		}
	}



#if WITH_APEX_CLOTHING
	// if skeletal mesh has clothing assets, call TickClothing
	if (SkeletalMesh && SkeletalMesh->ClothingAssets.Num() > 0)
	{
		TickClothing(DeltaTime, ThisTickFunction);
	}
#endif
}

#if WITH_APEX_CLOTHING

void USkeletalMeshComponent::UpdateClothTransform()
{
	int32 NumActors = ClothingActors.Num();

#if WITH_CLOTH_COLLISION_DETECTION

	if(bCollideWithAttachedChildren)
	{
		CopyClothCollisionsToChildren();
	}

	//check the environment when only transform is updated
	if(bCollideWithEnvironment && NumActors > 0)
	{
		ProcessClothCollisionWithEnvironment();
	}
#endif // WITH_CLOTH_COLLISION_DETECTION

	physx::PxMat44 PxGlobalPose = U2PMatrix(ComponentToWorld.ToMatrixWithScale());

	for(int32 ActorIdx=0; ActorIdx<NumActors; ActorIdx++)
	{
		// skip if ClothingActor is NULL or invalid
		if(!IsValidClothingActor(ActorIdx))
		{
			continue;
		}

		NxClothingActor* ClothingActor = ClothingActors[ActorIdx].ApexClothingActor;

		check(ClothingActor);

		NxParameterized::Interface* ActorDesc = ClothingActor->getActorDesc();

		verify(NxParameterized::setParamMat44(*ActorDesc, "globalPose", PxGlobalPose));
	}
}

void USkeletalMeshComponent::CheckClothTeleport(float DeltaTime)
{
	// Get the root bone transform
	FMatrix CurRootBoneMat = GetBoneMatrix(0);

	if(bNeedTeleportAndResetOnceMore)
	{
		ForceClothNextUpdateTeleportAndReset();
		bNeedTeleportAndResetOnceMore = false;
	}

	float DeltaTimeThreshold = 0.2f;
	// clothing simulation could be broken if frame-rate goes under 5 fps
	if(DeltaTime > DeltaTimeThreshold)
	{
		ForceClothNextUpdateTeleportAndReset();
	}

	// distance check 
	// TeleportDistanceThreshold is greater than Zero and not teleported yet
	if(TeleportDistanceThreshold > 0 && ClothTeleportMode == FClothingActor::Continuous)
	{
		float DistSquared = FVector::DistSquared(PrevRootBoneMatrix.GetOrigin(), CurRootBoneMat.GetOrigin());
		if ( DistSquared > ClothTeleportDistThresholdSquared ) // if it has traveled too far
		{
			ClothTeleportMode = bResetAfterTeleport ? FClothingActor::TeleportAndReset : FClothingActor::Teleport;
			// clothing reset is needed once more to avoid clothing pop up when moved the component too far
			bNeedTeleportAndResetOnceMore = true;
		}
	}

	// rotation check
	// if TeleportRotationThreshold is greater than Zero and the user didn't do force teleport
	if(TeleportRotationThreshold > 0 && ClothTeleportMode == FClothingActor::Continuous)
	{
		// Detect whether teleportation is needed or not
		// Rotation matrix's transpose means an inverse but can't use a transpose because this matrix includes scales
		FMatrix AInvB = CurRootBoneMat * PrevRootBoneMatrix.InverseFast();
		float Trace = AInvB.M[0][0] + AInvB.M[1][1] + AInvB.M[2][2];
		float CosineTheta = (Trace - 1.0f) / 2.0f; // trace = 1+2cos(theta) for a 3x3 matrix

		if ( CosineTheta < ClothTeleportCosineThresholdInRad ) // has the root bone rotated too much
		{
			ClothTeleportMode = bResetAfterTeleport ? FClothingActor::TeleportAndReset : FClothingActor::Teleport;
		}
	}

	PrevRootBoneMatrix = CurRootBoneMat;
}

void USkeletalMeshComponent::ChangeClothMorphTargetMapping(FClothMorphTargetData& MorphData, FName CurrentActivatedMorphName)
{
	int32 AssetIndex = MorphData.ClothAssetIndex;

	if(SkeletalMesh && SkeletalMesh->ClothingAssets.IsValidIndex(AssetIndex))
	{
		FClothingAssetData& Asset = SkeletalMesh->ClothingAssets[AssetIndex];
		// if different from prepared mapping, should do re-mapping
		if (Asset.PreparedMorphTargetName != CurrentActivatedMorphName)
		{
			TArray<PxVec3>	ClothOriginalPosArray;
			int32 NumOriginPos = MorphData.OriginPos.Num();
			ClothOriginalPosArray.AddUninitialized(NumOriginPos);
			for (int32 Index = 0; Index < NumOriginPos; Index++)
			{
				ClothOriginalPosArray[Index] = U2PVector(MorphData.OriginPos[Index]);
			}

			NxClothingAsset* ClothingAsset = Asset.ApexClothingAsset;
			float Epsilon = 0.0f;
			uint32 NumMapped = ClothingAsset->prepareMorphTargetMapping(ClothOriginalPosArray.GetData(), NumOriginPos, Epsilon);

			if ((int32)NumMapped < NumOriginPos)
			{
				// @TODO : 
				// The mapping still worked, but some vertices were mapped to an original vertex with more than epsilon differences.
				// Either bump the epsilon or, if too many failed, emit an error message about a potentially bad mapping

				// if more than half failed
				if ((int32)NumMapped < NumOriginPos / 2)
				{
					FMessageDialog::Open(EAppMsgType::Ok, NSLOCTEXT("SkelmeshComponent", "Warning_ClothMorphTargetMapping", "Mapping vertices for Cloth Morph Target too many failed! It could introduce a bad morphing result."));
				}			
			}

			// change prepared morph target name
			Asset.PreparedMorphTargetName = CurrentActivatedMorphName;
		}

	}
}

void USkeletalMeshComponent::PrepareClothMorphTargets()
{	
	// if didn't turn on the cloth morph target option or already precomputed
	if (!bClothMorphTarget || bPreparedClothMorphTargets)
	{ 
		return;
	}

	ClothMorphTargets.Empty();

	for (UMorphTarget* MorphTarget : SkeletalMesh->MorphTargets)
	{
		if (MorphTarget)
		{
			FName MorphTargetName = MorphTarget->GetFName();

			int32 NumVerts;
			FVertexAnimDelta* Vertices = MorphTarget->GetDeltasAtTime(0.0f, 0, NULL, NumVerts);

			check(MeshObject);
			// should exist at least 1 LODModel 
			check(MeshObject->GetSkeletalMeshResource().LODModels.Num() > 0);

			FStaticLODModel& Model = MeshObject->GetSkeletalMeshResource().LODModels[0];

			// Find the chunk and vertex within that chunk, and skinning type, for this vertex.
			int32 ChunkIndex;
			int32 VertIndexInChunk;
			bool bSoftVertex;
			bool bHasExtraBoneInfluences;
			int32 SectionIndex = INDEX_NONE;

			int32 NumAssets = SkeletalMesh->ClothingAssets.Num();
			TArray<TArray<PxVec3>> ClothOriginalPosArray;
			TArray<TArray<FVector>> ClothPositionDeltaArray;
			ClothOriginalPosArray.AddZeroed(NumAssets);
			ClothPositionDeltaArray.AddZeroed(NumAssets);

			int ClothChunkIndex = INDEX_NONE;

			for (int32 VertIdx = 0; VertIdx < NumVerts; VertIdx++)
			{
				Model.GetChunkAndSkinType(Vertices[VertIdx].SourceIdx, ChunkIndex, VertIndexInChunk, bSoftVertex, bHasExtraBoneInfluences);

				FSkelMeshChunk& Chunk = Model.Chunks[ChunkIndex];

				for (int32 SecIdx = 0; SecIdx < Model.Sections.Num(); SecIdx++)
				{
					FSkelMeshSection& Section = Model.Sections[SecIdx];
					if (Section.ChunkIndex == ChunkIndex)
					{
						// if current section is disabled and the corresponding cloth section is visible
						if (Section.bDisabled && Section.CorrespondClothSectionIndex >= 0)
						{
							SectionIndex = SecIdx;
							ClothChunkIndex = Model.Sections[Section.CorrespondClothSectionIndex].ChunkIndex;
						}
						break;
					}
				}

				if (SectionIndex != INDEX_NONE)
				{
					FVector Position;
					if (bSoftVertex)
					{
						Position = Chunk.SoftVertices[VertIndexInChunk].Position;
					}
					else
					{
						Position = Chunk.RigidVertices[VertIndexInChunk].Position;
					}

					int32 AssetIndex = Model.Chunks[ClothChunkIndex].CorrespondClothAssetIndex;
					// save to original positions
					ClothOriginalPosArray[AssetIndex].Add(U2PVector(Position));
					ClothPositionDeltaArray[AssetIndex].Add(Vertices[VertIdx].PositionDelta);
				}
			}

			// fill in FClothMorphTargetData array
			for (int32 AssetIdx = 0; AssetIdx < NumAssets; AssetIdx++)
			{
				if (ClothOriginalPosArray[AssetIdx].Num() > 0)
				{
					NxClothingAsset* ClothingAsset = SkeletalMesh->ClothingAssets[AssetIdx].ApexClothingAsset;
					float Epsilon = 0.0f;
					uint32 NumMapped = ClothingAsset->prepareMorphTargetMapping(ClothOriginalPosArray[AssetIdx].GetData(), ClothOriginalPosArray[AssetIdx].Num(), Epsilon);

					int32 NumOriginPos = ClothOriginalPosArray[AssetIdx].Num();
					if ((int32)NumMapped < NumOriginPos)
					{
						// @TODO : 
						// The mapping still worked, but some vertices were mapped to an original vertex with more than epsilon differences.
						// Either bump the epsilon or, if too many failed, emit an error message about a potentially bad mapping

						// if more than half failed
						if ((int32)NumMapped < NumOriginPos / 2)
						{
							FMessageDialog::Open(EAppMsgType::Ok, NSLOCTEXT("SkelmeshComponent", "Warning_ClothMorphTargetMapping", "Mapping vertices for Cloth Morph Target too many failed! It could introduce a bad morphing result."));
						}
					}
					
					SkeletalMesh->ClothingAssets[AssetIdx].PreparedMorphTargetName = MorphTargetName;

					FClothMorphTargetData *Data = new(ClothMorphTargets)FClothMorphTargetData;
					Data->MorphTargetName = MorphTargetName;
					Data->ClothAssetIndex = AssetIdx;
					Data->PrevWeight = 0.0f;

					// stores original positions
					TArray<PxVec3>& OriginPosArray = ClothOriginalPosArray[AssetIdx];
					NumOriginPos = OriginPosArray.Num();
					Data->OriginPos.AddUninitialized(NumOriginPos);
					for (int32 Index = 0; Index < NumOriginPos; Index++)
					{
						Data->OriginPos[Index] = P2UVector(OriginPosArray[Index]);
					}

					Data->PosDelta = ClothPositionDeltaArray[AssetIdx];
				}
			}
		}
	}

	bPreparedClothMorphTargets = true;
}

void USkeletalMeshComponent::UpdateClothMorphTarget()
{
	if (!bClothMorphTarget)
	{
		return;
	}

	for (int32 MorphIdx = 0; MorphIdx < ActiveVertexAnims.Num(); MorphIdx++)
	{
		if (ActiveVertexAnims[MorphIdx].Weight > 0.0f)
		{
			if (ActiveVertexAnims[MorphIdx].VertAnim)
			{
				FName MorphTargetName = ActiveVertexAnims[MorphIdx].VertAnim->GetFName();

				int32 SelectedClothMorphIndex = INDEX_NONE;
				for (int32 ClothMorphIdx = 0; ClothMorphIdx < ClothMorphTargets.Num(); ClothMorphIdx++)
				{
					if (ClothMorphTargets[ClothMorphIdx].MorphTargetName == MorphTargetName)
					{
						SelectedClothMorphIndex = ClothMorphIdx;
						break;
					}
				}

				// if this morph target is not cloth morph target, skip this index
				if (SelectedClothMorphIndex == INDEX_NONE)
				{
					continue;
				}

				FClothMorphTargetData& MorphData = ClothMorphTargets[SelectedClothMorphIndex];

				// if a currently mapped morph target name is same as MorphTargetName, doesn't change mapping. Otherwise, change morph target mapping
				ChangeClothMorphTargetMapping(MorphData, MorphTargetName);

				float CurWeight = ActiveVertexAnims[MorphIdx].Weight;

				if (CurWeight != MorphData.PrevWeight)
				{
					MorphData.PrevWeight = CurWeight;
					TArray<FVector> BlendedDelta;
					TArray<FVector>& OriginDelta = MorphData.PosDelta;

					if (OriginDelta.Num() > 0)
					{
						int32 ActorIndex = MorphData.ClothAssetIndex;

						if (!IsValidClothingActor(ActorIndex))
						{
							continue;
						}

						BlendedDelta.AddUninitialized(OriginDelta.Num());

						for (int32 DeltaIdx = 0; DeltaIdx < BlendedDelta.Num(); DeltaIdx++)
						{
							BlendedDelta[DeltaIdx] = (OriginDelta[DeltaIdx] * CurWeight);
						}

						// make current actor invalid
						ClothingActors[ActorIndex].Clear(true);
						CreateClothingActor(ActorIndex, SkeletalMesh->ClothingAssets[ActorIndex].ApexClothingAsset, &BlendedDelta);
					}
				}
			}
		}
	}
}

void USkeletalMeshComponent::UpdateClothState(float DeltaTime)
{
	if (CVarEnableClothPhysics.GetValueOnGameThread() == 0)
	{
		return;
	}

	// if turned on bClothMorphTarget option
	if (bClothMorphTarget)
	{
		// @TODO : if not in editor, doesn't need to check preparation of cloth morph targets in Tick
		// if pre-computation is not conducted yet
		if (!bPreparedClothMorphTargets)
		{
			PrepareClothMorphTargets();
		}
		UpdateClothMorphTarget();
	}

#if WITH_CLOTH_COLLISION_DETECTION

	if(bCollideWithAttachedChildren)
	{
		CopyClothCollisionsToChildren();
		CopyChildrenClothCollisionsToParent();
	}
#endif // WITH_CLOTH_COLLISION_DETECTION

	int32 NumActors = ClothingActors.Num();
	if(NumActors == 0)
	{
		return;
	}

	const TArray<FTransform>& BoneTransforms = MasterPoseComponent.IsValid() ? MasterPoseComponent.Get()->GetSpaceBases() : GetSpaceBases();

	if(BoneTransforms.Num() == 0)
	{
		return;
	}

	physx::PxMat44 PxGlobalPose = U2PMatrix(ComponentToWorld.ToMatrixWithScale());

	CheckClothTeleport(DeltaTime);

	// convert teleport mode to apex clothing teleport enum
	physx::apex::ClothingTeleportMode::Enum CurTeleportMode = (physx::apex::ClothingTeleportMode::Enum)ClothTeleportMode;

	for(int32 ActorIdx=0; ActorIdx<NumActors; ActorIdx++)
	{
		// skip if ClothingActor is NULL or invalid
		if(!IsValidClothingActor(ActorIdx))
		{
			continue;
		}

		ApplyWindForCloth(ClothingActors[ActorIdx]);

		TArray<physx::PxMat44> BoneMatrices;

		NxClothingAsset* ClothingAsset = ClothingActors[ActorIdx].ParentClothingAsset;

		uint32 NumUsedBones = ClothingAsset->getNumUsedBones();

		BoneMatrices.Empty(NumUsedBones);
		BoneMatrices.AddUninitialized(NumUsedBones);

		for(uint32 Index=0; Index < NumUsedBones; Index++)
		{
			FName BoneName = GetConvertedBoneName(ClothingActors[ActorIdx].ParentClothingAsset, Index);

		   int32 BoneIndex = GetBoneIndex(BoneName);

			if(MasterPoseComponent.IsValid())
			{
				int32 TempBoneIndex = BoneIndex;
				BoneIndex = INDEX_NONE;
				if(TempBoneIndex < MasterBoneMap.Num())
				{
					int32 MasterBoneIndex = MasterBoneMap[TempBoneIndex];
 
					// If ParentBoneIndex is valid, grab matrix from MasterPoseComponent.
					if( MasterBoneIndex != INDEX_NONE && 
						MasterBoneIndex < MasterPoseComponent->GetNumSpaceBases())
					{
						BoneIndex = MasterBoneIndex;
					}
				}
			}

		   if(BoneIndex != INDEX_NONE)
		   {
			   BoneMatrices[Index] = U2PMatrix(BoneTransforms[BoneIndex].ToMatrixWithScale());
		   }
		   else
		   {
			   BoneMatrices[Index] = PxMat44::createIdentity();
		   }
		}

		NxClothingActor* ClothingActor = ClothingActors[ActorIdx].ApexClothingActor;

		check(ClothingActor);

		// if bUseInternalboneOrder is set, "NumUsedBones" works, otherwise have to use "getNumBones" 
		ClothingActor->updateState(
			PxGlobalPose, 
			BoneMatrices.GetData(), 
			sizeof(physx::PxMat44), 
			NumUsedBones,
			CurTeleportMode);
	}

	// reset to Continuous
	ClothTeleportMode = FClothingActor::Continuous;
}

void USkeletalMeshComponent::GetClothRootBoneMatrix(int32 AssetIndex, FMatrix& OutRootBoneMatrix) const
{
	if (IsValidClothingActor(AssetIndex))
	{
		NxClothingAsset* Asset = ClothingActors[AssetIndex].ParentClothingAsset;

		check(Asset);

		const NxParameterized::Interface* AssetParams = Asset->getAssetNxParameterized();
		uint32 InternalRootBoneIndex;
		verify(NxParameterized::getParamU32(*AssetParams, "rootBoneIndex", InternalRootBoneIndex));
		check(InternalRootBoneIndex >= 0);
		FName BoneName = GetConvertedBoneName(Asset, InternalRootBoneIndex);
		int32 BoneIndex = GetBoneIndex(BoneName);
		check(BoneIndex >= 0);
		OutRootBoneMatrix = GetBoneMatrix(BoneIndex);
	}
	else
	{
		OutRootBoneMatrix = GetBoneMatrix(0);
	}
}

bool USkeletalMeshComponent::GetClothSimulatedPosition(int32 AssetIndex, int32 VertexIndex, FVector& OutSimulPos) const
{
	bool bSucceed = false;

	if (IsValidClothingActor(AssetIndex))
	{
		NxClothingActor* ApexClothingActor = ClothingActors[AssetIndex].ApexClothingActor;

		if (ApexClothingActor)
		{
			uint32 NumSimulVertices = ApexClothingActor->getNumSimulationVertices();

			// handles only simulated vertices, indices of fixed vertices are bigger than # of simulated vertices
			if ((uint32)VertexIndex < NumSimulVertices)
			{
				bSucceed = true;
				const physx::PxVec3* Vertices = ApexClothingActor->getSimulationPositions();

				if (bLocalSpaceSimulation)
				{
					FMatrix ClothRootBoneMatrix;
					GetClothRootBoneMatrix(AssetIndex, ClothRootBoneMatrix);
					OutSimulPos = ClothRootBoneMatrix.TransformPosition(P2UVector(Vertices[VertexIndex]));
				}
				else
				{
					OutSimulPos = P2UVector(Vertices[VertexIndex]);
				}
			}
		}
	}

	return bSucceed;
}

#endif// #if WITH_APEX_CLOTHING

class FTickClothingTask
{
	TWeakObjectPtr<USkeletalMeshComponent> SkeletalMeshComponent;
	float DeltaTime;

public:
	FTickClothingTask(TWeakObjectPtr<USkeletalMeshComponent> InSkeletalMeshComponent, float InDeltaTime)
		: SkeletalMeshComponent(InSkeletalMeshComponent)
		, DeltaTime(InDeltaTime)
	{
	}

	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FTickClothingTask, STATGROUP_TaskGraphTasks);
	}
	static ENamedThreads::Type GetDesiredThread()
	{
		return ENamedThreads::GameThread;
	}
	static ESubsequentsMode::Type GetSubsequentsMode()
	{
		return ESubsequentsMode::TrackSubsequents;
	}

	void DoTask(ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
	{
		//SCOPE_CYCLE_COUNTER(STAT_AnimGameThreadTime);
		if (USkeletalMeshComponent* Comp = SkeletalMeshComponent.Get())
		{
			Comp->PerformTickClothing(DeltaTime);
		}
	}
};

void USkeletalMeshComponent::PerformTickClothing(float DeltaTime)
{
	if (CVarEnableClothPhysics.GetValueOnGameThread() == 0)
	{
		return;
	}

#if WITH_APEX_CLOTHING
	// animated but bone transforms were not updated because it was not rendered
	if(PoseTickedThisFrame() && !bRecentlyRendered)
	{
		ForceClothNextUpdateTeleportAndReset();
	}
	else
	{
		ValidateClothingActors();
		UpdateClothState(DeltaTime);
	}
#if 0 //if turn on this flag, you can check which primitive objects are activated for collision detection
	DrawDebugClothCollisions();
#endif 
#endif// #if WITH_APEX_CLOTHING
}

void USkeletalMeshComponent::TickClothing(float DeltaTime, FTickFunction& ThisTickFunction)
{
	if(IsValidRef(ParallelBlendPhysicsCompletionTask))
	{
		FGraphEventArray Prerequistes;
		Prerequistes.Add(ParallelBlendPhysicsCompletionTask);
		FGraphEventRef TickClothingCompletionEvent = TGraphTask<FTickClothingTask>::CreateTask(&Prerequistes).ConstructAndDispatchWhenReady(this, DeltaTime);
		ThisTickFunction.GetCompletionHandle()->DontCompleteUntil(TickClothingCompletionEvent);
	}else
	{
		PerformTickClothing(DeltaTime);
	}
}

void USkeletalMeshComponent::GetUpdateClothSimulationData(TArray<FClothSimulData>& OutClothSimData, USkeletalMeshComponent* OverrideLocalRootComponent)
{
#if WITH_APEX_CLOTHING

	if (CVarEnableClothPhysics.GetValueOnAnyThread() == 0)
	{
		return;
	}

	int32 NumClothingActors = ClothingActors.Num();

	if(NumClothingActors == 0 || bDisableClothSimulation)
	{
		OutClothSimData.Empty();
		return;
	}

	if(OutClothSimData.Num() != NumClothingActors)
	{
		OutClothSimData.Empty(NumClothingActors);
		OutClothSimData.AddZeroed(NumClothingActors);
	}

	bool bSimulated = false;

	for(int32 ActorIndex=0; ActorIndex<NumClothingActors; ActorIndex++)
	{
		if(!IsValidClothingActor(ActorIndex))
		{
			OutClothSimData[ActorIndex].ClothSimulPositions.Empty();
			OutClothSimData[ActorIndex].ClothSimulNormals.Empty();
			continue;
		}

		NxClothingActor* ClothingActor = ClothingActors[ActorIndex].ApexClothingActor;

		// update simulation positions & normals
		if(ClothingActor)
		{
			uint32 NumSimulVertices = ClothingActor->getNumSimulationVertices();

			if(NumSimulVertices > 0)
			{
				bSimulated = true;

				FClothSimulData& ClothData = OutClothSimData[ActorIndex];

				if(ClothData.ClothSimulPositions.Num() != NumSimulVertices)
				{
					ClothData.ClothSimulPositions.Empty(NumSimulVertices);
					ClothData.ClothSimulPositions.AddUninitialized(NumSimulVertices);
					ClothData.ClothSimulNormals.Empty(NumSimulVertices);
					ClothData.ClothSimulNormals.AddUninitialized(NumSimulVertices);
				}

				const physx::PxVec3* Vertices = ClothingActor->getSimulationPositions();
				const physx::PxVec3* Normals = ClothingActor->getSimulationNormals();

				// In case of Local space simulation, need to transform simulated positions with the internal bone matrix
				if (bLocalSpaceSimulation)
				{
					FMatrix ClothRootBoneMatrix;
					(OverrideLocalRootComponent) ? OverrideLocalRootComponent->GetClothRootBoneMatrix(ActorIndex, ClothRootBoneMatrix) : GetClothRootBoneMatrix(ActorIndex, ClothRootBoneMatrix);
					for (uint32 VertexIndex = 0; VertexIndex < NumSimulVertices; VertexIndex++)
					{						
						ClothData.ClothSimulPositions[VertexIndex] = ClothRootBoneMatrix.TransformPosition(P2UVector(Vertices[VertexIndex]));
						ClothData.ClothSimulNormals[VertexIndex] = ClothRootBoneMatrix.TransformVector(P2UVector(Normals[VertexIndex]));
					}
				}
				else
				{
					for (uint32 VertexIndex = 0; VertexIndex < NumSimulVertices; VertexIndex++)
					{
						ClothData.ClothSimulPositions[VertexIndex] = P2UVector(Vertices[VertexIndex]);
						ClothData.ClothSimulNormals[VertexIndex] = P2UVector(Normals[VertexIndex]);
					}
				}
			}
		}
	}
	//no simulated vertices 
	if(!bSimulated)
	{
		OutClothSimData.Empty();
	}
#endif// #if WITH_APEX_CLOTHING
}

void USkeletalMeshComponent::FreezeClothSection(bool bFreeze)
{
#if WITH_APEX_CLOTHING

	int32 NumActors = ClothingActors.Num();

	for(int32 ActorIdx=0; ActorIdx<NumActors; ActorIdx++)
	{
		NxClothingActor* ClothingActor = ClothingActors[ActorIdx].ApexClothingActor;

		if (ClothingActor)
		{
			ClothingActor->setFrozen(bFreeze);
		}
	}
#endif// #if WITH_APEX_CLOTHING
}

bool USkeletalMeshComponent::IsValidClothingActor(int32 ActorIndex) const
{
#if WITH_APEX_CLOTHING

	if (!SkeletalMesh)
	{
		return false;
	}

	//false if ActorIndex is out-range
	if(ActorIndex >= SkeletalMesh->ClothingAssets.Num()
	|| ActorIndex >= ClothingActors.Num())
	{
		return false;
	}

	if(ClothingActors[ActorIndex].ApexClothingActor
	&& ClothingActors[ActorIndex].ParentClothingAsset == SkeletalMesh->ClothingAssets[ActorIndex].ApexClothingAsset)
	{
		return true;
	}

	return false;
	
#else
	return false;
#endif// #if WITH_APEX_CLOTHING

}

void USkeletalMeshComponent::DrawClothingNormals(FPrimitiveDrawInterface* PDI)
{
#if WITH_APEX_CLOTHING
	if (!SkeletalMesh) 
	{
		return;
	}

	int32 NumActors = ClothingActors.Num();

	for(int32 ActorIdx=0; ActorIdx<NumActors; ActorIdx++)
	{
		NxClothingActor* ClothingActor = ClothingActors[ActorIdx].ApexClothingActor;

		if(!IsValidClothingActor(ActorIdx))
		{
			continue;
		}

		if (ClothingActor)
		{
			uint32 NumSimulVertices = ClothingActor->getNumSimulationVertices();

			// simulation is enabled and there exist simulated vertices
			if(!bDisableClothSimulation && NumSimulVertices > 0)
			{
				const physx::PxVec3* Vertices = ClothingActor->getSimulationPositions();
				const physx::PxVec3* Normals = ClothingActor->getSimulationNormals();

				FLinearColor LineColor = FColor::Red;
				FVector Start, End;

				for(uint32 i=0; i<NumSimulVertices; i++)
				{
					Start = P2UVector(Vertices[i]);
					End = P2UVector(Normals[i]); 
					End *= 5.0f;

					End = Start + End;

					PDI->DrawLine(Start, End, LineColor, SDPG_World);
				}
			}
			else // otherwise, draws initial values loaded from the asset file
			{
				if(SkeletalMesh->ClothingAssets[ActorIdx].ClothVisualizationInfos.Num() == 0)
				{
					LoadClothingVisualizationInfo(ActorIdx);
				}

				int32 PhysicalMeshLOD = PredictedLODLevel;

				// if this asset doesn't have current physical mesh LOD, then skip to visualize
				if(!SkeletalMesh->ClothingAssets[ActorIdx].ClothVisualizationInfos.IsValidIndex(PhysicalMeshLOD))
				{
					continue;
				}

				FClothVisualizationInfo& VisualInfo = SkeletalMesh->ClothingAssets[ActorIdx].ClothVisualizationInfos[PhysicalMeshLOD];				

				uint32 NumVertices = VisualInfo.ClothPhysicalMeshVertices.Num();

				FLinearColor LineColor = FColor::Red;
				FVector Start, End;

				for(uint32 i=0; i<NumVertices; i++)
				{
					Start = VisualInfo.ClothPhysicalMeshVertices[i];
					End = VisualInfo.ClothPhysicalMeshNormals[i]; 

					End *= 5.0f;

					End = Start + End;

					PDI->DrawLine(Start, End, LineColor, SDPG_World);
				}
			}
		}
	}
#endif // #if WITH_APEX_CLOTHING
}

void USkeletalMeshComponent::DrawClothingTangents(FPrimitiveDrawInterface* PDI)
{
#if WITH_APEX_CLOTHING

	if (!SkeletalMesh || !MeshObject)
	{
		return;
	}

	int32 NumActors = ClothingActors.Num();

	for(int32 ActorIdx=0; ActorIdx<NumActors; ActorIdx++)
	{
		if(!IsValidClothingActor(ActorIdx))
		{
			continue;
		}

		NxClothingActor* ClothingActor = ClothingActors[ActorIdx].ApexClothingActor;

		if (ClothingActor)
		{
			uint32 NumSimulVertices = ClothingActor->getNumSimulationVertices();

			// simulation is enabled and there exist simulated vertices
			if(!bDisableClothSimulation && NumSimulVertices > 0)
			{
				uint16 ChunkIndex = 0;

				FStaticLODModel& LODModel = MeshObject->GetSkeletalMeshResource().LODModels[PredictedLODLevel];

				TArray<uint32> SectionIndices;
				SkeletalMesh->GetClothSectionIndices(PredictedLODLevel, ActorIdx, SectionIndices);

				int32 NumSections = SectionIndices.Num();

				for(int32 SecIdx=0; SecIdx <NumSections; SecIdx++)
				{
					uint16 SectionIndex = SectionIndices[SecIdx];

					ChunkIndex = LODModel.Sections[SectionIndex].ChunkIndex;

					FSkelMeshChunk& Chunk = LODModel.Chunks[ChunkIndex];

					const physx::PxVec3* SimulVertices = ClothingActor->getSimulationPositions();
					const physx::PxVec3* SimulNormals = ClothingActor->getSimulationNormals();

					uint32 NumMppingData = Chunk.ApexClothMappingData.Num();

					FVector Start, End;		

					for(uint32 MappingIndex=0; MappingIndex < NumMppingData; MappingIndex++)
					{
						FVector4 BaryCoordPos = Chunk.ApexClothMappingData[MappingIndex].PositionBaryCoordsAndDist;
						FVector4 BaryCoordNormal = Chunk.ApexClothMappingData[MappingIndex].NormalBaryCoordsAndDist;
						FVector4 BaryCoordTangent = Chunk.ApexClothMappingData[MappingIndex].TangentBaryCoordsAndDist;
						uint16*  SimulIndices = Chunk.ApexClothMappingData[MappingIndex].SimulMeshVertIndices;

						bool bFixed = (SimulIndices[3] == 0xFFFF);

						if(bFixed)
						{
							//if met a fixed vertex, skip to draw simulated vertices
							continue;
						}

						check(SimulIndices[0] < NumSimulVertices && SimulIndices[1] < NumSimulVertices && SimulIndices[2] < NumSimulVertices);

						PxVec3 a = SimulVertices[SimulIndices[0]];
						PxVec3 b = SimulVertices[SimulIndices[1]];
						PxVec3 c = SimulVertices[SimulIndices[2]];

						PxVec3 na = SimulNormals[SimulIndices[0]];
						PxVec3 nb = SimulNormals[SimulIndices[1]];
						PxVec3 nc = SimulNormals[SimulIndices[2]];

						FVector Position = P2UVector( 
							BaryCoordPos.X*(a + BaryCoordPos.W*na)
						  + BaryCoordPos.Y*(b + BaryCoordPos.W*nb)
						  + BaryCoordPos.Z*(c + BaryCoordPos.W*nc));

						FVector Normal = P2UVector(  
							  BaryCoordNormal.X*(a + BaryCoordNormal.W*na)
							+ BaryCoordNormal.Y*(b + BaryCoordNormal.W*nb) 
							+ BaryCoordNormal.Z*(c + BaryCoordNormal.W*nc));

						FVector Tangent = P2UVector(  
							  BaryCoordTangent.X*(a + BaryCoordTangent.W*na) 
							+ BaryCoordTangent.Y*(b + BaryCoordTangent.W*nb)  
							+ BaryCoordTangent.Z*(c + BaryCoordTangent.W*nc));

						Normal -= Position;
						Normal.Normalize();

						Tangent -= Position;
						Tangent.Normalize();

						FVector BiNormal = FVector::CrossProduct(Normal, Tangent);
						BiNormal.Normalize();

						Start = Position;
						End = Normal; 
						End *= 5.0f;
						End = Start + End;

						PDI->DrawLine(Start, End, FColor::Green, SDPG_World);

						End = Tangent; 
						End *= 5.0f;
						End = Start + End;

						PDI->DrawLine(Start, End, FColor::Red, SDPG_World);

						End = BiNormal; 
						End *= 5.0f;
						End = Start + End;

						PDI->DrawLine(Start, End, FColor::Blue, SDPG_World);
					}
				}
			}
			else
			{
				if(SkeletalMesh->ClothingAssets[ActorIdx].ClothVisualizationInfos.Num() == 0)
				{
					LoadClothingVisualizationInfo(ActorIdx);
				}

				int32 PhysicalMeshLOD = PredictedLODLevel;

				// if this asset doesn't have current physical mesh LOD, then skip to visualize
				if(!SkeletalMesh->ClothingAssets[ActorIdx].ClothVisualizationInfos.IsValidIndex(PhysicalMeshLOD))
				{
					continue;
				}

				FClothVisualizationInfo& VisualInfo = SkeletalMesh->ClothingAssets[ActorIdx].ClothVisualizationInfos[PhysicalMeshLOD];				

				NumSimulVertices = VisualInfo.ClothPhysicalMeshVertices.Num();

				uint16 ChunkIndex = 0;

				FStaticLODModel& LODModel = MeshObject->GetSkeletalMeshResource().LODModels[PredictedLODLevel];

				TArray<uint32> SectionIndices;
				SkeletalMesh->GetClothSectionIndices(PredictedLODLevel, ActorIdx, SectionIndices);

				int32 NumSections = SectionIndices.Num();

				for(int32 SecIdx=0; SecIdx <NumSections; SecIdx++)
				{
					uint16 SectionIndex = SectionIndices[SecIdx];

					ChunkIndex = LODModel.Sections[SectionIndex].ChunkIndex;

					FSkelMeshChunk& Chunk = LODModel.Chunks[ChunkIndex];

					const TArray<FVector>& SimulVertices = VisualInfo.ClothPhysicalMeshVertices;
					const TArray<FVector>& SimulNormals = VisualInfo.ClothPhysicalMeshNormals;

					uint32 NumMppingData = Chunk.ApexClothMappingData.Num();

					FVector Start, End;		

					for(uint32 MappingIndex=0; MappingIndex < NumMppingData; MappingIndex++)
					{
						FVector4 BaryCoordPos = Chunk.ApexClothMappingData[MappingIndex].PositionBaryCoordsAndDist;
						FVector4 BaryCoordNormal = Chunk.ApexClothMappingData[MappingIndex].NormalBaryCoordsAndDist;
						FVector4 BaryCoordTangent = Chunk.ApexClothMappingData[MappingIndex].TangentBaryCoordsAndDist;
						uint16*  SimulIndices = Chunk.ApexClothMappingData[MappingIndex].SimulMeshVertIndices;

						bool bFixed = (SimulIndices[3] == 0xFFFF);

						check(SimulIndices[0] < NumSimulVertices && SimulIndices[1] < NumSimulVertices && SimulIndices[2] < NumSimulVertices);

						FVector a = SimulVertices[SimulIndices[0]];
						FVector b = SimulVertices[SimulIndices[1]];
						FVector c = SimulVertices[SimulIndices[2]];

						FVector na = SimulNormals[SimulIndices[0]];
						FVector nb = SimulNormals[SimulIndices[1]];
						FVector nc = SimulNormals[SimulIndices[2]];

						FVector Position = 
							BaryCoordPos.X*(a + BaryCoordPos.W*na)
							+ BaryCoordPos.Y*(b + BaryCoordPos.W*nb)
							+ BaryCoordPos.Z*(c + BaryCoordPos.W*nc);

						FVector Normal = 
							BaryCoordNormal.X*(a + BaryCoordNormal.W*na)
							+ BaryCoordNormal.Y*(b + BaryCoordNormal.W*nb) 
							+ BaryCoordNormal.Z*(c + BaryCoordNormal.W*nc);

						FVector Tangent = 
							BaryCoordTangent.X*(a + BaryCoordTangent.W*na) 
							+ BaryCoordTangent.Y*(b + BaryCoordTangent.W*nb)  
							+ BaryCoordTangent.Z*(c + BaryCoordTangent.W*nc);

						Normal -= Position;
						Normal.Normalize();

						Tangent -= Position;
						Tangent.Normalize();

						FVector BiNormal = FVector::CrossProduct(Normal, Tangent);
						BiNormal.Normalize();

						Start = Position;
						End = Normal; 
						End *= 5.0f;
						End = Start + End;

						PDI->DrawLine(Start, End, FColor::Green, SDPG_World);

						End = Tangent; 
						End *= 5.0f;
						End = Start + End;

						PDI->DrawLine(Start, End, FColor::Red, SDPG_World);

						End = BiNormal; 
						End *= 5.0f;
						End = Start + End;

						PDI->DrawLine(Start, End, FColor::Blue, SDPG_World);
					}
					}
				}
			}
	}

#endif // #if WITH_APEX_CLOTHING
}

void USkeletalMeshComponent::DrawClothingCollisionVolumes(FPrimitiveDrawInterface* PDI)
{
#if WITH_APEX_CLOTHING
	if (!SkeletalMesh
	|| SkeletalMesh->ClothingAssets.Num() == 0) 
	{
		return;
	}

	static bool bDrawnPlanes = false;

	FColor Colors[3] = { FColor::Red, FColor::Green, FColor::Blue };
	FColor GrayColor(50, 50, 50); // dark gray to represent ignored collisions

	const int32 MaxSphereCollisions = 32; // Apex clothing supports up to 16 capsules or 32 spheres because 1 capsule takes 2 spheres

	for(int32 AssetIdx=0; AssetIdx < SkeletalMesh->ClothingAssets.Num(); AssetIdx++)
	{
		TArray<FApexClothCollisionVolumeData>& Collisions = SkeletalMesh->ClothingAssets[AssetIdx].ClothCollisionVolumes;
		int32 SphereCount = 0;

		for (const FApexClothCollisionVolumeData& Collision : Collisions)
		{
			if(Collision.BoneIndex < 0)
			{
				continue;
			}

			FName BoneName = GetConvertedBoneName(SkeletalMesh->ClothingAssets[AssetIdx].ApexClothingAsset, Collision.BoneIndex);
			
			int32 BoneIndex = GetBoneIndex(BoneName);

			if(BoneIndex < 0)
			{
				continue;
			}

			FMatrix BoneMat = GetBoneMatrix(BoneIndex);

			FMatrix LocalToWorld = Collision.LocalPose * BoneMat;

			if(Collision.IsCapsule())
			{
				FColor CapsuleColor = Colors[AssetIdx % 3];
				if (SphereCount >= MaxSphereCollisions)
				{
					CapsuleColor = GrayColor; // Draw in gray to show that these collisions are ignored
				}

				const int32 CapsuleSides = FMath::Clamp<int32>(Collision.CapsuleRadius/4.f, 16, 64);
				float CapsuleHalfHeight = (Collision.CapsuleHeight*0.5f+Collision.CapsuleRadius);
				float CapsuleRadius = Collision.CapsuleRadius*2.f;
				// swapped Y-axis and Z-axis to convert apex coordinate to UE coordinate
				DrawWireCapsule(PDI, LocalToWorld.GetOrigin(), LocalToWorld.GetUnitAxis(EAxis::X), LocalToWorld.GetUnitAxis(EAxis::Z), LocalToWorld.GetUnitAxis(EAxis::Y), CapsuleColor, Collision.CapsuleRadius, CapsuleHalfHeight, CapsuleSides, SDPG_World);
				SphereCount += 2; // 1 capsule takes 2 spheres
			}
			else // convex
			{
				int32 NumVerts = Collision.BoneVertices.Num();

				TArray<FVector> TransformedVerts;
				TransformedVerts.AddUninitialized(NumVerts);

				for(int32 VertIdx=0; VertIdx < NumVerts; VertIdx++)
				{
					TransformedVerts[VertIdx] = BoneMat.TransformPosition(Collision.BoneVertices[VertIdx]);
				}

				// just draw all connected wires to check convex shape
				for(int32 i=0; i < NumVerts-2; i++)
				{
					for(int32 j=i+1; j < NumVerts; j++)
					{
						PDI->DrawLine(TransformedVerts[i], TransformedVerts[j], Colors[AssetIdx%3], SDPG_World);
					}
				}
			}
		}

		TArray<FClothBonePlane>& BonePlanes = SkeletalMesh->ClothingAssets[AssetIdx].ClothCollisionVolumePlanes;
		int32 NumPlanes = BonePlanes.Num();

		FVector Origin = ComponentToWorld.GetLocation();


		for(int32 PlaneIdx=0; PlaneIdx < NumPlanes; PlaneIdx++)
		{
			FName BoneName = GetConvertedBoneName(SkeletalMesh->ClothingAssets[AssetIdx].ApexClothingAsset, BonePlanes[PlaneIdx].BoneIndex);

			int32 BoneIndex = GetBoneIndex(BoneName);

			if(BoneIndex < 0)
			{
				continue;
			}

			FMatrix BoneMat = GetBoneMatrix(BoneIndex);
			FPlane UPlane = BonePlanes[PlaneIdx].PlaneData.TransformBy(BoneMat);
			FVector BoneMatOrigin = BoneMat.GetOrigin();

			// draw once becasue of a bug in DrawDebugSolidPlane() method
			if(!bDrawnPlanes)
			{
				DrawDebugSolidPlane(GetWorld(), UPlane, BoneMatOrigin, 2, Colors[AssetIdx%3]);
			}

		}

		bDrawnPlanes = true;

		// draw bone spheres
		TArray<FApexClothBoneSphereData>& Spheres = SkeletalMesh->ClothingAssets[AssetIdx].ClothBoneSpheres;
		TArray<FVector> SpherePositions;

		int32 NumSpheres = Spheres.Num();

		SpherePositions.AddUninitialized(NumSpheres);

		for(int32 i=0; i<NumSpheres; i++)
		{
			if(Spheres[i].BoneIndex < 0)
			{
				continue;
			}

			FName BoneName = GetConvertedBoneName(SkeletalMesh->ClothingAssets[AssetIdx].ApexClothingAsset, Spheres[i].BoneIndex);

			int32 BoneIndex = GetBoneIndex(BoneName);

			if(BoneIndex < 0)
			{
				continue;
			}

			FMatrix BoneMat = GetBoneMatrix(BoneIndex);

			FVector SpherePos = BoneMat.TransformPosition(Spheres[i].LocalPos);

			SpherePositions[i] = SpherePos;
			FTransform SphereTransform(FQuat::Identity, SpherePos);

			FColor SphereColor = Colors[AssetIdx % 3];
			if (SphereCount >= MaxSphereCollisions)
			{
				SphereColor = GrayColor; // Draw in gray to show that these collisions are ignored
			}

			DrawWireSphere(PDI, SphereTransform, SphereColor, Spheres[i].Radius, 10, SDPG_World);
			SphereCount++;
		}

		// draw connections between bone spheres, this makes 2 sphere to a capsule
		TArray<uint16>& Connections = SkeletalMesh->ClothingAssets[AssetIdx].BoneSphereConnections;

		int32 NumConnections = Connections.Num();

		for(int32 i=0; i<NumConnections; i+=2)
		{
			uint16 Index1 = Connections[i];
			uint16 Index2 = Connections[i+1];

			DrawDebugLine(GetWorld(), SpherePositions[Index1], SpherePositions[Index2], FColor::Magenta, false, -1.0f, SDPG_Foreground);
		}
	}
#endif // #if WITH_APEX_CLOTHING
}

void USkeletalMeshComponent::DrawClothingFixedVertices(FPrimitiveDrawInterface* PDI)
{
#if WITH_APEX_CLOTHING

	if (!SkeletalMesh || !MeshObject)
	{
		return;
	}

	int32 NumActors = ClothingActors.Num();

	TArray<FMatrix> RefToLocals;
	// get local matrices for skinning
	UpdateRefToLocalMatrices(RefToLocals, this, GetSkeletalMeshResource(), 0, NULL);
	const float Inv_255 = 1.f/255.f;

	for(int32 ActorIdx=0; ActorIdx<NumActors; ActorIdx++)
	{
		if(!IsValidClothingActor(ActorIdx))
		{
			continue;
		}

		NxClothingActor* ClothingActor = ClothingActors[ActorIdx].ApexClothingActor;

		if (ClothingActor)
		{
			uint32 NumSimulVertices = ClothingActor->getNumSimulationVertices();

			if(SkeletalMesh->ClothingAssets[ActorIdx].ClothVisualizationInfos.Num() == 0)
			{
				LoadClothingVisualizationInfo(ActorIdx);
			}

			int32 PhysicalMeshLOD = PredictedLODLevel;

			// if this asset doesn't have current physical mesh LOD, then skip to visualize
			if(!SkeletalMesh->ClothingAssets[ActorIdx].ClothVisualizationInfos.IsValidIndex(PhysicalMeshLOD))
			{
				continue;
			}

			FClothVisualizationInfo& VisualInfo = SkeletalMesh->ClothingAssets[ActorIdx].ClothVisualizationInfos[PhysicalMeshLOD];				

			NumSimulVertices = VisualInfo.ClothPhysicalMeshVertices.Num();

			uint16 ChunkIndex = 0;

			FStaticLODModel& LODModel = MeshObject->GetSkeletalMeshResource().LODModels[PredictedLODLevel];

			TArray<uint32> SectionIndices;
			SkeletalMesh->GetClothSectionIndices(PredictedLODLevel, ActorIdx, SectionIndices);

			int32 NumSections = SectionIndices.Num();

			for(int32 SecIdx=0; SecIdx <NumSections; SecIdx++)
			{
				uint16 SectionIndex = SectionIndices[SecIdx];

				ChunkIndex = LODModel.Sections[SectionIndex].ChunkIndex;

				FSkelMeshChunk& Chunk = LODModel.Chunks[ChunkIndex];

				const TArray<FVector>& SimulVertices = VisualInfo.ClothPhysicalMeshVertices;
				const TArray<FVector>& SimulNormals = VisualInfo.ClothPhysicalMeshNormals;

				uint32 NumMppingData = Chunk.ApexClothMappingData.Num();

				FVector Start, End;		

				for(uint32 MappingIndex=0; MappingIndex < NumMppingData; MappingIndex++)
				{
					FVector4 BaryCoordPos = Chunk.ApexClothMappingData[MappingIndex].PositionBaryCoordsAndDist;
					FVector4 BaryCoordNormal = Chunk.ApexClothMappingData[MappingIndex].NormalBaryCoordsAndDist;
					FVector4 BaryCoordTangent = Chunk.ApexClothMappingData[MappingIndex].TangentBaryCoordsAndDist;
					uint16*  SimulIndices = Chunk.ApexClothMappingData[MappingIndex].SimulMeshVertIndices;

					bool bFixed = (SimulIndices[3] == 0xFFFF);

					if(!bFixed)
					{
						continue;
					}

					check(SimulIndices[0] < NumSimulVertices && SimulIndices[1] < NumSimulVertices && SimulIndices[2] < NumSimulVertices);
					
					FVector a = SimulVertices[SimulIndices[0]];
					FVector b = SimulVertices[SimulIndices[1]];
					FVector c = SimulVertices[SimulIndices[2]];

					FVector na = SimulNormals[SimulIndices[0]];
					FVector nb = SimulNormals[SimulIndices[1]];
					FVector nc = SimulNormals[SimulIndices[2]];

					FVector Position = 
						BaryCoordPos.X*(a + BaryCoordPos.W*na)
						+ BaryCoordPos.Y*(b + BaryCoordPos.W*nb)
						+ BaryCoordPos.Z*(c + BaryCoordPos.W*nc);

					FSoftSkinVertex& SoftVert = Chunk.SoftVertices[MappingIndex];

					// skinning, APEX clothing supports up to 4 bone influences for now
					const uint8* BoneIndices = SoftVert.InfluenceBones;
					const uint8* BoneWeights = SoftVert.InfluenceWeights;
				
					FMatrix SkinningMat(ForceInitToZero);

					for(int32 BoneWeightIdx=0; BoneWeightIdx < Chunk.MaxBoneInfluences; BoneWeightIdx++)
					{
						float Weight = BoneWeights[BoneWeightIdx]*Inv_255;
						SkinningMat += RefToLocals[Chunk.BoneMap[BoneIndices[BoneWeightIdx]]]*Weight;
					}

					FVector SkinnedPosition = SkinningMat.TransformPosition(Position);
					// draws a skinned fixed vertex
					PDI->DrawPoint(SkinnedPosition, FColor::Yellow, 2, SDPG_World);
				}
			}
		}
	}

#endif // #if WITH_APEX_CLOTHING
}

void USkeletalMeshComponent::LoadClothingVisualizationInfo(int32 AssetIndex)
{
#if WITH_APEX_CLOTHING

	if (!SkeletalMesh || !SkeletalMesh->ClothingAssets.IsValidIndex(AssetIndex)) 
	{
		return;
	}


	FClothingAssetData& AssetData = SkeletalMesh->ClothingAssets[AssetIndex];
	NxClothingAsset* ApexClothingAsset = AssetData.ApexClothingAsset;
	const NxParameterized::Interface* AssetParams = ApexClothingAsset->getAssetNxParameterized();

	int32 NumPhysicalLODs;
	NxParameterized::getParamArraySize(*AssetParams, "physicalMeshes", NumPhysicalLODs);

	check(NumPhysicalLODs == ApexClothingAsset->getNumGraphicalLodLevels());

	// prepares to load data for each LOD
	AssetData.ClothVisualizationInfos.Empty(NumPhysicalLODs);
	AssetData.ClothVisualizationInfos.AddZeroed(NumPhysicalLODs);

	for(int32 LODIndex=0; LODIndex<NumPhysicalLODs; LODIndex++)
	{
		FClothVisualizationInfo& VisualInfo = AssetData.ClothVisualizationInfos[LODIndex];

		char ParameterName[MAX_SPRINTF];
		FCStringAnsi::Sprintf(ParameterName, "physicalMeshes[%d]", LODIndex);

		NxParameterized::Interface* PhysicalMeshParams;
		uint32 NumVertices = 0;
		uint32 NumIndices = 0;
		// physical mesh vertices & normals
		if (NxParameterized::getParamRef(*AssetParams, ParameterName, PhysicalMeshParams))
		{
			if(PhysicalMeshParams != NULL)
			{
				verify(NxParameterized::getParamU32(*PhysicalMeshParams, "physicalMesh.numVertices", NumVertices));

				// physical mesh vertices
				physx::PxI32 VertexCount = 0;
				if (NxParameterized::getParamArraySize(*PhysicalMeshParams, "physicalMesh.vertices", VertexCount))
				{
					check(VertexCount == NumVertices);
					VisualInfo.ClothPhysicalMeshVertices.Empty(NumVertices);

					for (uint32 VertexIndex = 0; VertexIndex < NumVertices; ++VertexIndex)
					{
						FCStringAnsi::Sprintf( ParameterName, "physicalMesh.vertices[%d]", VertexIndex );
						NxParameterized::Handle MeshVertexHandle(*PhysicalMeshParams);
						if (NxParameterized::findParam(*PhysicalMeshParams, ParameterName, MeshVertexHandle) != NULL)
						{
							physx::PxVec3  Vertex;
							MeshVertexHandle.getParamVec3(Vertex);
							VisualInfo.ClothPhysicalMeshVertices.Add(P2UVector(Vertex));
						}
					}
				}

				// bone weights & bone indices

				physx::PxI32 BoneWeightsCount = 0;
				if (NxParameterized::getParamArraySize(*PhysicalMeshParams, "physicalMesh.boneWeights", BoneWeightsCount))
				{
					VisualInfo.ClothPhysicalMeshBoneWeightsInfo.AddZeroed(VertexCount);

					int32 MaxBoneWeights = BoneWeightsCount / VertexCount;
					VisualInfo.NumMaxBoneInfluences = MaxBoneWeights;

					physx::PxI32 BoneIndicesCount = 0;
					verify(NxParameterized::getParamArraySize(*PhysicalMeshParams, "physicalMesh.boneIndices", BoneIndicesCount));
					check(BoneIndicesCount == BoneWeightsCount);

					for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
					{
						for(uint8 WeightIndex=0; WeightIndex < MaxBoneWeights; ++WeightIndex)
						{
							uint32 CurBoneWeightIndex = VertexIndex*MaxBoneWeights + WeightIndex;

							NxParameterized::Handle BoneIndexHandle(*PhysicalMeshParams);
							FCStringAnsi::Sprintf( ParameterName, "physicalMesh.boneIndices[%d]", CurBoneWeightIndex );
							verify(NxParameterized::findParam(*PhysicalMeshParams, ParameterName, BoneIndexHandle));
							uint16 BoneIndex;
							BoneIndexHandle.getParamU16(BoneIndex);
							VisualInfo.ClothPhysicalMeshBoneWeightsInfo[VertexIndex].Indices[WeightIndex] = BoneIndex;

							NxParameterized::Handle BoneWeightHandle(*PhysicalMeshParams);
							FCStringAnsi::Sprintf( ParameterName, "physicalMesh.boneWeights[%d]", CurBoneWeightIndex );
							verify(NxParameterized::findParam(*PhysicalMeshParams, ParameterName, BoneWeightHandle));
							float BoneWeight;
							BoneWeightHandle.getParamF32(BoneWeight);
							VisualInfo.ClothPhysicalMeshBoneWeightsInfo[VertexIndex].Weights[WeightIndex] = BoneWeight;
						}
					}
				}

				// physical mesh normals
				physx::PxI32 NormalCount = 0;
				if (NxParameterized::getParamArraySize(*PhysicalMeshParams, "physicalMesh.normals", NormalCount))
				{
					check(NormalCount == NumVertices);
					VisualInfo.ClothPhysicalMeshNormals.Empty(NormalCount);

					for (int32 NormalIndex = 0; NormalIndex < NormalCount; ++NormalIndex)
					{
						FCStringAnsi::Sprintf( ParameterName, "physicalMesh.normals[%d]", NormalIndex );
						NxParameterized::Handle MeshNormalHandle(*PhysicalMeshParams);
						if (NxParameterized::findParam(*PhysicalMeshParams, ParameterName, MeshNormalHandle) != NULL)
						{
							physx::PxVec3  PxNormal;
							MeshNormalHandle.getParamVec3(PxNormal);
							VisualInfo.ClothPhysicalMeshNormals.Add(P2UVector(PxNormal));
						}
					}
				}
				// physical mesh indices
				verify(NxParameterized::getParamU32(*PhysicalMeshParams, "physicalMesh.numIndices", NumIndices));
				physx::PxI32 IndexCount = 0;
				if (NxParameterized::getParamArraySize(*PhysicalMeshParams, "physicalMesh.indices", IndexCount))
				{
					check(IndexCount == NumIndices);
					VisualInfo.ClothPhysicalMeshIndices.Empty(NumIndices);

					for (uint32 IndexIdx = 0; IndexIdx < NumIndices; ++IndexIdx)
					{
						FCStringAnsi::Sprintf( ParameterName, "physicalMesh.indices[%d]", IndexIdx );
						NxParameterized::Handle MeshIndexHandle(*PhysicalMeshParams);
						if (NxParameterized::findParam(*PhysicalMeshParams, ParameterName, MeshIndexHandle) != NULL)
						{
							uint32 IndexParam;
							MeshIndexHandle.getParamU32(IndexParam);
							VisualInfo.ClothPhysicalMeshIndices.Add(IndexParam);
						}
					}
				}

				// constraint coefficient parameters (max distances & backstop data)
				verify(NxParameterized::getParamF32(*PhysicalMeshParams, "physicalMesh.maximumMaxDistance", VisualInfo.MaximumMaxDistance));

				physx::PxI32  ConstraintCoeffCount = 0;
				if (NxParameterized::getParamArraySize(*PhysicalMeshParams, "physicalMesh.constrainCoefficients", ConstraintCoeffCount))
				{
					check(ConstraintCoeffCount == NumVertices);
					VisualInfo.ClothConstrainCoeffs.Empty(ConstraintCoeffCount);
					VisualInfo.ClothConstrainCoeffs.AddZeroed(ConstraintCoeffCount);

					for (uint32 ConstCoeffIdx = 0; ConstCoeffIdx < NumIndices; ++ConstCoeffIdx)
					{
						// max distances
						FCStringAnsi::Sprintf( ParameterName, "physicalMesh.constrainCoefficients[%d].maxDistance", ConstCoeffIdx );
						NxParameterized::Handle MeshConstCoeffHandle(*PhysicalMeshParams);
						if (NxParameterized::findParam(*PhysicalMeshParams, ParameterName, MeshConstCoeffHandle) != NULL)
						{
							float MaxDistance;
							MeshConstCoeffHandle.getParamF32(MaxDistance);
							VisualInfo.ClothConstrainCoeffs[ConstCoeffIdx].ClothMaxDistance = MaxDistance;
						}

						// backstop data
						FCStringAnsi::Sprintf( ParameterName, "physicalMesh.constrainCoefficients[%d].collisionSphereRadius", ConstCoeffIdx );
						if (NxParameterized::findParam(*PhysicalMeshParams, ParameterName, MeshConstCoeffHandle) != NULL)
						{
							float BackstopCollisionSphereRadius;
							MeshConstCoeffHandle.getParamF32(BackstopCollisionSphereRadius);
							VisualInfo.ClothConstrainCoeffs[ConstCoeffIdx].ClothBackstopRadius = BackstopCollisionSphereRadius;
						}

						FCStringAnsi::Sprintf( ParameterName, "physicalMesh.constrainCoefficients[%d].collisionSphereDistance", ConstCoeffIdx );
						if (NxParameterized::findParam(*PhysicalMeshParams, ParameterName, MeshConstCoeffHandle) != NULL)
						{
							float BackstopCollisionSphereDistance;
							MeshConstCoeffHandle.getParamF32(BackstopCollisionSphereDistance);
							VisualInfo.ClothConstrainCoeffs[ConstCoeffIdx].ClothBackstopDistance = BackstopCollisionSphereDistance;
						}
					}
				}
			}
		}
	}

#endif // #if WITH_APEX_CLOTHING
}

void USkeletalMeshComponent::LoadAllClothingVisualizationInfos()
{
#if WITH_APEX_CLOTHING

	if (!SkeletalMesh || SkeletalMesh->ClothingAssets.Num() == 0) 
	{
		return;
	}

	int32 NumAssets = SkeletalMesh->ClothingAssets.Num();

	for(int32 AssetIdx=0; AssetIdx<NumAssets; AssetIdx++)
	{
		LoadClothingVisualizationInfo(AssetIdx);
	}
#endif // #if WITH_APEX_CLOTHING
}

void USkeletalMeshComponent::DrawClothingMaxDistances(FPrimitiveDrawInterface* PDI)
{
#if WITH_APEX_CLOTHING
	if (!SkeletalMesh
		|| SkeletalMesh->ClothingAssets.Num() == 0) 
	{
		return;
	}

	int32 PhysicalMeshLOD = PredictedLODLevel;

	int32 NumAssets = SkeletalMesh->ClothingAssets.Num();

	for(int32 AssetIdx=0; AssetIdx < NumAssets; AssetIdx++)
	{
		if(SkeletalMesh->ClothingAssets[AssetIdx].ClothVisualizationInfos.Num() == 0)
		{
			LoadClothingVisualizationInfo(AssetIdx);
		}

		// if this asset doesn't have current physical mesh LOD, then skip to visualize
		if(!SkeletalMesh->ClothingAssets[AssetIdx].ClothVisualizationInfos.IsValidIndex(PhysicalMeshLOD))
		{
			continue;
		}

		FClothVisualizationInfo& VisualInfo = SkeletalMesh->ClothingAssets[AssetIdx].ClothVisualizationInfos[PhysicalMeshLOD];

		int32 NumMaxDistances = VisualInfo.ClothConstrainCoeffs.Num();
		for(int32 MaxDistIdx=0; MaxDistIdx < NumMaxDistances; MaxDistIdx++)
		{
			float MaxDistance = VisualInfo.ClothConstrainCoeffs[MaxDistIdx].ClothMaxDistance;
			if(MaxDistance > 0.0f)
			{
				FVector LineStart = VisualInfo.ClothPhysicalMeshVertices[MaxDistIdx];
				FVector LineEnd = LineStart + (VisualInfo.ClothPhysicalMeshNormals[MaxDistIdx] * MaxDistance);

				// calculates gray scale for this line color
				uint8 GrayLevel = (uint8)((MaxDistance / VisualInfo.MaximumMaxDistance)*255);
				PDI->DrawLine(LineStart, LineEnd, FColor(GrayLevel, GrayLevel, GrayLevel), SDPG_World);
			}
			else // fixed vertices
			{
				FVector FixedPoint = VisualInfo.ClothPhysicalMeshVertices[MaxDistIdx];
				PDI->DrawPoint(FixedPoint, FColor::Blue, 2, SDPG_World);
			}
		}
	}	

#endif // #if WITH_APEX_CLOTHING
}

void USkeletalMeshComponent::DrawClothingBackstops(FPrimitiveDrawInterface* PDI)
{
#if WITH_APEX_CLOTHING
	if (!SkeletalMesh
		|| SkeletalMesh->ClothingAssets.Num() == 0) 
	{
		return;
	}

	int32 PhysicalMeshLOD = PredictedLODLevel;

	int32 NumAssets = SkeletalMesh->ClothingAssets.Num();

	for(int32 AssetIdx=0; AssetIdx < NumAssets; AssetIdx++)
	{
		if(SkeletalMesh->ClothingAssets[AssetIdx].ClothVisualizationInfos.Num() == 0)
		{
			LoadClothingVisualizationInfo(AssetIdx);
		}

		// if this asset doesn't have current physical mesh LOD, then skip to visualize
		if(!SkeletalMesh->ClothingAssets[AssetIdx].ClothVisualizationInfos.IsValidIndex(PhysicalMeshLOD))
		{
			continue;
		}

		FClothVisualizationInfo& VisualInfo = SkeletalMesh->ClothingAssets[AssetIdx].ClothVisualizationInfos[PhysicalMeshLOD];

		int32 NumBackstopSpheres = VisualInfo.ClothConstrainCoeffs.Num();
		for(int32 BackstopIdx=0; BackstopIdx < NumBackstopSpheres; BackstopIdx++)
		{
			float Distance = VisualInfo.ClothConstrainCoeffs[BackstopIdx].ClothBackstopDistance;
			float MaxDistance = VisualInfo.ClothConstrainCoeffs[BackstopIdx].ClothMaxDistance;

			FColor FixedColor = FColor::White;
			if(Distance > MaxDistance) // Backstop is disabled if the value is bigger than its Max Distance value
			{
				Distance = 0.0f;
				FixedColor = FColor::Black;
			}

			if(Distance > 0.0f)
			{
				FVector LineStart = VisualInfo.ClothPhysicalMeshVertices[BackstopIdx];
				FVector LineEnd = LineStart + (VisualInfo.ClothPhysicalMeshNormals[BackstopIdx] * Distance);
				PDI->DrawLine(LineStart, LineEnd, FColor::Red, SDPG_World);
			}
			else
			if(Distance < 0.0f)
			{
				FVector LineStart = VisualInfo.ClothPhysicalMeshVertices[BackstopIdx];
				FVector LineEnd = LineStart + (VisualInfo.ClothPhysicalMeshNormals[BackstopIdx] * Distance);
				PDI->DrawLine(LineStart, LineEnd, FColor::Blue, SDPG_World);
			}
			else // White means collision distance is set to 0
			{
				FVector FixedPoint = VisualInfo.ClothPhysicalMeshVertices[BackstopIdx];
				PDI->DrawPoint(FixedPoint, FixedColor, 2, SDPG_World);
			}
		}
	}

#endif // #if WITH_APEX_CLOTHING
}

void USkeletalMeshComponent::DrawClothingPhysicalMeshWire(FPrimitiveDrawInterface* PDI)
{
#if WITH_APEX_CLOTHING
	if (!SkeletalMesh
		|| SkeletalMesh->ClothingAssets.Num() == 0) 
	{
		return;
	}

	int32 PhysicalMeshLOD = PredictedLODLevel;

	int32 NumAssets = SkeletalMesh->ClothingAssets.Num();

	TArray<FMatrix> RefToLocals;
	// get local matrices for skinning
	UpdateRefToLocalMatrices(RefToLocals, this, GetSkeletalMeshResource(), 0, NULL);

	for(int32 AssetIdx=0; AssetIdx < NumAssets; AssetIdx++)
	{
		if(SkeletalMesh->ClothingAssets[AssetIdx].ClothVisualizationInfos.Num() == 0)
		{
			LoadClothingVisualizationInfo(AssetIdx);
		}

		// if this asset doesn't have current physical mesh LOD, then skip to visualize
		if(!SkeletalMesh->ClothingAssets[AssetIdx].ClothVisualizationInfos.IsValidIndex(PhysicalMeshLOD))
		{
			continue;
		}

		FClothVisualizationInfo& VisualInfo = SkeletalMesh->ClothingAssets[AssetIdx].ClothVisualizationInfos[PhysicalMeshLOD];

		uint32 NumPhysicalMeshVerts = VisualInfo.ClothPhysicalMeshVertices.Num();

		bool bUseSimulatedResult = false;
		// skinning for fixed vertices
		TArray<FVector> SimulatedPhysicalMeshVertices;
		// if cloth simulation is enabled and there exist a clothing actor and simulation vertices, 
		// then draws wire frame using simulated vertices
		if(!bDisableClothSimulation && ClothingActors.IsValidIndex(AssetIdx))
		{
			NxClothingActor* ClothingActor = ClothingActors[AssetIdx].ApexClothingActor;

			if(ClothingActor)
			{
				uint32 NumSimulVertices = ClothingActor->getNumSimulationVertices();

				if(NumSimulVertices > 0)
				{
					// if # of simulated vertices is bigger than loaded info, it will be LOD transition period 
					// just skip this tick because PredictedLODLevel is different from internal clothing LOD. it will be matched at next tick
					if (NumSimulVertices > NumPhysicalMeshVerts)
					{
						return;
					}

					bUseSimulatedResult = true;
					SimulatedPhysicalMeshVertices.AddUninitialized(NumPhysicalMeshVerts);

					const physx::PxVec3* SimulVertices = ClothingActor->getSimulationPositions();
					for(uint32 SimulVertIdx=0; SimulVertIdx < NumSimulVertices; SimulVertIdx++)
					{
						SimulatedPhysicalMeshVertices[SimulVertIdx] = P2UVector(SimulVertices[SimulVertIdx]);
					}

					// skinning for fixed vertices
					for(uint32 FixedVertIdx=NumSimulVertices; FixedVertIdx < NumPhysicalMeshVerts; FixedVertIdx++)
					{
						FMatrix SkinningMat(ForceInitToZero);

						for(uint32 BoneWeightIdx=0; BoneWeightIdx < VisualInfo.NumMaxBoneInfluences; BoneWeightIdx++)
						{
							uint16 ApexBoneIndex = VisualInfo.ClothPhysicalMeshBoneWeightsInfo[FixedVertIdx].Indices[BoneWeightIdx];

							FName BoneName = GetConvertedBoneName(SkeletalMesh->ClothingAssets[AssetIdx].ApexClothingAsset, ApexBoneIndex);

							int32 BoneIndex = GetBoneIndex(BoneName);

							if(BoneIndex < 0)
							{
								continue;
							}

							float Weight = VisualInfo.ClothPhysicalMeshBoneWeightsInfo[FixedVertIdx].Weights[BoneWeightIdx];
							SkinningMat += RefToLocals[BoneIndex]*Weight;
						}

						SimulatedPhysicalMeshVertices[FixedVertIdx] = SkinningMat.TransformPosition(VisualInfo.ClothPhysicalMeshVertices[FixedVertIdx]);
					}
				}
			}
		}

		TArray<FVector>* PhysicalMeshVertices = &VisualInfo.ClothPhysicalMeshVertices;
		if(bUseSimulatedResult)
		{
			PhysicalMeshVertices = &SimulatedPhysicalMeshVertices;
		}

		uint32 NumIndices = VisualInfo.ClothPhysicalMeshIndices.Num();
		check(NumIndices % 3 == 0); // check triangles count

		for(uint32 IndexIdx=0; IndexIdx < NumIndices; IndexIdx+=3)
		{
			// draw a triangle
			FVector V[3];
			float MaxDists[3];
			{
				uint32 Index0 = VisualInfo.ClothPhysicalMeshIndices[IndexIdx];
				uint32 Index1 = VisualInfo.ClothPhysicalMeshIndices[IndexIdx + 1];
				uint32 Index2 = VisualInfo.ClothPhysicalMeshIndices[IndexIdx + 2];

				// If index is greater than Num of vertices, then skip 
				if(Index0 >= NumPhysicalMeshVerts 
				|| Index1 >= NumPhysicalMeshVerts 
				|| Index2 >= NumPhysicalMeshVerts)
				{
					continue;
				}

				V[0] = (*PhysicalMeshVertices)[Index0];
				V[1] = (*PhysicalMeshVertices)[Index1];
				V[2] = (*PhysicalMeshVertices)[Index2];

				MaxDists[0] = VisualInfo.ClothConstrainCoeffs[Index0].ClothMaxDistance;
				MaxDists[1] = VisualInfo.ClothConstrainCoeffs[Index1].ClothMaxDistance;
				MaxDists[2] = VisualInfo.ClothConstrainCoeffs[Index2].ClothMaxDistance;
			}

			for(int32 i=0; i<3; i++)
			{
				uint32 Index0 = i;
				uint32 Index1 = i+1;
				if(Index1 >= 3)
				{
					Index1 = 0;
				}

				// calculates gray scaled color
				uint8 GrayLevel0 = (uint8)((MaxDists[Index0] / VisualInfo.MaximumMaxDistance)*255.0f);
				uint8 GrayLevel1 = (uint8)((MaxDists[Index1] / VisualInfo.MaximumMaxDistance)*255.0f);

				uint8 GrayMidColor = (uint8)(((uint32)GrayLevel0 + (uint32)GrayLevel1)*0.5);
				FColor LineColor(GrayMidColor, GrayMidColor, GrayMidColor);
				if(GrayMidColor == 0)
				{
					LineColor = FColor::Magenta;
				}
				else
				{
					LineColor = FColor::White;
				}

				PDI->DrawLine(V[Index0], V[Index1], LineColor, SDPG_World);
			}
		}
	}

#endif // #if WITH_APEX_CLOTHING
}

void USkeletalMeshComponent::SetAllMassScale(float InMassScale)
{
	// Apply mass scale to each child body
	for(FBodyInstance* BI : Bodies)
	{
		if (BI->IsValidBodyInstance())
		{
			BI->SetMassScale(InMassScale);
		}
	}
}


float USkeletalMeshComponent::GetMass() const
{
	float Mass = 0.0f;
	for (int32 i=0; i < Bodies.Num(); ++i)
	{
		FBodyInstance* BI = Bodies[i];

		if (BI->IsValidBodyInstance())
		{
			Mass += BI->GetBodyMass();
		}
	}
	return Mass;
}

// blueprint callable methods 
float USkeletalMeshComponent::GetClothMaxDistanceScale()
{
#if WITH_APEX_CLOTHING
	return ClothMaxDistanceScale;
#else
	return 1.0f;
#endif// #if WITH_APEX_CLOTHING
}

void USkeletalMeshComponent::SetClothMaxDistanceScale(float Scale)
{
#if WITH_APEX_CLOTHING

	//this scale parameter is also used when new clothing actor is created
	ClothMaxDistanceScale = Scale;

	int32 NumActors = ClothingActors.Num();

	for(int32 ActorIdx=0; ActorIdx<NumActors; ActorIdx++)
	{
		// skip if ClothingActor is NULL or invalid
		if(!IsValidClothingActor(ActorIdx))
		{
			continue;
		}

		NxClothingActor* ClothingActor = ClothingActors[ActorIdx].ApexClothingActor;

		check(ClothingActor);

		NxParameterized::Interface* ActorDesc = ClothingActor->getActorDesc();

		verify(NxParameterized::setParamF32(*ActorDesc, "maxDistanceScale.Scale", Scale));
	}
#endif// #if WITH_APEX_CLOTHING
}


void USkeletalMeshComponent::ResetClothTeleportMode()
{
#if WITH_APEX_CLOTHING
	ClothTeleportMode = FClothingActor::Continuous;
#endif// #if WITH_APEX_CLOTHING
}

void USkeletalMeshComponent::ForceClothNextUpdateTeleport()
{
#if WITH_APEX_CLOTHING
	ClothTeleportMode = FClothingActor::Teleport;
#endif// #if WITH_APEX_CLOTHING
}

void USkeletalMeshComponent::ForceClothNextUpdateTeleportAndReset()
{
#if WITH_APEX_CLOTHING
	ClothTeleportMode = FClothingActor::TeleportAndReset;
#endif// #if WITH_APEX_CLOTHING
}

FTransform USkeletalMeshComponent::GetComponentTransformFromBodyInstance(FBodyInstance* UseBI)
{
	// undo root transform so that it only moves according to what actor itself suppose to move
	FTransform BodyTransform = UseBI->GetUnrealWorldTransform();
	return RootBodyData.TransformToRoot * BodyTransform;
}
#undef LOCTEXT_NAMESPACE
