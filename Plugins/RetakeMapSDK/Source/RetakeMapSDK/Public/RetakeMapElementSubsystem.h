// Copyright 2026 Codnyx. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "RetakeMapElementSubsystem.generated.h"

/** Bindings made by URetakeMapElementSubsystem::Observe; pass back to Unobserve to stop observing before the world ends. */
struct FRetakeMapElementObservation
{
	FDelegateHandle Added;
	FDelegateHandle Removed;
	FDelegateHandle StateChanged;
};

/**
 * Registry of the SDK map elements that have begun play in one world on this machine (RetakeMapSDK seam, MKD-029): SDK elements,
 * or actors hosting an SDK element component, which registers its owner. Elements
 * register at the end of BeginPlay, unregister at the start of EndPlay and report state changes from their setters and OnReps;
 * the game observes them here instead of being called by the SDK. Local only: nothing replicates, so server-only actors appear
 * only on the server and replicated actors appear on a client when they arrive. Exists in game and PIE worlds (including replay
 * playback), never in editor worlds, where validators walk the level instead. Native delegates only, never Blueprint-assignable.
 * Game thread.
 */
UCLASS(MinimalAPI)
class URetakeMapElementSubsystem final : public UWorldSubsystem
{
	GENERATED_BODY()
public:
	DECLARE_MULTICAST_DELEGATE_OneParam(FElementEvent, AActor& /*Element*/);
	DECLARE_MULTICAST_DELEGATE_TwoParams(FElementStateEvent, AActor& /*Element*/, FName /*Channel*/);

	/** Null in worlds without elements (editor, preview). */
	static RETAKEMAPSDK_API URetakeMapElementSubsystem* Get(const UObject* WorldContext);

	/** SDK elements, or actors hosting an SDK element component. Idempotent; a no-op in worlds without the subsystem. */
	static RETAKEMAPSDK_API void Register(AActor& Element);
	static RETAKEMAPSDK_API void Unregister(AActor& Element);
	/** SDK elements, or actors hosting an SDK element component, after the new state is applied locally. Ignored until the
	 *  element is registered (OnReps can run first). */
	static RETAKEMAPSDK_API void NotifyStateChanged(AActor& Element, FName Channel);

	/** True while a replay loads a checkpoint or fast-forwards to a seek target: suppress one-shot effects, keep persistent state. */
	static RETAKEMAPSDK_API bool IsReconstructingReplay(const UWorld* World);

	/** Registered elements of type T, in registration order. Safe against registration changes made by Fn. */
	template<class T>
	void ForEach(TFunctionRef<void(T&)> Fn) const
	{
		const TArray<TWeakObjectPtr<AActor>> Snapshot = Elements;
		for (const TWeakObjectPtr<AActor>& Weak : Snapshot)
			if (T* Element = Cast<T>(Weak.Get())) Fn(*Element);
	}

	template<class T>
	int32 Num() const
	{
		int32 Count = 0;
		for (const TWeakObjectPtr<AActor>& Weak : Elements) Count += Cast<T>(Weak.Get()) ? 1 : 0;
		return Count;
	}

	/**
	 * Calls OnAdded for every registered T now, then for each one added later; OnRemoved and OnStateChanged follow the same filter.
	 * Callbacks stop when Owner is destroyed. Read the element's current state in OnAdded: late joiners never see the defaults.
	 */
	template<class T>
	FRetakeMapElementObservation Observe(UObject& Owner, TFunction<void(T&)> OnAdded, TFunction<void(T&)> OnRemoved = nullptr,
		TFunction<void(T&, FName)> OnStateChanged = nullptr)
	{
		FRetakeMapElementObservation Observation;
		if (OnAdded)
		{
			ForEach<T>([&OnAdded](T& Element) { OnAdded(Element); });
			Observation.Added = OnElementAdded.AddWeakLambda(&Owner, [OnAdded](AActor& Element)
				{ if (T* Typed = Cast<T>(&Element)) OnAdded(*Typed); });
		}
		if (OnRemoved)
			Observation.Removed = OnElementRemoved.AddWeakLambda(&Owner, [OnRemoved](AActor& Element)
				{ if (T* Typed = Cast<T>(&Element)) OnRemoved(*Typed); });
		if (OnStateChanged)
			Observation.StateChanged = OnElementStateChanged.AddWeakLambda(&Owner, [OnStateChanged](AActor& Element, FName Channel)
				{ if (T* Typed = Cast<T>(&Element)) OnStateChanged(*Typed, Channel); });
		return Observation;
	}

	RETAKEMAPSDK_API void Unobserve(FRetakeMapElementObservation& Observation);

	FElementEvent OnElementAdded;
	FElementEvent OnElementRemoved;
	FElementStateEvent OnElementStateChanged;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Deinitialize() override;

private:
	TArray<TWeakObjectPtr<AActor>> Elements;
};
