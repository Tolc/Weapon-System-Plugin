﻿// Copyright 2022, Gannet Markozen, All rights reserved


#include "WeaponSystem/AttributeSystem/AttributesComponent.h"

#include "Engine/ActorChannel.h"
#include "GameFramework/Character.h"
#include "Net/UnrealNetwork.h"
#include "WeaponSystem/AttributeSystem/AttributeFunctionLibrary.h"
#include "WeaponSystem/Character/NetworkPrediction/WeaponSystemPlayerController.h"


UAttributesComponent::UAttributesComponent()
{
	PrimaryComponentTick.bCanEverTick = false;

	SetIsReplicatedByDefault(true);
	SetIsReplicated(true);
}


void UAttributesComponent::BeginPlay()
{
	Super::BeginPlay();

	// Init all attributes' handles
	for(TFieldIterator<FStructProperty> Itr(GetClass()); Itr; ++Itr)
	{
		if(!Itr->Struct->IsChildOf(FAttribute::StaticStruct())) continue;
		FAttribute& Attr = *Itr->ContainerPtrToValuePtr<FAttribute>(this);
		Attr.Handle.Set(this, *Itr);
	}
}

void UAttributesComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// Replicate OwnedTags state
	DOREPLIFETIME(ThisClass, OwnedTags);
}


void UAttributesComponent::BindAllAttributesChanged(const FAttributeValueChangedUniDelegate& Delegate)
{
	for(TFieldIterator<FStructProperty> Itr(GetClass()); Itr; ++Itr)
	{
		if(!Itr->Struct->IsChildOf(FAttribute::StaticStruct())) continue;
		Itr->ContainerPtrToValuePtr<FAttribute>(this)->OnAttributeChanged.Add(Delegate);
	}
}

int32 UAttributesComponent::RemoveActiveEffectsByClass(const TSubclassOf<UAttributeEffect> Class, const bool bIncludeChildren)
{
	if(!Class) return 0;
	int32 NumRemoved = 0;
	for(int32 i = 0; i < ActiveEffects.Num(); i++)
	{
		if(bIncludeChildren ? !ActiveEffects[i]->Effect->IsChildOf(Class) : ActiveEffects[i]->Effect != Class) continue;
		Internal_RemoveActiveEffect(i, EEffectRemovalReason::ManualRemoval);
		NumRemoved++;
	}
	return NumRemoved;
}


bool UAttributesComponent::TryApplyEffect(const TSubclassOf<UAttributeEffect> Effect, const AActor* Instigator, FPolyStructHandle& Context)
{
	if(!Effect || !Effect.GetDefaultObject()->CanApplyEffect(this, Context)) return false;
	
#if WITH_EDITOR// Editor notification only
	if(Effect.GetDefaultObject()->GetRepCond() == EEffectRepCond::LocalPredicted && !HasAuthority() && !AWeaponSystemPlayerController::StaticGetOwningPlayerController((AActor*)Instigator))
	{
		UE_LOG(LogTemp, Error, TEXT("Try Apply Effect: Attempted to apply a locally-predicted effect on the Client with an invalid Instigator."
			"Instigator must be valid and have an owning Weapon System Player Controller"));
		return false;
	}
#endif// WITH_EDITOR
	
	if(HasAuthority())
	{
		Internal_ApplyEffect(Effect, Instigator, Context);
		return true;
	}

	switch(Effect.GetDefaultObject()->GetRepCond())
	{
	case EEffectRepCond::LocalOnly:
		Internal_ApplyEffect(Effect, Instigator, Context);
		break;
	case EEffectRepCond::ServerOnly:
		Server_ApplyEffect(Effect, Instigator, Context);
		break;
	case EEffectRepCond::LocalPredicted:
		LocalPredicted_ApplyEffect(Effect, Instigator, Context);
		break;
	}

	return true;
}

