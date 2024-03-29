// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "MaterialShaderQualitySettingsPrivatePCH.h"
#include "MaterialShaderQualitySettings.h"
#include "ShaderPlatformQualitySettings.h"
#include "RHI.h"
#include "SecureHash.h"

#if WITH_EDITOR
#include "TargetPlatform.h"
#include "PlatformInfo.h"
#endif

UMaterialShaderQualitySettings* UMaterialShaderQualitySettings::RenderQualitySingleton = nullptr;

UMaterialShaderQualitySettings::UMaterialShaderQualitySettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

UMaterialShaderQualitySettings* UMaterialShaderQualitySettings::Get()
{
	if( RenderQualitySingleton == nullptr )
	{
		static const TCHAR* SettingsContainerName = TEXT("MaterialShaderQualitySettingsContainer");

		RenderQualitySingleton = FindObject<UMaterialShaderQualitySettings>(GetTransientPackage(), SettingsContainerName);

		if (RenderQualitySingleton == nullptr)
		{
			RenderQualitySingleton = NewObject<UMaterialShaderQualitySettings>(GetTransientPackage(), UMaterialShaderQualitySettings::StaticClass(), SettingsContainerName);
			RenderQualitySingleton->AddToRoot();
		}
		RenderQualitySingleton->CurrentPlatformSettings = RenderQualitySingleton->GetShaderPlatformQualitySettings(FPlatformProperties::PlatformName());

		// LegacyShaderPlatformToShaderFormat(EShaderPlatform)
		// GShaderPlatformForFeatureLevel
		// GMaxRHIFeatureLevel

		// populate shader platforms
		RenderQualitySingleton->CurrentPlatformSettings = RenderQualitySingleton->GetShaderPlatformQualitySettings(FPlatformProperties::PlatformName());

	}
	return RenderQualitySingleton;
}

#if WITH_EDITOR
const FName& UMaterialShaderQualitySettings::GetPreviewPlatform()
{
	return PreviewPlatformName;
}

void UMaterialShaderQualitySettings::SetPreviewPlatform(FName PlatformName)
{
	 UShaderPlatformQualitySettings** FoundPlatform = ForwardSettingMap.Find(PlatformName);
	 PreviewPlatformSettings = FoundPlatform == nullptr ? nullptr : *FoundPlatform;
	 PreviewPlatformName = PlatformName;
}

void UMaterialShaderQualitySettings::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
}

void UMaterialShaderQualitySettings::PostInitProperties()
{
	Super::PostInitProperties();
}

#endif

UShaderPlatformQualitySettings* UMaterialShaderQualitySettings::GetOrCreatePlatformSettings(FName PlatformName)
{
	auto* PlatformSettings = ForwardSettingMap.Find(PlatformName);
	if (PlatformSettings == nullptr)
	{
		FString ObjectName("ForwardShadingQuality_");
		PlatformName.AppendString(ObjectName);

		auto* ForwardQualitySettings = FindObject<UShaderPlatformQualitySettings>(this, *ObjectName);
		if (ForwardQualitySettings == nullptr)
		{
			FName ForwardSettingsName(*ObjectName);
			ForwardQualitySettings = NewObject<UShaderPlatformQualitySettings>(this, UShaderPlatformQualitySettings::StaticClass(), FName(*ObjectName));
			ForwardQualitySettings->LoadConfig();
		}

		return ForwardSettingMap.Add(PlatformName, ForwardQualitySettings);
	}
	return *PlatformSettings;
}

static const FName GetPlatformNameFromShaderPlatform(EShaderPlatform Platform)
{
	return LegacyShaderPlatformToShaderFormat(Platform);
}

const UShaderPlatformQualitySettings* UMaterialShaderQualitySettings::GetShaderPlatformQualitySettings(EShaderPlatform ShaderPlatform)
{
 #if WITH_EDITORONLY_DATA
	// TODO: discuss this, in order to preview render quality settings we override the 
	// requested platform's settings.
	// However we do not know if we are asking for the editor preview window (override able) or for thumbnails, cooking purposes etc.. (Must not override)
	// The code below 'works' because desktop platforms do not cook for ES2 preview.
	if (IsPCPlatform(ShaderPlatform) && IsES2Platform(ShaderPlatform))
	{
		// Can check this cant be cooked by iterating through target platforms and shader formats to ensure it's not covered.
		if (PreviewPlatformSettings != nullptr)
		{
			return PreviewPlatformSettings;
		}
	}
#endif
	return GetShaderPlatformQualitySettings(GetPlatformNameFromShaderPlatform(ShaderPlatform));

}

UShaderPlatformQualitySettings* UMaterialShaderQualitySettings::GetShaderPlatformQualitySettings(FName PlatformName)
{
	return GetOrCreatePlatformSettings(PlatformName);
}


//////////////////////////////////////////////////////////////////////////

UShaderPlatformQualitySettings::UShaderPlatformQualitySettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// high quality overrides are always enabled by default
	GetQualityOverrides(EMaterialQualityLevel::High).bEnableOverride = true;
}

#if WITH_EDITOR

void UShaderPlatformQualitySettings::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	SaveConfig(); // needed?
}

void UShaderPlatformQualitySettings::PostInitProperties()
{
	Super::PostInitProperties();
}
#endif

void UShaderPlatformQualitySettings::BuildHash(EMaterialQualityLevel::Type QualityLevel, FSHAHash& OutHash) const
{
	FSHA1 Hash;

	AppendToHashState(QualityLevel, Hash);

	Hash.Final();
	Hash.GetHash(&OutHash.Hash[0]);
}

void UShaderPlatformQualitySettings::AppendToHashState(EMaterialQualityLevel::Type QualityLevel, FSHA1& HashState) const
{
	const FMaterialQualityOverrides& QualityLevelOverrides = GetQualityOverrides(QualityLevel);
	HashState.Update((const uint8*)&QualityLevelOverrides, sizeof(QualityLevelOverrides));
}

