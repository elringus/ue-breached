// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "SlatePrivatePCH.h"


/**
 * Construct this widget
 *
 * @param	InArgs	The declaration data for this widget
 */
void SButton::Construct( const FArguments& InArgs )
{
	bIsPressed = false;

	// Text overrides button content. If nothing is specified, put an null widget in the button.
	// Null content makes the button enter a special mode where it will ask to be as big as the image used for its border.
	struct
	{
		TSharedRef<SWidget> operator()( const FArguments& InOpArgs ) const
		{
			if ((InOpArgs._Content.Widget == SNullWidget::NullWidget) && (InOpArgs._Text.IsBound() || !InOpArgs._Text.Get().IsEmpty()) )
			{
				return SNew(STextBlock)
					.Text( InOpArgs._Text )
					.TextStyle( InOpArgs._TextStyle );
			}
			else
			{
				return InOpArgs._Content.Widget;
			}
		}
	} DetermineContent; 

	SBorder::Construct( SBorder::FArguments()
		.ContentScale(InArgs._ContentScale)
		.DesiredSizeScale(InArgs._DesiredSizeScale)
		.BorderBackgroundColor(InArgs._ButtonColorAndOpacity)
		.ForegroundColor(InArgs._ForegroundColor)
		.BorderImage( this, &SButton::GetBorder )
		.HAlign( InArgs._HAlign )
		.VAlign( InArgs._VAlign )
		.Padding( TAttribute<FMargin>(this, &SButton::GetCombinedPadding) )
		.ShowEffectWhenDisabled( TAttribute<bool>(this, &SButton::GetShowDisabledEffect) )
		[
			DetermineContent(InArgs)
		]
	);

	ContentPadding = InArgs._ContentPadding;

	SetButtonStyle(InArgs._ButtonStyle);

	bIsFocusable = InArgs._IsFocusable;

	OnClicked = InArgs._OnClicked;
	OnPressed = InArgs._OnPressed;
	OnReleased = InArgs._OnReleased;

	ClickMethod = InArgs._ClickMethod;
	TouchMethod = InArgs._TouchMethod;
	PressMethod = InArgs._PressMethod;

	HoveredSound = InArgs._HoveredSoundOverride.Get(Style->HoveredSlateSound);
	PressedSound = InArgs._PressedSoundOverride.Get(Style->PressedSlateSound);
}

FMargin SButton::GetCombinedPadding() const
{
	return ( IsPressed() )
		? ContentPadding.Get() + PressedBorderPadding
		: ContentPadding.Get() + BorderPadding;	
}

bool SButton::GetShowDisabledEffect() const
{
	return DisabledImage->DrawAs == ESlateBrushDrawType::NoDrawType;
}

/** @return An image that represents this button's border*/
const FSlateBrush* SButton::GetBorder() const
{
	if (!GetShowDisabledEffect() && !IsEnabled())
	{
		return DisabledImage;
	}
	else if (IsPressed())
	{
		return PressedImage;
	}
	else if (IsHovered())
	{
		return HoverImage;
	}
	else
	{
		return NormalImage;
	}
}


bool SButton::SupportsKeyboardFocus() const
{
	// Buttons are focusable by default
	return bIsFocusable;
}

void SButton::OnFocusLost( const FFocusEvent& InFocusEvent )
{
	Release();
}

FReply SButton::OnKeyDown( const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent )
{
	FReply Reply = FReply::Unhandled();
	if (IsEnabled() && (InKeyEvent.GetKey() == EKeys::Enter || InKeyEvent.GetKey() == EKeys::SpaceBar || InKeyEvent.GetKey() == EKeys::Gamepad_FaceButton_Bottom))
	{
		Press();

		if (PressMethod == EButtonPressMethod::ButtonPress)
		{
			//execute our "OnClicked" delegate, and get the reply
			Reply = OnClicked.IsBound() ? OnClicked.Execute() : FReply::Handled();

			//You should ALWAYS handle the OnClicked event.
			ensure(Reply.IsEventHandled() == true);
		}
		else
		{
			Reply = FReply::Handled();
		}
	}
	else
	{
		Reply = SBorder::OnKeyDown(MyGeometry, InKeyEvent);
	}

	//return the constructed reply
	return Reply;
}