void UAttributesComponent::Internal_ApplyEffect(const TSubclassOf<UAttributeEffect> Effect, const AActor* Instigator, FPolyStructHandle& Context)
{
	const UAttributeEffect* EffectDefObj = Effect.GetDefaultObject();
	if(EffectDefObj->GetDurationType() == EEffectDuration::Instant)// Instant-effect simply calls modify and other functions for it's "lifespan"
	{
		EffectDefObj->OnEffectApplied(this, Context);
		EffectDefObj->ModifyAttributes(this, Context);
		EffectDefObj->OnEffectRemoved(this, Context, EEffectRemovalReason::LifespanEnd);
	}
	else// Latent-effect gets added to an array of active effects and calls modify every interval until removed via lifespan or manually
	{
		const TSharedPtr<FActiveEffect> ActiveEffect = MakeShared<FActiveEffect>(Effect, Context);
		ActiveEffects.Add(ActiveEffect);

		EffectDefObj->OnEffectApplied(this, ActiveEffect->Context);
		EffectDefObj->ModifyAttributes(this, ActiveEffect->Context);

		auto CallModifyAtIntervalLambda = [=](){ EffectDefObj->ModifyAttributes(this, ActiveEffect->Context); };

		const float Interval = EffectDefObj->IntervalDuration;
		GetWorld()->GetTimerManager().SetTimer(ActiveEffect->IntervalTimerHandle, CallModifyAtIntervalLambda, Interval, true);

		// If has lifespan
		if(EffectDefObj->GetDurationType() == EEffectDuration::ForDuration)
		{
			auto CallRemoveAtLifespanEndLambda = [=](){ Internal_RemoveActiveEffect(ActiveEffects.Find(ActiveEffect), EEffectRemovalReason::LifespanEnd); };
			
			// If Lifespan is perfectly divisible by Interval. Make Lifespan slightly
			// larger so it is ensured to be called after the Interval has ended
			float Lifespan = EffectDefObj->Lifespan;
			if(std::fmodf(Lifespan / Interval, 1) == 0) Lifespan = std::nextafterf(Lifespan, 1.f);
			GetWorld()->GetTimerManager().SetTimer(ActiveEffect->LifespanTimerHandle, CallRemoveAtLifespanEndLambda, Lifespan, false);
		}
	}
}

void UAttributesComponent::Internal_RemoveActiveEffect(const int32 Index, const EEffectRemovalReason Reason)
{
	if(!ActiveEffects.IsValidIndex(Index)) return;
	checkf(ActiveEffects[Index].IsValid() || ActiveEffects[Index]->GetEffect(), TEXT("Active Effect at %i is invalid???"), Index);

	const FPolyStructHandle& Context = ActiveEffects[Index]->Context;
	ActiveEffects[Index]->GetEffect().GetDefaultObject()->OnEffectRemoved(this, Context, Reason);

	GetWorld()->GetTimerManager().ClearTimer(ActiveEffects[Index]->IntervalTimerHandle);
	GetWorld()->GetTimerManager().ClearTimer(ActiveEffects[Index]->LifespanTimerHandle);
	
	ActiveEffects.RemoveAt(Index);
}






void UAttributesComponent::LocalPredicted_ApplyEffect(const TSubclassOf<UAttributeEffect> Effect, const AActor* Instigator, FPolyStructHandle& Context)
{
	Internal_ApplyEffect(Effect, Instigator, Context);

	const FEffectNetPredKey PredictionKey = MakePredictionKey();
	LocalPredictedEffects.Add(PredictionKey, ActiveEffects[ActiveEffects.Num() - 1]);// Add latest effect to map of locally predicted effects

	AWeaponSystemPlayerController::StaticCallRemoteFunctionOnObject((AActor*)Instigator, this, FindFunction(GET_FUNCTION_NAME_CHECKED(ThisClass, Server_ApplyEffect_LocalPredicted)),
		Effect.Get(), Instigator, Context, PredictionKey);
}

