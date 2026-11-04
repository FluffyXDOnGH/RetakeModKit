// Copyright 2026 Codnyx. All Rights Reserved.
#include "RetakeMapSDKIds.h"

const TArray<FName>& RetakeMapSDKIds::DevicePresentations()
{
	static const TArray<FName> Ids = { FieldUplink };
	return Ids;
}

const TArray<FName>& RetakeMapSDKIds::Weapons()
{
	static const TArray<FName> Ids = {
		TEXT("ak47"), TEXT("ak74u"), TEXT("atxmil"), TEXT("flash_grenade"), TEXT("g17"), TEXT("m24"), TEXT("m4a1"),
		TEXT("m590a1"), TEXT("mp5"), TEXT("molotov"), TEXT("p320"), TEXT("ump45"), TEXT("usp45") };
	return Ids;
}

const TArray<FName>& RetakeMapSDKIds::BoostProfiles()
{
	static const TArray<FName> Ids = { StandardBoost };
	return Ids;
}

TArray<FName> URetakeMapSDKIdOptions::GetDevicePresentationIds()
{
	return RetakeMapSDKIds::DevicePresentations();
}

TArray<FName> URetakeMapSDKIdOptions::GetWeaponIds()
{
	return RetakeMapSDKIds::Weapons();
}

TArray<FName> URetakeMapSDKIdOptions::GetBoostProfileIds()
{
	return RetakeMapSDKIds::BoostProfiles();
}
