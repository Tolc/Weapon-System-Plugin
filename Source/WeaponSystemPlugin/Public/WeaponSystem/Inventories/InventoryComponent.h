﻿// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "InventoryComponent.generated.h"

UINTERFACE(MinimalAPI, BlueprintType, Blueprintable)
class UInventoryInterface : public UInterface
{
	GENERATED_BODY()
};

class WEAPONSYSTEMPLUGIN_API IInventoryInterface
{
	GENERATED_BODY()
protected:
	// Returns the inventory, must be overridden.
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Meta = (AllowPrivateAccess = "true"), Category = "Inventory Interface")
	class UInventoryComponent* GetInventory() const;
	virtual FORCEINLINE class UInventoryComponent* GetInventory_Implementation() const
	{
		LowLevelFatalError(TEXT("IInventoryInterface::GetInventory_Implementation not implemented"));
		return nullptr;
	}
};


UCLASS(BlueprintType, Blueprintable, ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class WEAPONSYSTEMPLUGIN_API UInventoryComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	// Sets default values for this component's properties
	UInventoryComponent();

protected:
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual bool ReplicateSubobjects(UActorChannel* Channel, FOutBunch* Bunch, FReplicationFlags* RepFlags) override;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Replicated, Meta = (AllowPrivateAccess = "true"), Category = "Inventory")
	TArray<class AWeaponBase*> Weapons;

	UPROPERTY(EditDefaultsOnly, Category = "Configurations")
	TArray<TSubclassOf<class AWeaponBase>> DefaultWeapons;

public:
	// NOTE: Does not check the validity of the new weapon by default. Only call on server.
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, BlueprintAuthorityOnly, Category = "Inventory")
	void AddWeapon(class AWeaponBase* NewWeapon);
	virtual void AddWeapon_Implementation(class AWeaponBase* NewWeapon);
	
	// NOTE: Does not check the validity of the new weapon by default. Only call on server.
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, BlueprintAuthorityOnly, Category = "Inventory")
	void RemoveWeapon(class AWeaponBase* RemoveWeapon);
	virtual void RemoveWeapon_Implementation(class AWeaponBase* RemoveWeapon);

	// Only call on server.
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, BlueprintAuthorityOnly, Category = "Inventory")
	void RemoveWeaponAt(const int32 Index);
	virtual void RemoveWeaponAt_Implementation(const int32 Index);
	


	
	UFUNCTION(BlueprintPure)
	virtual FORCEINLINE bool HasAuthority() const { return GetOwner() && GetOwner()->HasAuthority(); }

	UFUNCTION(BlueprintPure)
	virtual FORCEINLINE bool IsLocallyControlled() const
	{
		const APawn* OwnerPawn = GetOwner<APawn>();
		return OwnerPawn && OwnerPawn->IsLocallyControlled();
	}

	// Interface: Character Inventory returns the actual current weapon
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Inventory")
	class AWeaponBase* GetCurrentWeapon() const;

	template<typename T>
	FORCEINLINE T* GetCurrentWeapon() const { return Cast<T>(GetCurrentWeapon()); }

protected:
	virtual FORCEINLINE class AWeaponBase* GetCurrentWeapon_Implementation() const { return nullptr; }
};