void UAttributesComponent::Server_ApplyEffect_LocalPredicted_Implementation(UClass* Effect, const AActor* Instigator, const FPolyStructHandle& Context, const FEffectNetPredKey PredictionKey)
{
	if(!Effect) PRINT(TEXT("Effect is NULL"));
	if(!Instigator) PRINT(TEXT("Instigator is NULL"));
	if(Context.IsEmpty()) PRINT(TEXT("Context is empty"));

	if(TryApplyEffect(Effect, Instigator, const_cast<FPolyStructHandle&>(Context)))
	{
		PRINT(TEXT("Success"));
		AWeaponSystemPlayerController::StaticCallRemoteFunctionOnObject(const_cast<AActor*>(Instigator), this, FindFunction(GET_FUNCTION_NAME_CHECKED(ThisClass, Client_ApplyEffect_LocalPredicted_Success)),
			Effect, Instigator, PredictionKey);
	}
	else
	{
		PRINT(TEXT("Failed"));
		AWeaponSystemPlayerController::StaticCallRemoteFunctionOnObject(const_cast<AActor*>(Instigator), this, FindFunction(GET_FUNCTION_NAME_CHECKED(ThisClass, Client_ApplyEffect_LocalPredicted_Fail)),
			Effect, Instigator, PredictionKey);

		const TArray<FAttributeHandle> Attributes = Effect->GetDefaultObject<UAttributeEffect>()->GetAllModAttributes(this);
		FAttributeValuePairs AttributeValues;
		AttributeValues.AttributeValues.SetNum(Attributes.Num());
		for(int32 i = 0; i < Attributes.Num(); i++)
		{
			AttributeValues.AttributeValues[i] = TPair<FAttributeHandle, float>(Attributes[i], Attributes[i]->GetValue());
		}
		Client_SyncAttributes(AttributeValues);
		AWeaponSystemPlayerController::StaticCallRemoteFunctionOnObject(const_cast<AActor*>(Instigator), this, FindFunction(GET_FUNCTION_NAME_CHECKED(ThisClass, Client_SyncAttributes)),
			AttributeValues);
	}
}

void UAttributesComponent::Client_ApplyEffect_LocalPredicted_Success_Implementation(UClass* Effect, const AActor* Instigator, const FEffectNetPredKey PredictionKey)
{
	const TWeakPtr<FActiveEffect>* LocalPredictedEffectPtr = LocalPredictedEffects.Find(PredictionKey);
	LocalPredictedEffects.Remove(PredictionKey);
	if(LocalPredictedEffects.IsEmpty()) ClearCurrentPredictionKey();
	if(!LocalPredictedEffectPtr || !LocalPredictedEffectPtr->IsValid()) return;

	const int32 LocalPredictedEffectIndex = ActiveEffects.Find(LocalPredictedEffectPtr->Pin());
	Internal_RemoveActiveEffect(LocalPredictedEffectIndex, EEffectRemovalReason::NetPredSuccess);
	UE_LOG(LogTemp, Log, TEXT("Locally predicted effect %s succeeded"), *Effect->GetName());
}

void UAttributesComponent::Client_ApplyEffect_LocalPredicted_Fail_Implementation(UClass* Effect, const AActor* Instigator, const FEffectNetPredKey PredictionKey)
{
	const TWeakPtr<FActiveEffect>* LocalPredictedEffectPtr = LocalPredictedEffects.Find(PredictionKey);
	LocalPredictedEffects.Remove(PredictionKey);
	if(LocalPredictedEffects.IsEmpty()) ClearCurrentPredictionKey();
	if(!LocalPredictedEffectPtr || !LocalPredictedEffectPtr->IsValid()) return;

	const int32 LocalPredictedEffectIndex = ActiveEffects.Find(LocalPredictedEffectPtr->Pin());
	Internal_RemoveActiveEffect(LocalPredictedEffectIndex, EEffectRemovalReason::NetPredFail);
	
	UE_LOG(LogTemp, Log, TEXT("Locally predicted effect %s failed"), *Effect->GetName());
}