FReply SButton::OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	FReply Reply = FReply::Unhandled();

	if (IsEnabled() && (InKeyEvent.GetKey() == EKeys::Enter || InKeyEvent.GetKey() == EKeys::SpaceBar || InKeyEvent.GetKey() == EKeys::Gamepad_FaceButton_Bottom))
	{
		const bool bWasPressed = bIsPressed;

		Release();

		//@Todo Slate: This should check focus, however we don't have that API yet, will be easier when focus is unified.
		if ( PressMethod == EButtonPressMethod::ButtonRelease || ( PressMethod == EButtonPressMethod::DownAndUp && bWasPressed ) )
		{
			//execute our "OnClicked" delegate, and get the reply
			Reply = OnClicked.IsBound() ? OnClicked.Execute() : FReply::Handled();

			//You should ALWAYS handle the OnClicked event.
			ensure(Reply.IsEventHandled() == true);
		}
		else
		{
			Reply = FReply::Handled();
		}
	}

	//return the constructed reply
	return Reply;
}

FReply SButton::OnMouseButtonDown( const FGeometry& MyGeometry, const FPointerEvent& MouseEvent )
{
	FReply Reply = FReply::Unhandled();
	if (IsEnabled() && (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton || MouseEvent.IsTouchEvent()))
	{
		Press();
		
		if( ClickMethod == EButtonClickMethod::MouseDown )
		{
			//get the reply from the execute function
			Reply = OnClicked.IsBound() ? OnClicked.Execute() : FReply::Handled();

			//You should ALWAYS handle the OnClicked event.
			ensure(Reply.IsEventHandled() == true);
		}
		else if ( IsPreciseTapOrClick(MouseEvent) )
		{
			// do not capture the pointer for precise taps or clicks
			// 
		}
		else
		{
			//we need to capture the mouse for MouseUp events
			Reply =  FReply::Handled().CaptureMouse( AsShared() );
		}
	}

	Invalidate(EInvalidateWidget::Layout);

	//return the constructed reply
	return Reply;
}


FReply SButton::OnMouseButtonDoubleClick( const FGeometry& InMyGeometry, const FPointerEvent& InMouseEvent )
{
	return OnMouseButtonDown( InMyGeometry, InMouseEvent );
}

FReply SButton::OnMouseButtonUp( const FGeometry& MyGeometry, const FPointerEvent& MouseEvent )
{
	FReply Reply = FReply::Unhandled();
	if ( bIsPressed && IsEnabled() && ( MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton || MouseEvent.IsTouchEvent() ) )
	{
		Release();

		if( ClickMethod == EButtonClickMethod::MouseDown )
		{
			// NOTE: If we're configured to click on mouse-down/precise-tap, then we never capture the mouse thus
			//       may never receive an OnMouseButtonUp() call.  We make sure that our bIsPressed
			//       state is reset by overriding OnMouseLeave().
		}
		else
		{
			const bool bIsUnderMouse = MyGeometry.IsUnderLocation(MouseEvent.GetScreenSpacePosition());
			if( bIsUnderMouse )
			{
				// If we asked for a precise tap, all we need is for the user to have not moved their pointer very far.
				const bool bTriggerForTouchEvent = IsPreciseTapOrClick(MouseEvent);

				// If we were asked to allow the button to be clicked on mouse up, regardless of whether the user
				// pressed the button down first, then we'll allow the click to proceed without an active capture
				const bool bTriggerForMouseEvent = ( ClickMethod == EButtonClickMethod::MouseUp || HasMouseCapture() );

				if( (bTriggerForTouchEvent || bTriggerForMouseEvent) && OnClicked.IsBound() == true )
				{
					Reply = OnClicked.Execute();
				}
			}
		}
		
		//If the user of the button didn't handle this click, then the button's
		//default behavior handles it.
		if ( Reply.IsEventHandled() == false )
		{
			Reply = FReply::Handled();
		}

		//If the user hasn't requested a new mouse captor, then the default
		//behavior of the button is to release mouse capture.
		if ( Reply.GetMouseCaptor().IsValid() == false )
		{
			Reply.ReleaseMouseCapture();
		}
	}

	Invalidate(EInvalidateWidget::Layout);

	return Reply;
}

