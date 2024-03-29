// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#pragma once
#include "K2Node.h"
#include "K2Node_GetClassDefaults.generated.h"

UCLASS(MinimalAPI)
class UK2Node_GetClassDefaults : public UK2Node
{
	GENERATED_UCLASS_BODY()

	// Begin UObject interface
	virtual void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;
	// End UObject interface

	// Begin UEdGraphNode interface
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual void AllocateDefaultPins() override;
	virtual void PostPlacedNewNode() override;
	virtual void PinDefaultValueChanged(UEdGraphPin* Pin) override;
	virtual void PinConnectionListChanged(UEdGraphPin* Pin) override;
	virtual void ValidateNodeDuringCompilation(class FCompilerResultsLog& MessageLog) const override;
	// End UEdGraphNode interface

	// Begin UK2Node interface
	virtual bool IsNodePure() const override { return true; }
	virtual bool ShouldShowNodeProperties() const override { return true; }
	virtual void ReallocatePinsDuringReconstruction(TArray<UEdGraphPin*>& OldPins) override;
	virtual bool HasExternalDependencies(TArray<class UStruct*>* OptionalOutput) const override;
	virtual class FNodeHandlingFunctor* CreateNodeHandler(class FKismetCompilerContext& CompilerContext) const override;
	virtual void ExpandNode(class FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual FText GetMenuCategory() const override;
	// End UK2Node interface

public:
	/** Finds and returns the class input pin from the current set of pins. */
	UEdGraphPin* FindClassPin() const
	{
		return FindClassPin(Pins);
	}

protected:
	/**
	 * Finds and returns the class input pin.
	 *
	 * @param FromPins	A list of pins to search.
	 */
	UEdGraphPin* FindClassPin(const TArray<UEdGraphPin*>& FromPins) const;

	/**
	 * Determines the input class type from the given pin.
	 *
	 * @param FromPin	Input class pin. If not set (default), then it will fall back to using FindClassPin().
	 */
	UClass* GetInputClass(const UEdGraphPin* FromPin = nullptr) const;

	/**
	 * Creates the full set of output pins (properties) from the given input class.
	 *
	 * @param InClass	Input class type.
	 */
	void CreateOutputPins(UClass* InClass);

	/** Will be called whenever the class pin selector changes its value. */
	void OnClassPinChanged();

private:
	/** Class pin name */
	static FString ClassPinName;

	/** Output pin visibility control */
	UPROPERTY(EditAnywhere, Category=PinOptions, EditFixedSize)
	TArray<FOptionalPinFromProperty> ShowPinForProperties;

	/** Whether or not to exclude object array properties */
	UPROPERTY()
	bool bExcludeObjectArrays;
};