void UAttributesComponent::Server_SyncAttributes_Implementation(const TArray<FAttributeHandle>& Attributes)
{
	FAttributeValuePairs AttributeValues;
	AttributeValues.AttributeValues.SetNum(Attributes.Num());
	for(int32 i = 0; i < Attributes.Num(); i++)
	{
		AttributeValues.AttributeValues[i] = TPair<FAttributeHandle, float>(Attributes[i], Attributes[i]->GetValue());
	}
	Client_SyncAttributes(AttributeValues);
}

void UAttributesComponent::Client_SyncAttributes_Implementation(const FAttributeValuePairs& AttributeValues)
{
	for(TPair<FAttributeHandle, float>& Value : const_cast<FAttributeValuePairs&>(AttributeValues).AttributeValues)
	{
		checkf(Value.Get<0>().IsValid(), TEXT("Attribute is invalid on Client"));
		Value.Get<0>()->SetValue(Value.Get<1>());
	}
}









void UAttributesComponent::ApplyInstantNumericEffect(const FName& AttributeName, const AActor* Instigator, const EEffectRepCond ReplicationCondition, const EEffectModType ModificationType, const float Magnitude)
{
	const FAttributeHandle Attribute = FindAttributeByName(AttributeName);
	if(!Attribute.IsValid()) return;
#if WITH_EDITOR
	if(ReplicationCondition != EEffectRepCond::LocalOnly && !HasAuthority() && !AWeaponSystemPlayerController::StaticGetOwningPlayerController((AActor*)Instigator))
	{
		UE_LOG(LogTemp, Error, TEXT("Apply Numeric Effect: Attempted to apply a locally-predicted effect on the Client with an invalid Instigator."
			"Instigator must be valid and have an owning Weapon System Player Controller"));
		return;
	}
#endif// WITH_EDITOR

	if(HasAuthority())
	{
		Internal_ApplyInstantNumericEffect(Attribute, ModificationType, Magnitude);
		return;
	}

	switch(ReplicationCondition)
	{
	case EEffectRepCond::LocalOnly:
		Internal_ApplyInstantNumericEffect(Attribute, ModificationType, Magnitude);
		break;
	case EEffectRepCond::ServerOnly:
		AWeaponSystemPlayerController::StaticCallRemoteFunctionOnObject((AActor*)Instigator, this, FindFunction(GET_FUNCTION_NAME_CHECKED(ThisClass, Net_ApplyInstantNumericEffect)),
			FInstantNumericEffectNetValue(Attribute, ModificationType, Magnitude));
		break;
	case EEffectRepCond::LocalPredicted:
		AWeaponSystemPlayerController::StaticCallRemoteFunctionOnObject((AActor*)Instigator, this, FindFunction(GET_FUNCTION_NAME_CHECKED(ThisClass, Net_ApplyInstantNumericEffect)),
			FInstantNumericEffectNetValue(Attribute, ModificationType, Magnitude));
		Internal_ApplyInstantNumericEffect(Attribute, ModificationType, Magnitude);
		break;
	}
}

void UAttributesComponent::Internal_ApplyInstantNumericEffect(const FAttributeHandle& Attribute, const EEffectModType ModType, const float Magnitude)
{
	checkf(Attribute.IsValid(), TEXT("Attribute parameter is invalid on %s"), *FString(HasAuthority() ? "SERVER" : "CLIENT"));
	float NewValue = Attribute->GetValue();
	switch(ModType)
	{
	case EEffectModType::Additive:
		NewValue += Magnitude;
		break;
	case EEffectModType::Multiplicative:
		NewValue *= Magnitude;
		break;
	case EEffectModType::Overriding:
		NewValue = Magnitude;
		break;
	default:
		break;
	}
	
	const_cast<FAttributeHandle&>(Attribute)->SetValue(NewValue);
}


void UAttributesComponent::OnRep_OwnedTags()
{
	UE_LOG(LogTemp, Warning, TEXT("%s: Tags == %s"), *FString(__FUNCTION__), *OwnedTags.ToString());
}






















