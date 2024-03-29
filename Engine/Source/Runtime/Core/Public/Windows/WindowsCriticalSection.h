// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#pragma once

/**
 * This is the Windows version of a critical section. It uses an aggregate
 * CRITICAL_SECTION to implement its locking.
 */
class FWindowsCriticalSection
{
	/**
	 * The windows specific critical section
	 */
	CRITICAL_SECTION CriticalSection;

public:

	/**
	 * Constructor that initializes the aggregated critical section
	 */
	FORCEINLINE FWindowsCriticalSection()
	{
#if USING_CODE_ANALYSIS
	MSVC_PRAGMA( warning( suppress : 28125 ) )	// warning C28125: The function 'InitializeCriticalSection' must be called from within a try/except block:  The requirement might be conditional.
#endif	// USING_CODE_ANALYSIS
		InitializeCriticalSection(&CriticalSection);
		SetCriticalSectionSpinCount(&CriticalSection,4000);
	}

	/**
	 * Destructor cleaning up the critical section
	 */
	FORCEINLINE ~FWindowsCriticalSection()
	{
		DeleteCriticalSection(&CriticalSection);
	}

	/**
	 * Locks the critical section
	 */
	FORCEINLINE void Lock()
	{
		// Spin first before entering critical section, causing ring-0 transition and context switch.
		if( TryEnterCriticalSection(&CriticalSection) == 0 )
		{
			EnterCriticalSection(&CriticalSection);
		}
	}

	/**
	* Quick test for seeing if the lock is already being used.
	*/
	FORCEINLINE bool TryLock()
	{
		if (TryEnterCriticalSection(&CriticalSection))
		{
			LeaveCriticalSection(&CriticalSection);
			return true;
		};
		return false;
	}

	/**
	 * Releases the lock on the critical seciton
	 */
	FORCEINLINE void Unlock()
	{
		LeaveCriticalSection(&CriticalSection);
	}
};

/** System-Wide Critical Section for windows using mutex */
class CORE_API FWindowsSystemWideCriticalSection
{
public:
	/** Construct a named, system-wide critical section and attempt to get access/ownership of it */
	explicit FWindowsSystemWideCriticalSection(const FString& InName, FTimespan InTimeout = FTimespan::Zero());

	/** Destructor releases system-wide critical section if it is currently owned */
	~FWindowsSystemWideCriticalSection();

	/**
	 * Does the calling thread have ownership of the system-wide critical section?
	 *
	 * @return True if obtained. WARNING: Returns true for an owned but previously abandoned locks so shared resources can be in undetermined states. You must handle shared data robustly.
	 */
	bool IsValid() const;

	/** Releases system-wide critical section if it is currently owned */
	void Release();

private:
	FWindowsSystemWideCriticalSection(const FWindowsSystemWideCriticalSection&);
	FWindowsSystemWideCriticalSection& operator=(const FWindowsSystemWideCriticalSection&);

private:
	HANDLE Mutex;
};
