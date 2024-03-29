// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "CoreUObjectPrivate.h"

/*-----------------------------------------------------------------------------
	Helper functions.
-----------------------------------------------------------------------------*/
namespace
{
	FORCEINLINE bool IsCorePackage(const FName& PackageName)
	{
		return PackageName == NAME_Core || PackageName == GLongCorePackageName;
	}
}

/*-----------------------------------------------------------------------------
	FObjectResource
-----------------------------------------------------------------------------*/

FObjectResource::FObjectResource()
{}

FObjectResource::FObjectResource( UObject* InObject )
:	ObjectName		( InObject ? InObject->GetFName() : FName(NAME_None)		)
{
}

/*-----------------------------------------------------------------------------
	FObjectExport.
-----------------------------------------------------------------------------*/

FObjectExport::FObjectExport()
: FObjectResource()
, ObjectFlags(RF_NoFlags)
, SerialSize(0)
, SerialOffset(0)
, ScriptSerializationStartOffset(0)
, ScriptSerializationEndOffset(0)
, Object(NULL)
, HashNext(INDEX_NONE)
, bForcedExport(false)
, bNotForClient(false)
, bNotForServer(false)
, bNotForEditorGame(true)
, bIsAsset(false)
, bExportLoadFailed(false)
, PackageGuid(FGuid(0, 0, 0, 0))
, PackageFlags(0)
{}

FObjectExport::FObjectExport( UObject* InObject )
: FObjectResource(InObject)
, ObjectFlags(InObject ? InObject->GetMaskedFlags() : RF_NoFlags)
, SerialSize(0)
, SerialOffset(0)
, ScriptSerializationStartOffset(0)
, ScriptSerializationEndOffset(0)
, Object(InObject)
, HashNext(INDEX_NONE)
, bForcedExport(false)
, bNotForClient(false)
, bNotForServer(false)
, bNotForEditorGame(true)
, bIsAsset(false)
, bExportLoadFailed(false)
, PackageGuid(FGuid(0, 0, 0, 0))
, PackageFlags(0)
{
	if(Object)		
	{
		bNotForClient = Object->HasAnyMarks(OBJECTMARK_NotForClient);
		bNotForServer = Object->HasAnyMarks(OBJECTMARK_NotForServer);
		bNotForEditorGame = Object->HasAnyMarks(OBJECTMARK_NotForEditorGame);
		bIsAsset = Object->IsAsset();
	}
}

FArchive& operator<<( FArchive& Ar, FObjectExport& E )
{
	Ar << E.ClassIndex;
	Ar << E.SuperIndex;
	Ar << E.OuterIndex;
	Ar << E.ObjectName;

	uint32 Save = E.ObjectFlags & RF_Load;
	if (Ar.IsSaving() && E.bIsAsset)
	{
		// Add RF_AssetExport flag if this is the main asset in the package
		// This flag should never be set on the actual object
		Save |= RF_AssetExport;
	}
	Ar << Save;
	if (Ar.IsLoading())
	{
		// Remember if the export is the main asset in its package. RF_Load will mask RF_AssetExport flag out.
		E.bIsAsset = !!(Save & RF_AssetExport);
		E.ObjectFlags = EObjectFlags(Save & RF_Load);
	}

	Ar << E.SerialSize;
	Ar << E.SerialOffset;

	Ar << E.bForcedExport;
	Ar << E.bNotForClient;
	Ar << E.bNotForServer;

	Ar << E.PackageGuid;
	Ar << E.PackageFlags;

	if (Ar.UE4Ver() >= VER_UE4_LOAD_FOR_EDITOR_GAME)
	{
		Ar << E.bNotForEditorGame;
	}

	return Ar;
}

/*-----------------------------------------------------------------------------
	FObjectImport.
-----------------------------------------------------------------------------*/

FObjectImport::FObjectImport()
:	FObjectResource	()
{
}

FObjectImport::FObjectImport( UObject* InObject )
:	FObjectResource	( InObject																)
,	ClassPackage	( InObject ? InObject->GetClass()->GetOuter()->GetFName()	: NAME_None	)
,	ClassName		( InObject ? InObject->GetClass()->GetFName()				: NAME_None	)
,	XObject			( InObject																)
,	SourceLinker	( NULL																	)
,	SourceIndex		( INDEX_NONE															)
{
}

FArchive& operator<<( FArchive& Ar, FObjectImport& I )
{
	Ar << I.ClassPackage << I.ClassName;
	Ar << I.OuterIndex;
	Ar << I.ObjectName;
	if( Ar.IsLoading() )
	{
		I.SourceLinker	= NULL;
		I.SourceIndex	= INDEX_NONE;
		I.XObject		= NULL;
	}
	return Ar;
}