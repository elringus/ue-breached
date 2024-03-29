// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MovieSceneTrack.h"
#include "MovieSceneMaterialTrack.generated.h"

/**
 * Structure representing the animated value of a scalar parameter.
 */
struct FScalarParameterNameAndValue
{
	/** Creates a new FScalarParameterAndValue with a parameter name and a value. */
	FScalarParameterNameAndValue( FName InParameterName, float InValue )
	{
		ParameterName = InParameterName;
		Value = InValue;
	}

	/** The name of the scalar parameter. */
	FName ParameterName;

	/** The animated value of the scalar parameter. */
	float Value;
};

/**
* Structure representing the animated value of a vector parameter.
*/
struct FVectorParameterNameAndValue
{
	/** Creates a new FVectorParameterAndValue with a parameter name and a value. */
	FVectorParameterNameAndValue( FName InParameterName, FLinearColor InValue )
	{
		ParameterName = InParameterName;
		Value = InValue;
	}

	/** The name of the vector parameter. */
	FName ParameterName;

	/** The animated value of the vector parameter. */
	FLinearColor Value;
};

/**
 * Handles manipulation of material parameters in a movie scene.
 */
UCLASS( MinimalAPI )
class UMovieSceneMaterialTrack : public UMovieSceneTrack
{
	GENERATED_UCLASS_BODY()

public:
	/** UMovieSceneTrack interface */
	virtual UMovieSceneSection* CreateNewSection() override;
	virtual void RemoveAllAnimationData() override;
	virtual bool HasSection( UMovieSceneSection* Section ) const override;
	virtual void AddSection( UMovieSceneSection* Section ) override;
	virtual void RemoveSection( UMovieSceneSection* Section ) override;
	virtual bool IsEmpty() const override;
	virtual TRange<float> GetSectionBoundaries() const override;
	virtual const TArray<UMovieSceneSection*>& GetAllSections() const override;

	/**
	 * Adds a scalar parameter key to the track. 
	 * @param ParameterName The name of the parameter to add a key for.
	 * @param Time The time to add the new key.
	 * @param The value for the new key.
	 */
	void MOVIESCENETRACKS_API AddScalarParameterKey( FName ParameterName, float Position, float Value );

	/**
	* Adds a Vector parameter key to the track.
	* @param ParameterName The name of the parameter to add a key for.
	* @param Time The time to add the new key.
	* @param The value for the new key.
	*/
	void MOVIESCENETRACKS_API AddVectorParameterKey( FName ParameterName, float Position, FLinearColor Value );

	/**
	 * Gets the animated values for this track.
	 * @param Position The playback position to use for evaluation.
	 * @param OutScalarValues An array of FScalarParameterNameAndValue objects representing each animated scalar parameter and it's animated value.
	 * @param OutVectorValues An array of FVectorParameterNameAndValue objects representing each animated vector parameter and it's animated value.
	 */
	void Eval( float Position, TArray<FScalarParameterNameAndValue>& OutScalarValues, TArray<FVectorParameterNameAndValue>& OutVectorValues ) const;

private:
	/** The sections owned by this track .*/
	UPROPERTY()
	TArray<UMovieSceneSection*> Sections;
};

/**
 * A material track which is specialized for animation materials which are owned by actor components.
 */
UCLASS( MinimalAPI )
class UMovieSceneComponentMaterialTrack : public UMovieSceneMaterialTrack
{
	GENERATED_UCLASS_BODY()

public:
	/** UMovieSceneTrack interface */
	virtual TSharedPtr<IMovieSceneTrackInstance> CreateInstance() override;
	virtual FName GetTrackName() const override { return TrackName; }

	/** Gets the index of the material in the component. */
	int32 GetMaterialIndex() const { return MaterialIndex; }

	/** Sets the index of the material in the component. */
	void SetMaterialIndex(int32 InMaterialIndex) 
	{
		MaterialIndex = InMaterialIndex;
		TrackName = *FString::Printf(TEXT("Material Element %i"), MaterialIndex);
	}

private:
	/** The name of this track .*/
	UPROPERTY()
	FName TrackName;

	/** The index of this material this track is animating. */
	UPROPERTY()
	int32 MaterialIndex;
};