FReply SButton::OnMouseMove( const FGeometry& MyGeometry, const FPointerEvent& MouseEvent )
{
	const float SlateDragStartDistance = FSlateApplication::Get().GetDragTriggerDistance();
	if ( IsPreciseTapOrClick(MouseEvent) && MouseEvent.GetCursorDelta().SizeSquared() > ( SlateDragStartDistance*SlateDragStartDistance ) )
	{
		Release();
	}
	return FReply::Unhandled();
}

void SButton::OnMouseEnter( const FGeometry& MyGeometry, const FPointerEvent& MouseEvent )
{
	if ( IsEnabled() )
	{
		PlayHoverSound();
	}
	
	SBorder::OnMouseEnter( MyGeometry, MouseEvent );

	Invalidate(EInvalidateWidget::Layout);
}

void SButton::OnMouseLeave( const FPointerEvent& MouseEvent )
{
	// Call parent implementation
	SWidget::OnMouseLeave( MouseEvent );

	// If we're setup to click on mouse-down, then we never capture the mouse and may not receive a
	// mouse up event, so we need to make sure our pressed state is reset properly here
	if ( ClickMethod == EButtonClickMethod::MouseDown || IsPreciseTapOrClick(MouseEvent) )
	{
		Release();
	}

	Invalidate(EInvalidateWidget::Layout);
}

void SButton::Press()
{
	if ( !bIsPressed )
	{
		bIsPressed = true;

		PlayPressedSound();

		OnPressed.ExecuteIfBound();
	}
}

void SButton::Release()
{
	if ( bIsPressed )
	{
		bIsPressed = false;
		OnReleased.ExecuteIfBound();
	}
}

bool SButton::IsInteractable() const
{
	return IsEnabled() && SupportsKeyboardFocus();
}

bool SButton::IsPreciseTapOrClick(const FPointerEvent& MouseEvent) const
{
	return ( TouchMethod == EButtonTouchMethod::PreciseTap && MouseEvent.IsTouchEvent() ) ||
		   ( ClickMethod == EButtonClickMethod::PreciseClick && !MouseEvent.IsTouchEvent() );
}

void SButton::PlayPressedSound() const
{
	FSlateApplication::Get().PlaySound( PressedSound );
}

void SButton::PlayHoverSound() const
{
	FSlateApplication::Get().PlaySound( HoveredSound );
}

FVector2D SButton::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	// When there is no widget in the button, it sizes itself based on
	// the border image specified by the style.
	if (ChildSlot.GetWidget() == SNullWidget::NullWidget)
	{
		return GetBorder()->ImageSize;
	}
	else
	{
		return SBorder::ComputeDesiredSize(LayoutScaleMultiplier);
	}
}

void SButton::SetContentPadding(const TAttribute<FMargin>& InContentPadding)
{
	ContentPadding = InContentPadding;
}

void SButton::SetHoveredSound(TOptional<FSlateSound> InHoveredSound)
{
	HoveredSound = InHoveredSound.Get(Style->HoveredSlateSound);
}

void SButton::SetPressedSound(TOptional<FSlateSound> InPressedSound)
{
	PressedSound = InPressedSound.Get(Style->PressedSlateSound);
}

void SButton::SetOnClicked(FOnClicked InOnClicked)
{
	OnClicked = InOnClicked;
}

void SButton::SetButtonStyle(const FButtonStyle* ButtonStyle)
{
	/* Get pointer to the button style */
	Style = ButtonStyle;

	NormalImage = &Style->Normal;
	HoverImage = &Style->Hovered;
	PressedImage = &Style->Pressed;
	DisabledImage = &Style->Disabled;

	BorderPadding = Style->NormalPadding;
	PressedBorderPadding = Style->PressedPadding;

	HoveredSound = Style->HoveredSlateSound;
	PressedSound = Style->PressedSlateSound;
}
