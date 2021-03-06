// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#pragma once

#include <memory>

#include "Core/HW/SI_Device.h"

#include "SFML/Network.hpp"

// GameBoy Advance "Link Cable"

u8 GetNumConnected();
int GetTransferTime(u8 cmd);
void GBAConnectionWaiter_Shutdown();

class GBASockServer
{
public:
	GBASockServer(int _iDeviceNumber);
	~GBASockServer();

	void Disconnect();

	void ClockSync();

	void Send(u8* si_buffer);
	int Receive(u8* si_buffer);

	//Dragonbane: Fake GBA
	int CreateFakeResponse(u8* si_buffer);

private:
	std::unique_ptr<sf::TcpSocket> client;
	std::unique_ptr<sf::TcpSocket> clock_sync;
	unsigned char send_data[5];
	unsigned char recv_data[5];

	u64 time_cmd_sent;
	u64 last_time_slice;
	u8 device_number;
	u8 cmd;
	bool booted;
};

class CSIDevice_GBA : public ISIDevice, private GBASockServer
{
public:
	CSIDevice_GBA(SIDevices device, int _iDeviceNumber);
	~CSIDevice_GBA();

	virtual int RunBuffer(u8* _pBuffer, int _iLength) override;
	virtual int TransferInterval() override;

	virtual bool GetData(u32& _Hi, u32& _Low) override { return false; }
	virtual void SendCommand(u32 _Cmd, u8 _Poll) override {}

	//Dragonbane: Savestate support
	virtual void DoState(PointerWrap& p) override;

private:
	u8 send_data[5];
	int num_data_received;
	u64 timestamp_sent;
	bool waiting_for_response;
};
