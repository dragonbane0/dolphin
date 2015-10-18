// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include <memory>
#include <mutex>
#include <queue>

#include "Common/CommonFuncs.h"
#include "Common/ChunkFile.h"
#include "Common/Flag.h"
#include "Common/Thread.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SI_Device.h"
#include "Core/HW/SI_DeviceGBA.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/Movie.h" //Dragonbane
#include "Core/HW/Memmap.h" //Dragonbane

#include "SFML/Network.hpp"

static std::thread connectionThread;
static std::queue<std::unique_ptr<sf::TcpSocket>> waiting_socks;
static std::queue<std::unique_ptr<sf::TcpSocket>> waiting_clocks;
static std::mutex cs_gba;
static std::mutex cs_gba_clk;
static u8 num_connected;

namespace { Common::Flag server_running; }

enum EJoybusCmds
{
	CMD_RESET = 0xff,
	CMD_STATUS = 0x00,
	CMD_READ = 0x14,
	CMD_WRITE = 0x15
};

const u64 BITS_PER_SECOND = 115200;
const u64 BYTES_PER_SECOND = BITS_PER_SECOND / 8;


//Dragonbane: Custom Tuner Stuff

static File::IOFile outputFile("C:\\DolphinExport\\GBA.log", "wb");
static u64 frameTarget = 0;

//Connection variables

static bool isEnabled = true;
static bool isConnecting = true;
static bool idlePhase = true;

static int globalConnectionPhase = 0;
static int localConnectionPhase = 0;

static u8 handShake1;
static u8 handShake2;
static u8 handShake3;
static u8 handShake4;

static int finalDataGlobalPhase = 0;


//Action variables
static bool isConnected = false;
static bool tingleRNG = false;
static bool inLoading = false;

static int actionPhase = 0;
static int actionDataMode = 0;


//Report
static int cyclesPerFrame = 0;
static int totalActionCyclesRequired = 0;
static int executionTime = 0;
static bool reportActive = false;
static u64 currentFrame = 0;
static int reportedActionID = 0;

//static bool nextRNGBlocking = true;

u8 GetNumConnected()
{
	int num_ports_connected = num_connected;
	if (num_ports_connected == 0)
		num_ports_connected = 1;

	return num_ports_connected;
}

// --- GameBoy Advance "Link Cable" ---

int GetTransferTime(u8 cmd)
{
	u64 bytes_transferred = 0;

	switch (cmd)
	{
	case CMD_RESET:
	case CMD_STATUS:
	{
		bytes_transferred = 4;
		break;
	}
	case CMD_READ:
	{
		bytes_transferred = 6;
		break;
	}
	case CMD_WRITE:
	{
		bytes_transferred = 1;
		break;
	}
	default:
	{
		bytes_transferred = 1;
		break;
	}
	}
	return (int)(bytes_transferred * SystemTimers::GetTicksPerSecond() / (GetNumConnected() * BYTES_PER_SECOND));
}

static void GBAConnectionWaiter()
{
	server_running.Set();

	Common::SetCurrentThreadName("GBA Connection Waiter");

	sf::TcpListener server;
	sf::TcpListener clock_server;

	// "dolphin gba"
	if (server.listen(0xd6ba) != sf::Socket::Done)
		return;

	// "clock"
	if (clock_server.listen(0xc10c) != sf::Socket::Done)
		return;

	server.setBlocking(false);
	clock_server.setBlocking(false);

	auto new_client = std::make_unique<sf::TcpSocket>();
	while (server_running.IsSet())
	{
		if (server.accept(*new_client) == sf::Socket::Done)
		{
			std::lock_guard<std::mutex> lk(cs_gba);
			waiting_socks.push(std::move(new_client));

			new_client = std::make_unique<sf::TcpSocket>();
		}
		if (clock_server.accept(*new_client) == sf::Socket::Done)
		{
			std::lock_guard<std::mutex> lk(cs_gba_clk);
			waiting_clocks.push(std::move(new_client));

			new_client = std::make_unique<sf::TcpSocket>();
		}

		Common::SleepCurrentThread(1);
	}
}

void GBAConnectionWaiter_Shutdown()
{
	server_running.Clear();
	if (connectionThread.joinable())
		connectionThread.join();
}

static bool GetAvailableSock(std::unique_ptr<sf::TcpSocket>& sock_to_fill)
{
	bool sock_filled = false;

	std::lock_guard<std::mutex> lk(cs_gba);

	if (!waiting_socks.empty())
	{
		sock_to_fill = std::move(waiting_socks.front());
		waiting_socks.pop();
		sock_filled = true;
	}

	return sock_filled;
}

static bool GetNextClock(std::unique_ptr<sf::TcpSocket>& sock_to_fill)
{
	bool sock_filled = false;

	std::lock_guard<std::mutex> lk(cs_gba_clk);

	if (!waiting_clocks.empty())
	{
		sock_to_fill = std::move(waiting_clocks.front());
		waiting_clocks.pop();
		sock_filled = true;
	}

	return sock_filled;
}

GBASockServer::GBASockServer(int _iDeviceNumber)
{
	if (!connectionThread.joinable())
		connectionThread = std::thread(GBAConnectionWaiter);

	cmd = 0;
	num_connected = 0;
	last_time_slice = 0;
	booted = false;
	device_number = _iDeviceNumber;
}

GBASockServer::~GBASockServer()
{
	Disconnect();
}

void GBASockServer::Disconnect()
{
	if (client)
	{
		num_connected--;
		client->disconnect();
		client = nullptr;
	}
	if (clock_sync)
	{
		clock_sync->disconnect();
		clock_sync = nullptr;
	}
	last_time_slice = 0;
	booted = false;
}

void GBASockServer::ClockSync()
{
	if (!clock_sync)
		if (!GetNextClock(clock_sync))
			return;

	u32 time_slice = 0;

	if (last_time_slice == 0)
	{
		num_connected++;
		last_time_slice = CoreTiming::GetTicks();
		time_slice = (u32)(SystemTimers::GetTicksPerSecond() / 60);
	}
	else
	{
		time_slice = (u32)(CoreTiming::GetTicks() - last_time_slice);
	}

	time_slice = (u32)((u64)time_slice * 16777216 / SystemTimers::GetTicksPerSecond());
	last_time_slice = CoreTiming::GetTicks();
	char bytes[4] = { 0, 0, 0, 0 };
	bytes[0] = (time_slice >> 24) & 0xff;
	bytes[1] = (time_slice >> 16) & 0xff;
	bytes[2] = (time_slice >> 8) & 0xff;
	bytes[3] = time_slice & 0xff;

	sf::Socket::Status status = clock_sync->send(bytes, 4);
	if (status == sf::Socket::Disconnected)
	{
		clock_sync->disconnect();
		clock_sync = nullptr;
	}
}

void GBASockServer::Send(u8* si_buffer)
{
	if (!client)
		if (!GetAvailableSock(client))
			return;

	for (int i = 0; i < 5; i++)
		send_data[i] = si_buffer[i ^ 3];

	cmd = (u8)send_data[0];

	//Logging of send data
	/*
	std::string output;

	if (cmd == CMD_STATUS)
	output = StringFromFormat("\n\n %d: STATUS (00) [> %02x]\n", Movie::g_currentFrame, (u8)send_data[4]);
	else if (cmd == CMD_READ)
	output = StringFromFormat("\n\n %d: READ (14) [> %02x]\n", Movie::g_currentFrame, (u8)send_data[4]);
	else if (cmd == CMD_WRITE)
	output = StringFromFormat("\n\n %d: WRITE (15) [> %02x %02x %02x %02x]\n", Movie::g_currentFrame,
	(u8)send_data[1], (u8)send_data[2],
	(u8)send_data[3], (u8)send_data[4]);
	else if (cmd == CMD_RESET)
	output = StringFromFormat("\n\n %d: RESET (ff) [> %02x]\n", Movie::g_currentFrame, (u8)send_data[4]);

	outputFile.WriteBytes(output.data(), output.size());
	*/

#ifdef _DEBUG
	NOTICE_LOG(SERIALINTERFACE, "%01d cmd %02x [> %02x%02x%02x%02x]",
		device_number,
		(u8)send_data[0], (u8)send_data[1], (u8)send_data[2],
		(u8)send_data[3], (u8)send_data[4]);
#endif

	client->setBlocking(false);
	sf::Socket::Status status;
	if (cmd == CMD_WRITE)
		status = client->send(send_data, sizeof(send_data));
	else
		status = client->send(send_data, 1);

	if (cmd != CMD_STATUS)
		booted = true;

	if (status == sf::Socket::Disconnected)
		Disconnect();

	time_cmd_sent = CoreTiming::GetTicks();
}

int GBASockServer::Receive(u8* si_buffer)
{
	if (!client)
		if (!GetAvailableSock(client))
			return 5;

	size_t num_received = 0;

	u64 transferTime = GetTransferTime((u8)send_data[0]);
	bool block = (CoreTiming::GetTicks() - time_cmd_sent) > transferTime;
	if (cmd == CMD_STATUS && !booted)
		block = false;

	if (block)
	{
		sf::SocketSelector Selector;
		Selector.add(*client);
		Selector.wait(sf::milliseconds(1000));
	}

	sf::Socket::Status recv_stat = client->receive(recv_data, sizeof(recv_data), num_received);
	if (recv_stat == sf::Socket::Disconnected)
	{
		Disconnect();
		return 5;
	}

	if (recv_stat == sf::Socket::NotReady)
		num_received = 0;

	if (num_received > sizeof(recv_data))
		num_received = sizeof(recv_data);

	if (num_received > 0)
	{
		//Logging of received data
		/*
		std::string output;

		if (num_received == 5)
		output = StringFromFormat("[< %02x %02x %02x %02x %02x]",
		(u8)recv_data[0], (u8)recv_data[1], (u8)recv_data[2],
		(u8)recv_data[3], (u8)recv_data[4]);
		else if (num_received == 4)
		output = StringFromFormat("[< %02x %02x %02x %02x]",
		(u8)recv_data[0], (u8)recv_data[1], (u8)recv_data[2],
		(u8)recv_data[3]);
		else if (num_received == 3)
		output = StringFromFormat("[< %02x %02x %02x]",
		(u8)recv_data[0], (u8)recv_data[1], (u8)recv_data[2]);
		else if (num_received == 2)
		output = StringFromFormat("[< %02x %02x]",
		(u8)recv_data[0], (u8)recv_data[1]);
		else if (num_received == 1)
		output = StringFromFormat("[< %02x]",
		(u8)recv_data[0]);

		outputFile.WriteBytes(output.data(), output.size());
		*/

		#ifdef _DEBUG
		if ((u8)send_data[0] == 0x00 || (u8)send_data[0] == 0xff)
		{
			WARN_LOG(SERIALINTERFACE, "%01d                              [< %02x%02x%02x%02x%02x] (%d)",
				device_number,
				(u8)recv_data[0], (u8)recv_data[1], (u8)recv_data[2],
				(u8)recv_data[3], (u8)recv_data[4],
				num_received);
		}
		else
		{
			ERROR_LOG(SERIALINTERFACE, "%01d                              [< %02x%02x%02x%02x%02x] (%d)",
				device_number,
				(u8)recv_data[0], (u8)recv_data[1], (u8)recv_data[2],
				(u8)recv_data[3], (u8)recv_data[4],
				num_received);
		}
		#endif

		for (int i = 0; i < 5; i++)
			si_buffer[i ^ 3] = recv_data[i];
	}

	return (int)num_received;
}


// Dragonbane: Fake GBA
int GBASockServer::CreateFakeResponse(u8* si_buffer)
{
	for (int i = 0; i < 5; i++)
		send_data[i] = si_buffer[i ^ 3];

	cmd = (u8)send_data[0];

	size_t num_received = 0;

	//Logging of send data
	/*
	std::string output;

	if (cmd == CMD_STATUS)
	output = StringFromFormat("\n\n %d: STATUS (00) [> %02x]\n", Movie::g_currentFrame, (u8)send_data[4]);
	else if (cmd == CMD_READ)
	output = StringFromFormat("\n\n %d: READ (14) [> %02x]\n", Movie::g_currentFrame, (u8)send_data[4]);
	else if (cmd == CMD_WRITE)
	output = StringFromFormat("\n\n %d: WRITE (15) [> %02x %02x %02x %02x]\n", Movie::g_currentFrame,
	(u8)send_data[1], (u8)send_data[2],
	(u8)send_data[3], (u8)send_data[4]);
	else if (cmd == CMD_RESET)
	output = StringFromFormat("\n\n %d: RESET (ff) [> %02x]\n", Movie::g_currentFrame, (u8)send_data[4]);

	outputFile.WriteBytes(output.data(), output.size());
	*/

	/*
	if (!nextRNGBlocking)
	{
	Movie::tunerExecuteID = 13;
	}
	if (frameTarget != 0)
	{
	if (frameTarget <= Movie::g_currentFrame)
	{
	std::string output;

	nextRNGBlocking = false;

	output = StringFromFormat("RNG: [< NO RESPONSE]\n");

	outputFile.WriteBytes(output.data(), output.size());
	}
	}
	if (cmd == CMD_WRITE && send_data[1] == 0x05 && send_data[3] == 0x00 && send_data[4] == 0x00)
	{
	std::string output;

	nextRNGBlocking = false;
	frameTarget = 0;

	output = StringFromFormat("RNG: [< %02x]\n",
	(u8)send_data[2]);

	if (send_data[2] == 0x12 || send_data[2] == 0x0c)
	PanicAlertT("WTF!");

	outputFile.WriteBytes(output.data(), output.size());
	}
	*/

	bool reportEnd = false;

	if (Movie::tunerExecuteID > 0 && Movie::tunerExecuteID < 18 || Movie::tunerExecuteID > 19)
	{
		if (!reportActive)
		{
			std::string output;
			output = StringFromFormat("\n%d: ACTION ID: %i\n", Movie::g_currentFrame, Movie::tunerExecuteID);

			outputFile.WriteBytes(output.data(), output.size());

			reportActive = true;
			reportedActionID = Movie::tunerExecuteID;

			totalActionCyclesRequired = 1;
			cyclesPerFrame = 1;
			executionTime = 0;
			currentFrame = Movie::g_currentFrame;
		}
		else
		{
			if (Movie::tunerExecuteID != reportedActionID)
			{
				reportEnd = true;
			}
			else
			{
				totalActionCyclesRequired = totalActionCyclesRequired + 1;

				if (currentFrame < Movie::g_currentFrame)
				{
					std::string output;
					output = StringFromFormat("%d: Frame Advance! %i cycles were executed during the last frame\n", Movie::g_currentFrame, cyclesPerFrame);

					outputFile.WriteBytes(output.data(), output.size());

					executionTime = executionTime + 1;
					cyclesPerFrame = 1;
					currentFrame = Movie::g_currentFrame;
				}
				else
				{
					cyclesPerFrame = cyclesPerFrame + 1;
				}
			}
		}
	}
	else
	{
		reportEnd = true;
	}

	if (reportEnd)
	{
		if (reportActive)
		{
			std::string output;

			if (executionTime == 0 || executionTime == 1)
				output = StringFromFormat("%d: ACTION COMPLETED! Cycles required: %i ; Total execution time: 1 frame\n", Movie::g_currentFrame, totalActionCyclesRequired);
			else
				output = StringFromFormat("%d: ACTION COMPLETED! Cycles required: %i ; Total execution time: %i frames\n", Movie::g_currentFrame, totalActionCyclesRequired, executionTime);


			outputFile.WriteBytes(output.data(), output.size());

			reportActive = false;
			reportedActionID = 0;
		}
	}


	if (isConnected)
	{
		if (cmd == CMD_RESET)
		{
			isConnected = false;
			idlePhase = true;
			isConnecting = true;
		}
	}
	else
	{
		if (!isConnecting)
		{
			idlePhase = true;
			isConnecting = true;
		}
	}


	//CONNECTION PHASE
	if (isConnecting)
	{
		if (idlePhase)
		{
			Movie::tunerStatus = 2;

			if (cmd == CMD_STATUS)
			{
				recv_data[0] = 0x00;
				recv_data[1] = 0x04;
				recv_data[2] = 0x08;

				num_received = 3;
			}
			if (cmd == CMD_RESET)
			{
				recv_data[0] = 0x00;
				recv_data[1] = 0x04;
				recv_data[2] = 0x08;

				num_received = 3;
				idlePhase = false;
				globalConnectionPhase = 0;
				Movie::tunerStatus = 3;
			}

		}
		else if (globalConnectionPhase == 0)
		{
			recv_data[0] = 0x00;
			recv_data[1] = 0x04;
			recv_data[2] = 0x18;

			num_received = 3;

			globalConnectionPhase = 1;
		}
		else if (globalConnectionPhase == 1)
		{
			recv_data[0] = 0xd3;
			recv_data[1] = 0xd1;
			recv_data[2] = 0xa9;
			recv_data[3] = 0xaf;
			recv_data[4] = 0x18;

			num_received = 5;

			globalConnectionPhase = 2;
			localConnectionPhase = 0;
		}
		else if (globalConnectionPhase == 2)
		{
			if (cmd == CMD_STATUS || cmd == CMD_RESET)
			{
				globalConnectionPhase = 0;
				idlePhase = true;

				recv_data[0] = 0x00;
				recv_data[1] = 0x04;
				recv_data[2] = 0x08;

				num_received = 3;
			}
			else if (cmd == CMD_READ)
			{
				recv_data[0] = 0x2e;
				recv_data[1] = 0xd8;
				recv_data[2] = 0xc1;
				recv_data[3] = 0xa5;
				recv_data[4] = 0x08;

				num_received = 5;

				globalConnectionPhase = 3;
			}
			else if (localConnectionPhase == 0)
			{
				recv_data[0] = 0x12;
				localConnectionPhase = 1;
				num_received = 1;
			}
			else if (localConnectionPhase == 1)
			{
				recv_data[0] = 0x22;
				localConnectionPhase = 2;
				num_received = 1;
			}
			else if (localConnectionPhase == 2)
			{
				recv_data[0] = 0x32;
				localConnectionPhase = 1;
				num_received = 1;
			}
		}
		else if (globalConnectionPhase == 3)
		{
			recv_data[0] = 0x00;
			recv_data[1] = 0x04;
			recv_data[2] = 0x00;

			num_received = 3;

			globalConnectionPhase = 4;
		}
		else if (globalConnectionPhase == 4)
		{
			recv_data[0] = 0x00;
			recv_data[1] = 0x04;
			recv_data[2] = 0x08;

			num_received = 3;

			globalConnectionPhase = 5;
		}
		else if (globalConnectionPhase == 5)
		{
			recv_data[0] = 0x47;
			recv_data[1] = 0x5a;
			recv_data[2] = 0x4c;
			recv_data[3] = 0x4a;
			recv_data[4] = 0x08;

			num_received = 5;

			globalConnectionPhase = 6;
		}
		else if (globalConnectionPhase == 6)
		{
			recv_data[0] = 0x02;
			num_received = 1;

			globalConnectionPhase = 7;
		}
		else if (globalConnectionPhase == 7)
		{
			if (cmd == CMD_STATUS)
			{
				recv_data[0] = 0x00;
				recv_data[1] = 0x04;
				recv_data[2] = 0x08;

				num_received = 3;
			}
			if (cmd == CMD_RESET)
			{
				recv_data[0] = 0x00;
				recv_data[1] = 0x04;
				recv_data[2] = 0x08;

				num_received = 3;

				globalConnectionPhase = 8;
			}
		}
		else if (globalConnectionPhase == 8)
		{
			recv_data[0] = 0x00;
			recv_data[1] = 0x04;
			recv_data[2] = 0x28;

			num_received = 3;
			globalConnectionPhase = 9;
			localConnectionPhase = 0;
		}
		else if (globalConnectionPhase == 9)
		{
			if (localConnectionPhase == 0) //Read
			{
				recv_data[0] = 0x47;
				recv_data[1] = 0x5a;
				recv_data[2] = 0x4c;
				recv_data[3] = 0x4a;
				recv_data[4] = 0x28;

				num_received = 5;
				localConnectionPhase = 1;
			}
			else if (localConnectionPhase == 1) //Status
			{
				recv_data[0] = 0x00;
				recv_data[1] = 0x04;
				recv_data[2] = 0x20;

				num_received = 3;
				localConnectionPhase = 2;
			}
			else if (localConnectionPhase == 2) //Write
			{
				recv_data[0] = 0x22;

				num_received = 1;
				localConnectionPhase = 3;
			}
			else if (localConnectionPhase == 3) //Status
			{
				recv_data[0] = 0x00;
				recv_data[1] = 0x04;
				recv_data[2] = 0x30;

				num_received = 3;
				localConnectionPhase = 4;
			}
			else if (localConnectionPhase == 4) //Write
			{
				recv_data[0] = 0x32;

				//Save Handshake Data
				handShake1 = send_data[1];
				handShake2 = send_data[2];
				handShake3 = send_data[3];
				handShake4 = send_data[4];

				num_received = 1;
				localConnectionPhase = 5;
			}
			else if (localConnectionPhase == 5) //Status
			{
				recv_data[0] = 0x00;
				recv_data[1] = 0x04;
				recv_data[2] = 0x38;

				num_received = 3;
				localConnectionPhase = 6;
			}
			else if (localConnectionPhase == 6) //Read
			{
				recv_data[0] = handShake1;
				recv_data[1] = handShake2;
				recv_data[2] = handShake3;
				recv_data[3] = handShake4;
				recv_data[4] = 0x38;

				num_received = 5;
				localConnectionPhase = 7;
			}
			else if (localConnectionPhase == 7) //Write
			{
				recv_data[0] = 0x32;

				num_received = 1;
				globalConnectionPhase = 10;

				localConnectionPhase = 0;
				finalDataGlobalPhase = 0;
			}

		}
		else if (globalConnectionPhase == 10)
		{
			if (localConnectionPhase == 0)
			{
				recv_data[0] = 0xfe;
				recv_data[1] = 0xfe;
				recv_data[2] = 0xfe;
				recv_data[3] = 0xfe;
				recv_data[4] = 0x38;

				num_received = 5;
				localConnectionPhase = 1;
			}
			else
			{
				if (cmd == CMD_WRITE)
				{
					recv_data[0] = 0x3a;
					num_received = 1;
				}
				else
				{
					if (localConnectionPhase == 1)
					{
						recv_data[0] = 0x04;
						recv_data[1] = 0x00;
						recv_data[2] = 0x00;
						recv_data[3] = 0x00;
						recv_data[4] = 0x38;

						num_received = 5;
						localConnectionPhase = 2;
					}
					else if (localConnectionPhase == 2)
					{
						recv_data[0] = 0x07;
						recv_data[1] = 0x00;
						recv_data[2] = 0x00;
						recv_data[3] = 0x00;
						recv_data[4] = 0x38;

						num_received = 5;
						localConnectionPhase = 3;
					}
					else if (localConnectionPhase == 3)
					{
						recv_data[0] = 0x08;
						recv_data[1] = 0x00;
						recv_data[2] = 0x00;
						recv_data[3] = 0x00;
						recv_data[4] = 0x38;

						num_received = 5;
						localConnectionPhase = 4;
					}
					else if (localConnectionPhase == 4)
					{
						recv_data[0] = 0x00;
						recv_data[1] = 0x00;
						recv_data[2] = 0x00;
						recv_data[3] = 0x00;
						recv_data[4] = 0x38;

						num_received = 5;
						localConnectionPhase = 5;
					}
					else if (localConnectionPhase == 5)
					{
						recv_data[0] = 0x08;
						recv_data[1] = 0x00;
						recv_data[2] = 0x08;
						recv_data[3] = 0x00;
						recv_data[4] = 0x38;

						num_received = 5;
						localConnectionPhase = 6;
					}
					else if (localConnectionPhase == 6)
					{
						recv_data[0] = 0x08;
						recv_data[1] = 0x00;
						recv_data[2] = 0x08;
						recv_data[3] = 0x00;
						recv_data[4] = 0x38;

						num_received = 5;
						globalConnectionPhase = 11;
						localConnectionPhase = 0;
					}
				}
			}
		}
		else if (globalConnectionPhase == 11)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x32;
				num_received = 1;
			}
			else
			{
				recv_data[0] = 0x08;
				recv_data[1] = 0x00;
				recv_data[2] = 0x08;
				recv_data[3] = 0x00;
				recv_data[4] = 0x30;

				num_received = 5;

				if (finalDataGlobalPhase == 0)
				{
					if (localConnectionPhase == 0)
					{
						if (send_data[1] == 0xfe && send_data[2] == 0xfe && send_data[3] == 0xfe && send_data[4] == 0x30) //0xdc
						{
							//localConnectionPhase = 1;

							recv_data[0] = 0xfe;
							recv_data[1] = 0xfe;
							recv_data[2] = 0xfe;
							recv_data[3] = 0xfe;
							recv_data[4] = 0x38;

							num_received = 5;

							finalDataGlobalPhase = 1;
							localConnectionPhase = 1;

							globalConnectionPhase = 10;

						}
					}
					/*
					else if (localConnectionPhase == 1)
					{
					if (send_data[1] == 0x09 && send_data[2] == 0x00 && send_data[3] == 0x00 && send_data[4] == 0x30) //0xdc
					{
					localConnectionPhase = 2;
					}
					else
					{
					localConnectionPhase = 0;
					}
					}
					else if (localConnectionPhase == 2)
					{
					if (send_data[1] == 0x03 && send_data[2] == 0x00 && send_data[3] == 0x00 && send_data[4] == 0x30) //0xdc
					{
					localConnectionPhase = 3;
					}
					else
					{
					localConnectionPhase = 0;
					}
					}
					else if (localConnectionPhase == 3)
					{
					localConnectionPhase = 4;
					}
					else if (localConnectionPhase == 4)
					{
					localConnectionPhase = 5;
					}
					else if (localConnectionPhase == 5)
					{
					localConnectionPhase = 6;
					}
					else if (localConnectionPhase == 6)
					{

					recv_data[0] = 0xfe;
					recv_data[1] = 0xfe;
					recv_data[2] = 0xfe;
					recv_data[3] = 0xfe;
					recv_data[4] = 0x38;

					num_received = 5;

					localConnectionPhase = 1;
					finalDataGlobalPhase = 1;
					localConnectionPhase = 0;
					finalData2 = false;
					finalData = true;
					}
					*/
				}
				else if (finalDataGlobalPhase == 1)
				{
					if (send_data[1] == 0x4b && send_data[2] == 0x11 && send_data[3] == 0x5d && send_data[4] == 0x30)
					{
						recv_data[0] = 0xfe;
						recv_data[1] = 0xfe;
						recv_data[2] = 0xfe;
						recv_data[3] = 0xfe;
						recv_data[4] = 0x38;

						num_received = 5;

						globalConnectionPhase = 12;
						localConnectionPhase = 0;
					}
				}
			}
		}
		else if (globalConnectionPhase == 12)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x3a;
				num_received = 1;
			}
			else
			{
				if (localConnectionPhase == 0)
				{
					recv_data[0] = 0x09;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 1;
				}
				else if (localConnectionPhase == 1)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					globalConnectionPhase = 13;
				}
			}
		}
		else if (globalConnectionPhase == 13)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x32;
				num_received = 1;
			}
			else
			{
				recv_data[0] = 0x00;
				recv_data[1] = 0x00;
				recv_data[2] = 0x00;
				recv_data[3] = 0x00;
				recv_data[4] = 0x30;

				num_received = 5;

				if (send_data[1] == 0xff && send_data[2] == 0xff && send_data[3] == 0xff && send_data[4] == 0x30)
				{
					recv_data[0] = 0xfe;
					recv_data[1] = 0xfe;
					recv_data[2] = 0xfe;
					recv_data[3] = 0xfe;
					recv_data[4] = 0x38;

					num_received = 5;

					globalConnectionPhase = 14;
					localConnectionPhase = 0;
				}
			}
		}
		else if (globalConnectionPhase == 14)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x3a;
				num_received = 1;
			}
			else
			{
				if (localConnectionPhase == 0)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 1;
				}
				else if (localConnectionPhase == 1)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					globalConnectionPhase = 15;
				}
			}
		}
		else if (globalConnectionPhase == 15)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x32;
				num_received = 1;
			}
			else
			{
				recv_data[0] = 0x00;
				recv_data[1] = 0x00;
				recv_data[2] = 0x00;
				recv_data[3] = 0x00;
				recv_data[4] = 0x30;

				num_received = 5;

				if (send_data[1] == 0xff && send_data[2] == 0xff && send_data[3] == 0xff && send_data[4] == 0x30)
				{
					globalConnectionPhase = 16;
				}
			}
		}
		else if (globalConnectionPhase == 16)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x3a;
				num_received = 1;
			}
			else
			{
				recv_data[0] = 0xfe;
				recv_data[1] = 0xfe;
				recv_data[2] = 0xfe;
				recv_data[3] = 0xfe;
				recv_data[4] = 0x38;

				num_received = 5;
				globalConnectionPhase = 17;
				localConnectionPhase = 0;
			}
		}
		else if (globalConnectionPhase == 17)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x3a;
				num_received = 1;
			}
			else
			{
				if (localConnectionPhase == 0)
				{
					recv_data[0] = 0x04;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 1;
				}
				else if (localConnectionPhase == 1)
				{
					recv_data[0] = 0x07;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 2;
				}
				else if (localConnectionPhase == 2)
				{
					recv_data[0] = 0x08;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 3;
				}
				else if (localConnectionPhase == 3)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x01;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 4;
				}
				else if (localConnectionPhase == 4)
				{
					recv_data[0] = 0x08;
					recv_data[1] = 0x00;
					recv_data[2] = 0x08;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 5;
				}
				else if (localConnectionPhase == 5)
				{
					recv_data[0] = 0x08;
					recv_data[1] = 0x00;
					recv_data[2] = 0x08;
					recv_data[3] = 0x01;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 6;
				}
				else if (localConnectionPhase == 6)
				{
					recv_data[0] = 0xfe;
					recv_data[1] = 0xfe;
					recv_data[2] = 0xfe;
					recv_data[3] = 0xfe;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 7;
				}
				else if (localConnectionPhase == 7)
				{
					recv_data[0] = 0x09;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 8;
				}
				else if (localConnectionPhase == 8)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					globalConnectionPhase = 18;
				}
			}
		}
		else if (globalConnectionPhase == 18)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x32;
				num_received = 1;
			}
			else
			{
				recv_data[0] = 0x00;
				recv_data[1] = 0x00;
				recv_data[2] = 0x00;
				recv_data[3] = 0x00;
				recv_data[4] = 0x30;

				num_received = 5;

				if (send_data[1] == 0x00 && send_data[2] == 0x00 && send_data[3] == 0x00 && send_data[4] == 0x30)
				{
					globalConnectionPhase = 19;
				}
			}
		}
		else if (globalConnectionPhase == 19)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x32;
				num_received = 1;
			}
			else
			{
				recv_data[0] = 0xfe;
				recv_data[1] = 0xfe;
				recv_data[2] = 0xfe;
				recv_data[3] = 0xfe;
				recv_data[4] = 0x38;

				num_received = 5;
				globalConnectionPhase = 20;
				localConnectionPhase = 0;
			}
		}
		else if (globalConnectionPhase == 20)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x3a;
				num_received = 1;
			}
			else
			{
				if (localConnectionPhase == 0)
				{
					recv_data[0] = 0x04;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 1;
				}
				else if (localConnectionPhase == 1)
				{
					recv_data[0] = 0x07;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 2;
				}
				else if (localConnectionPhase == 2)
				{
					recv_data[0] = 0x08;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 3;
				}
				else if (localConnectionPhase == 3)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x01;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 4;
				}
				else if (localConnectionPhase == 4)
				{
					recv_data[0] = 0x08;
					recv_data[1] = 0x00;
					recv_data[2] = 0x08;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 5;
				}
				else if (localConnectionPhase == 5)
				{
					recv_data[0] = 0x08;
					recv_data[1] = 0x00;
					recv_data[2] = 0x08;
					recv_data[3] = 0x01;
					recv_data[4] = 0x38;

					num_received = 5;
					globalConnectionPhase = 21;
				}
			}
		}
		else if (globalConnectionPhase == 21)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x32;
				num_received = 1;
			}
			else
			{
				recv_data[0] = 0x08;
				recv_data[1] = 0x00;
				recv_data[2] = 0x08;
				recv_data[3] = 0x01;
				recv_data[4] = 0x30;

				num_received = 5;

				if (send_data[1] == 0xff && send_data[2] == 0xff && send_data[3] == 0xff && send_data[4] == 0x30)
				{
					recv_data[0] = 0xfe;
					recv_data[1] = 0xfe;
					recv_data[2] = 0xfe;
					recv_data[3] = 0xfe;
					recv_data[4] = 0x38;

					num_received = 5;

					globalConnectionPhase = 22;
					localConnectionPhase = 0;
				}
			}
		}
		else if (globalConnectionPhase == 22)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x3a;
				num_received = 1;
			}
			else
			{
				if (localConnectionPhase == 0)
				{
					recv_data[0] = 0x09;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 1;
				}
				else if (localConnectionPhase == 1)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 2;
				}
				else if (localConnectionPhase == 2)
				{
					recv_data[0] = 0xfe;
					recv_data[1] = 0xfe;
					recv_data[2] = 0xfe;
					recv_data[3] = 0xfe;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 3;
				}
				else if (localConnectionPhase == 3)
				{
					recv_data[0] = 0x01;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 4;
				}
				else if (localConnectionPhase == 4)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					globalConnectionPhase = 23;
				}
			}
		}
		else if (globalConnectionPhase == 23)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x32;
				num_received = 1;
			}
			else
			{
				recv_data[0] = 0x00;
				recv_data[1] = 0x00;
				recv_data[2] = 0x00;
				recv_data[3] = 0x00;
				recv_data[4] = 0x30;

				num_received = 5;

				if (send_data[1] == 0xff && send_data[2] == 0xff && send_data[3] == 0xff && send_data[4] == 0x30)
				{
					globalConnectionPhase = 24;
				}
			}
		}
		else if (globalConnectionPhase == 24)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x3a;
				num_received = 1;
			}
			else
			{
				recv_data[0] = 0xfe;
				recv_data[1] = 0xfe;
				recv_data[2] = 0xfe;
				recv_data[3] = 0xfe;
				recv_data[4] = 0x38;

				num_received = 5;

				globalConnectionPhase = 25;
				localConnectionPhase = 0;
			}
		}
		else if (globalConnectionPhase == 25)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x3a;
				num_received = 1;
			}
			else
			{
				if (localConnectionPhase == 0)
				{
					recv_data[0] = 0x04;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 1;
				}
				else if (localConnectionPhase == 1)
				{
					recv_data[0] = 0x07;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 2;
				}
				else if (localConnectionPhase == 2)
				{
					recv_data[0] = 0x08;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 3;
				}
				else if (localConnectionPhase == 3)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x01;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 4;
				}
				else if (localConnectionPhase == 4)
				{
					recv_data[0] = 0x08;
					recv_data[1] = 0x00;
					recv_data[2] = 0x08;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 5;
				}
				else if (localConnectionPhase == 5)
				{
					recv_data[0] = 0x08;
					recv_data[1] = 0x00;
					recv_data[2] = 0x08;
					recv_data[3] = 0x01;
					recv_data[4] = 0x38;

					num_received = 5;
					globalConnectionPhase = 26;
					localConnectionPhase = 0;
				}
			}
		}
		else if (globalConnectionPhase == 26)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x32;
				num_received = 1;
			}
			else
			{
				recv_data[0] = 0x08;
				recv_data[1] = 0x00;
				recv_data[2] = 0x08;
				recv_data[3] = 0x01;
				recv_data[4] = 0x30;

				num_received = 5;

				if (localConnectionPhase == 0)
				{
					if (send_data[1] == 0xfe && send_data[2] == 0xfe && send_data[3] == 0xfe && send_data[4] == 0x30) //0xdc
					{
						//localConnectionPhase = 1;

						recv_data[0] = 0xfe;
						recv_data[1] = 0xfe;
						recv_data[2] = 0xfe;
						recv_data[3] = 0xfe;
						recv_data[4] = 0x38;

						num_received = 5;

						globalConnectionPhase = 27;
						localConnectionPhase = 0;
					}
				}
				/*
				else if (localConnectionPhase == 1)
				{
				if (send_data[1] == 0x09 && send_data[2] == 0x00 && send_data[3] == 0x00 && send_data[4] == 0x30) //0xdc
				{
				localConnectionPhase = 2;
				}
				else
				{
				localConnectionPhase = 0;
				}
				}
				else if (localConnectionPhase == 2)
				{
				if (send_data[1] == 0x03 && send_data[2] == 0x00 && send_data[3] == 0x00 && send_data[4] == 0x30) //0xdc
				{
				localConnectionPhase = 3;
				}
				else
				{
				localConnectionPhase = 0;
				}
				}
				else if (localConnectionPhase == 3)
				{
				localConnectionPhase = 4;
				}
				else if (localConnectionPhase == 4)
				{
				localConnectionPhase = 5;
				}
				else if (localConnectionPhase == 5)
				{
				localConnectionPhase = 6;
				}
				else if (localConnectionPhase == 6)
				{
				recv_data[0] = 0xfe;
				recv_data[1] = 0xfe;
				recv_data[2] = 0xfe;
				recv_data[3] = 0xfe;
				recv_data[4] = 0x38;

				num_received = 5;

				closeData11 = false;
				closeData12 = true;
				localConnectionPhase = 0;
				}
				*/
			}
		}
		else if (globalConnectionPhase == 27)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x3a;
				num_received = 1;
			}
			else
			{
				if (localConnectionPhase == 0)
				{
					recv_data[0] = 0x04;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 1;
				}
				else if (localConnectionPhase == 1)
				{
					recv_data[0] = 0x07;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 2;
				}
				else if (localConnectionPhase == 2)
				{
					recv_data[0] = 0x08;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 3;
				}
				else if (localConnectionPhase == 3)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x01;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 4;
				}
				else if (localConnectionPhase == 4)
				{
					recv_data[0] = 0x08;
					recv_data[1] = 0x00;
					recv_data[2] = 0x08;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 5;
				}
				else if (localConnectionPhase == 5)
				{
					recv_data[0] = 0x08;
					recv_data[1] = 0x00;
					recv_data[2] = 0x08;
					recv_data[3] = 0x01;
					recv_data[4] = 0x38;

					num_received = 5;
					globalConnectionPhase = 28;
				}
			}
		}
		else if (globalConnectionPhase == 28)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x32;
				num_received = 1;
			}
			else
			{
				recv_data[0] = 0x08;
				recv_data[1] = 0x00;
				recv_data[2] = 0x08;
				recv_data[3] = 0x01;
				recv_data[4] = 0x30;

				num_received = 5;

				if (send_data[1] == 0x11 && send_data[2] == 0x11 && send_data[3] == 0x11 && send_data[4] == 0x30 || send_data[1] == 0xff && send_data[2] == 0xff && send_data[3] == 0xff && send_data[4] == 0x30)
				{
					recv_data[0] = 0xfe;
					recv_data[1] = 0xfe;
					recv_data[2] = 0xfe;
					recv_data[3] = 0xfe;
					recv_data[4] = 0x38;

					num_received = 5;

					globalConnectionPhase = 29;
					localConnectionPhase = 0;
				}
			}
		}
		else if (globalConnectionPhase == 29)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x3a;
				num_received = 1;
			}
			else
			{
				if (localConnectionPhase == 0)
				{
					recv_data[0] = 0x09;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 1;
				}
				else if (localConnectionPhase == 1)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					globalConnectionPhase = 30;
				}
			}
		}
		else if (globalConnectionPhase == 30)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x32;
				num_received = 1;
			}
			else
			{
				recv_data[0] = 0x00;
				recv_data[1] = 0x00;
				recv_data[2] = 0x00;
				recv_data[3] = 0x00;
				recv_data[4] = 0x30;

				num_received = 5;

				if (send_data[1] == 0xff && send_data[2] == 0xff && send_data[3] == 0xff && send_data[4] == 0x30)
				{
					globalConnectionPhase = 31;
				}
			}
		}
		else if (globalConnectionPhase == 31)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x3a;
				num_received = 1;
			}
			else
			{
				recv_data[0] = 0xfe;
				recv_data[1] = 0xfe;
				recv_data[2] = 0xfe;
				recv_data[3] = 0xfe;
				recv_data[4] = 0x38;

				num_received = 5;
				globalConnectionPhase = 32;
				localConnectionPhase = 0;
			}
		}
		else if (globalConnectionPhase == 32)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x3a;
				num_received = 1;
			}
			else
			{
				if (localConnectionPhase == 0)
				{
					recv_data[0] = 0x04;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 1;
				}
				else if (localConnectionPhase == 1)
				{
					recv_data[0] = 0x07;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 2;
				}
				else if (localConnectionPhase == 2)
				{
					recv_data[0] = 0x08;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 3;
				}
				else if (localConnectionPhase == 3)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x01;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 4;
				}
				else if (localConnectionPhase == 4)
				{
					recv_data[0] = 0x08;
					recv_data[1] = 0x00;
					recv_data[2] = 0x08;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 5;
				}
				else if (localConnectionPhase == 5)
				{
					recv_data[0] = 0x08;
					recv_data[1] = 0x00;
					recv_data[2] = 0x08;
					recv_data[3] = 0x01;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 6;
				}
				else if (localConnectionPhase == 6)
				{
					recv_data[0] = 0xfe;
					recv_data[1] = 0xfe;
					recv_data[2] = 0xfe;
					recv_data[3] = 0xfe;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 7;
				}
				else if (localConnectionPhase == 7)
				{
					recv_data[0] = 0x09;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 8;
				}
				else if (localConnectionPhase == 8)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 9;
				}
				else if (localConnectionPhase == 9)
				{
					recv_data[0] = 0xfe;
					recv_data[1] = 0xfe;
					recv_data[2] = 0xfe;
					recv_data[3] = 0xfe;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 10;
				}
				else if (localConnectionPhase == 10)
				{
					recv_data[0] = 0x02;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 11;
				}
				else if (localConnectionPhase == 11)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					globalConnectionPhase = 33;
				}
			}
		}
		else if (globalConnectionPhase == 33)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x32;
				num_received = 1;
			}
			else
			{
				recv_data[0] = 0x00;
				recv_data[1] = 0x00;
				recv_data[2] = 0x00;
				recv_data[3] = 0x00;
				recv_data[4] = 0x30;

				num_received = 5;

				if (send_data[1] == 0xff && send_data[2] == 0xff && send_data[3] == 0xff && send_data[4] == 0x30)
				{
					globalConnectionPhase = 34;
					localConnectionPhase = 0;
				}
			}
		}
		else if (globalConnectionPhase == 34)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x3a;
				num_received = 1;
			}
			else
			{
				if (localConnectionPhase == 0)
				{
					recv_data[0] = 0xfe;
					recv_data[1] = 0xfe;
					recv_data[2] = 0xfe;
					recv_data[3] = 0xfe;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 1;
				}
				else if (localConnectionPhase == 1)
				{
					recv_data[0] = 0x03;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 2;
				}
				else if (localConnectionPhase == 2)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 3;
				}
				else if (localConnectionPhase == 3)
				{
					recv_data[0] = 0xfe;
					recv_data[1] = 0xfe;
					recv_data[2] = 0xfe;
					recv_data[3] = 0xfe;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 4;
				}
				else if (localConnectionPhase == 4)
				{
					recv_data[0] = 0x0a;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 5;
				}
				else if (localConnectionPhase == 5)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 6;
				}
				else if (localConnectionPhase == 6)
				{
					recv_data[0] = 0xfe;
					recv_data[1] = 0xfe;
					recv_data[2] = 0xfe;
					recv_data[3] = 0xfe;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 7;
				}
				else if (localConnectionPhase == 7)
				{
					recv_data[0] = 0x0e;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 8;
				}
				else if (localConnectionPhase == 8)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;

					globalConnectionPhase = 35;
					localConnectionPhase = 0;
				}

			}
		}
		else if (globalConnectionPhase == 35)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x32;
				num_received = 1;
			}
			else
			{
				recv_data[0] = 0x00;
				recv_data[1] = 0x00;
				recv_data[2] = 0x00;
				recv_data[3] = 0x00;
				recv_data[4] = 0x30;

				num_received = 5;

				if (localConnectionPhase == 0)
				{
					if (send_data[1] == 0xfe && send_data[2] == 0xfe && send_data[3] == 0xfe && send_data[4] == 0x30) //0xdc
					{
						//localConnectionPhase = 1;

						globalConnectionPhase = 36;
						localConnectionPhase = 0;
					}
				}
				/*
				else if (localConnectionPhase == 1)
				{
				if (send_data[1] == 0x03 && send_data[2] == 0x00 && send_data[3] == 0x00 && send_data[4] == 0x30) //0xdc
				{
				localConnectionPhase = 2;
				}
				else
				{
				localConnectionPhase = 0;
				}
				}
				else if (localConnectionPhase == 2)
				{
				if (send_data[1] == 0x03 && send_data[2] == 0x00 && send_data[3] == 0x00 && send_data[4] == 0x30) //0xdc
				{
				localConnectionPhase = 3;
				}
				else
				{
				localConnectionPhase = 0;
				}
				}
				else if (localConnectionPhase == 3)
				{
				if (send_data[4] == 0x30) //0xdc
				{
				localConnectionPhase = 4;
				}
				else
				{
				localConnectionPhase = 0;
				}
				}
				else if (localConnectionPhase == 4)
				{
				if (send_data[1] > 0x00 && send_data[2] == 0x00 && send_data[4] == 0x30) //0xdc
				{
				handShake1 = send_data[3];
				localConnectionPhase = 5;
				}
				}
				else if (localConnectionPhase == 5)
				{
				if (send_data[1] == 0x00 && send_data[2] == 0x00 && send_data[3] == handShake1 && send_data[4] == 0x30) //0xdc
				{
				closeData20 = false;
				closeData21 = true;
				localConnectionPhase = 0;
				}
				}
				*/
			}
		}
		else if (globalConnectionPhase == 36)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x3a;
				num_received = 1;
			}
			else
			{
				if (localConnectionPhase == 0)
				{
					recv_data[0] = 0xfe;
					recv_data[1] = 0xfe;
					recv_data[2] = 0xfe;
					recv_data[3] = 0xfe;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 1;
				}
				else if (localConnectionPhase == 1)
				{
					recv_data[0] = 0x04;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 2;
				}
				else if (localConnectionPhase == 2)
				{
					recv_data[0] = 0x07;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 3;
				}
				else if (localConnectionPhase == 3)
				{
					recv_data[0] = 0x08;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 4;
				}
				else if (localConnectionPhase == 4)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x04;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 5;
				}
				else if (localConnectionPhase == 5)
				{
					recv_data[0] = 0x08;
					recv_data[1] = 0x00;
					recv_data[2] = 0x08;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 6;
				}
				else if (localConnectionPhase == 6)
				{
					recv_data[0] = 0x08;
					recv_data[1] = 0x00;
					recv_data[2] = 0x0c;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 7;
				}
				else if (localConnectionPhase == 7)
				{
					recv_data[0] = 0xfe;
					recv_data[1] = 0xfe;
					recv_data[2] = 0xfe;
					recv_data[3] = 0xfe;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 8;
				}
				else if (localConnectionPhase == 8)
				{
					recv_data[0] = 0x09;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 9;
				}
				else if (localConnectionPhase == 9)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 10;
				}
				else if (localConnectionPhase == 10)
				{
					recv_data[0] = 0xfe;
					recv_data[1] = 0xfe;
					recv_data[2] = 0xfe;
					recv_data[3] = 0xfe;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 11;
				}
				else if (localConnectionPhase == 11)
				{
					recv_data[0] = 0x0b;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 12;
				}
				else if (localConnectionPhase == 12)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;

					isConnected = true;
					isConnecting = false;
					actionDataMode = 0;
					actionPhase = -1;
				}
			}
		}
	}



	//ACTION PHASE

	if (isConnected)
	{

		Movie::tunerStatus = 4;

		if (actionPhase == -1)
		{
			actionPhase = 0;
		}
		else if (actionPhase == 0)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x32;
				num_received = 1;
			}
			else
			{
				recv_data[0] = 0x00;
				recv_data[1] = 0x00;
				recv_data[2] = 0x00;
				recv_data[3] = 0x00;
				recv_data[4] = 0x30;

				num_received = 5;

				if (send_data[4] == 0x30)
				{
					if (tingleRNG)
					{
						if (frameTarget <= Movie::g_currentFrame) //Timeout
						{
							tingleRNG = false;
							frameTarget = 0;
							Movie::tunerExecuteID = 0;
						}
						else
						{
							if (send_data[1] == 0x05 && send_data[3] == 0x00)
							{
								if (send_data[2] == 0x0a || send_data[2] == 0x0c || send_data[2] == 0x0d || send_data[2] == 0x13)
								{
									tingleRNG = false;
									frameTarget = 0;

									/*
									std::string output;
									output = StringFromFormat("RNG: [< %02x]\n",
									(u8)send_data[2]);

									if (send_data[2] == 0x0c)
									PanicAlertT("WTF!");

									outputFile.WriteBytes(output.data(), output.size());

									Movie::tunerExecuteID = 13;
									*/

									if (send_data[2] == 0x0a)
										Movie::tunerExecuteID = 20; //Hidden ID for Discounted Blue Ting
									else if (send_data[2] == 0x0c)
										Movie::tunerExecuteID = 21; //Hidden ID for Balloon+Shield
									else if (send_data[2] == 0x0d)
										Movie::tunerExecuteID = 22; //Hidden ID for Extended Shield
									else if (send_data[2] == 0x13)
										Movie::tunerExecuteID = 23; //Hidden ID for Nothing
								}
							}
						}
					}

					if (Movie::tunerExecuteID > 0 && !tingleRNG)
					{
						actionPhase = 1;
						localConnectionPhase = 0;
						actionDataMode = 0; //fe phase by default

						recv_data[0] = 0xfe;
						recv_data[1] = 0xfe;
						recv_data[2] = 0xfe;
						recv_data[3] = 0xfe;
						recv_data[4] = 0x38;

						num_received = 5;
					}
					else if (!tingleRNG)
					{
						u32 isLoadingAdd;
						u32 eventFlagAdd;

						isLoadingAdd = 0x3ad335;
						eventFlagAdd = 0x3bd3a2;

						std::string gameID = SConfig::GetInstance().GetUniqueID();

						if (!gameID.compare("GZLJ01"))
						{
							u8 eventFlag = Memory::Read_U8(eventFlagAdd);
							u32 currLoading = Memory::Read_U32(isLoadingAdd);

							if (currLoading > 0)
							{
								inLoading = true;
								actionPhase = 2;
							}
							else
							{
								if (inLoading)
								{
									actionPhase = 2;

									if (eventFlag == 0)
									{
										inLoading = false;
										Movie::tunerExecuteID = 24; //Send Tingle Nothing without a fee
									}
								}
							}
						}
					}
				}
			}
		}
		else if (actionPhase == 1)
		{
			if (cmd == CMD_WRITE)
			{
				if (actionDataMode == 0)
				{
					recv_data[0] = 0x3a;
					num_received = 1;
				}
				else if (actionDataMode == 1)
				{
					recv_data[0] = 0x32;
					num_received = 1;
				}
				else if (actionDataMode == 2)
				{
					recv_data[0] = 0x3a;
					num_received = 1;
				}
			}
			else
			{
				if (actionDataMode == 1) //In between special Phases
				{
					if (Movie::tunerExecuteID == 10) //Bomb
					{
						recv_data[0] = 0x02;
						recv_data[1] = 0x0a; //Price = 10 rupees
						recv_data[2] = 0x1d;
						recv_data[3] = 0x08;
						recv_data[4] = 0x30;
					}
					else if (Movie::tunerExecuteID == 11) //Balloon
					{
						recv_data[0] = 0x03;
						recv_data[1] = 0x1e; //Price = 30 Rupees
						recv_data[2] = 0x1d;
						recv_data[3] = 0x08;
						recv_data[4] = 0x30;
					}
					else if (Movie::tunerExecuteID == 12) //Shield
					{
						recv_data[0] = 0x04;
						recv_data[1] = 0x28; //40 rupees
						recv_data[2] = 0x1d;
						recv_data[3] = 0x08;
						recv_data[4] = 0x30;
					}
					else if (Movie::tunerExecuteID == 13) //Kooloolimpah
					{
						recv_data[0] = 0x05;
						recv_data[1] = 0x00;
						recv_data[2] = 0x1d;
						recv_data[3] = 0x08;
						recv_data[4] = 0x30;
					}
					else if (Movie::tunerExecuteID == 14) //Red Ting
					{
						recv_data[0] = 0x08;
						recv_data[1] = 0x14; //Price = 20 Rupees
						recv_data[2] = 0x1d;
						recv_data[3] = 0x08;
						recv_data[4] = 0x30;
					}
					else if (Movie::tunerExecuteID == 15) //Green Ting
					{
						recv_data[0] = 0x09;
						recv_data[1] = 0x28; //40 rupees
						recv_data[2] = 0x1d;
						recv_data[3] = 0x08;
						recv_data[4] = 0x30;
					}
					else if (Movie::tunerExecuteID == 16) //Blue Ting
					{
						recv_data[0] = 0x0a;
						recv_data[1] = 0x50; //80 rupees
						recv_data[2] = 0x1d;
						recv_data[3] = 0x08;
						recv_data[4] = 0x30;
					}
					else if (Movie::tunerExecuteID == 17) //Heyy Tingle
					{
						recv_data[0] = 0x10;
						recv_data[1] = 0x00;
						recv_data[2] = 0x1d;
						recv_data[3] = 0x08;
						recv_data[4] = 0x30;
					}
					else if (Movie::tunerExecuteID == 20) //Discounted Blue Ting
					{
						recv_data[0] = 0x0a;
						recv_data[1] = 0x28; //40 rupees
						recv_data[2] = 0x1d;
						recv_data[3] = 0x08;
						recv_data[4] = 0x30;
					}
					else if (Movie::tunerExecuteID == 21) //Balloon+Shield
					{
						recv_data[0] = 0x0c;
						recv_data[1] = 0x28; //40 rupees
						recv_data[2] = 0x1d;
						recv_data[3] = 0x08;
						recv_data[4] = 0x30;
					}
					else if (Movie::tunerExecuteID == 22) //Extended Shield
					{
						recv_data[0] = 0x0d;
						recv_data[1] = 0x28; //40 rupees
						recv_data[2] = 0x1d;
						recv_data[3] = 0x08;
						recv_data[4] = 0x30;
					}
					else if (Movie::tunerExecuteID == 23) //Nothing
					{
						recv_data[0] = 0x13;
						recv_data[1] = 0x28; //40 rupees
						recv_data[2] = 0x1d;
						recv_data[3] = 0x08;
						recv_data[4] = 0x30;
					}
					else if (Movie::tunerExecuteID == 24) //Nothing (Loading)
					{
						recv_data[0] = 0x13;
						recv_data[1] = 0x00;
						recv_data[2] = 0x1d;
						recv_data[3] = 0x08;
						recv_data[4] = 0x30;
					}

					num_received = 5;

					if (send_data[4] == 0x30) //send_data[1] == 0xff && send_data[2] == 0xff && send_data[3] == 0xff && send_data[4] == 0x30)
					{
						localConnectionPhase = 0;
						actionDataMode = 2;

						recv_data[0] = 0xfe;
						recv_data[1] = 0xfe;
						recv_data[2] = 0xfe;
						recv_data[3] = 0xfe;
						recv_data[4] = 0x38;

						num_received = 5;
					}
				}
				else if (actionDataMode == 2)
				{
					if (localConnectionPhase == 0)
					{
						recv_data[0] = 0x09;
						recv_data[1] = 0x00;
						recv_data[2] = 0x00;
						recv_data[3] = 0x00;
						recv_data[4] = 0x38;

						num_received = 5;
						localConnectionPhase = 1;
					}
					else if (localConnectionPhase == 1)
					{
						recv_data[0] = 0x00;
						recv_data[1] = 0x00;
						recv_data[2] = 0x00;
						recv_data[3] = 0x00;
						recv_data[4] = 0x38;

						num_received = 5;
						localConnectionPhase = 2;
					}
					else if (localConnectionPhase == 2)
					{
						recv_data[0] = 0xfe;
						recv_data[1] = 0xfe;
						recv_data[2] = 0xfe;
						recv_data[3] = 0xfe;
						recv_data[4] = 0x38;

						num_received = 5;
						localConnectionPhase = 3;
					}
					else if (localConnectionPhase == 3)
					{
						recv_data[0] = 0x03;
						recv_data[1] = 0x00;
						recv_data[2] = 0x00;
						recv_data[3] = 0x00;
						recv_data[4] = 0x38;

						num_received = 5;
						localConnectionPhase = 4;
					}
					else if (localConnectionPhase == 4)
					{
						recv_data[0] = 0x00;
						recv_data[1] = 0x00;
						recv_data[2] = 0x00;
						recv_data[3] = 0x00;
						recv_data[4] = 0x38;

						num_received = 5;
						localConnectionPhase = 5;
					}
					else if (localConnectionPhase == 5)
					{
						recv_data[0] = 0xfe;
						recv_data[1] = 0xfe;
						recv_data[2] = 0xfe;
						recv_data[3] = 0xfe;
						recv_data[4] = 0x38;

						num_received = 5;
						localConnectionPhase = 6;
					}
					else if (localConnectionPhase == 6)
					{
						recv_data[0] = 0x0b;
						recv_data[1] = 0x00;
						recv_data[2] = 0x00;
						recv_data[3] = 0x00;
						recv_data[4] = 0x38;

						num_received = 5;
						localConnectionPhase = 7;
					}
					else if (localConnectionPhase == 7)
					{
						recv_data[0] = 0x00;
						recv_data[1] = 0x00;
						recv_data[2] = 0x00;
						recv_data[3] = 0x00;
						recv_data[4] = 0x38;

						num_received = 5;

						actionPhase = 3;
						localConnectionPhase = 0;
						actionDataMode = 0;
					}
				}
				else
				{
					if (Movie::tunerExecuteID > 9 && Movie::tunerExecuteID < 18 || Movie::tunerExecuteID > 19 && Movie::tunerExecuteID < 25)  //Bomb = 10, Balloon = 11, Shield = 12, Kooloolimpah = 13, Red = 14, Green = 15, Blue = 16, Discount Blue = 20, Shield+Balloon = 21, Extended Shield = 22, Nothing = 23, Nothing (Load) = 24
					{
						if (localConnectionPhase == 0)
						{
							recv_data[0] = 0x04;
							recv_data[1] = 0x00;
							recv_data[2] = 0x00;
							recv_data[3] = 0x00;
							recv_data[4] = 0x38;

							num_received = 5;
							localConnectionPhase = 1;
						}
						else if (localConnectionPhase == 1)
						{
							recv_data[0] = 0x07;
							recv_data[1] = 0x00;
							recv_data[2] = 0x00;
							recv_data[3] = 0x00;
							recv_data[4] = 0x38;

							num_received = 5;
							localConnectionPhase = 2;
						}
						else if (localConnectionPhase == 2)
						{
							recv_data[0] = 0x08;
							recv_data[1] = 0x00;
							recv_data[2] = 0x00;
							recv_data[3] = 0x00;
							recv_data[4] = 0x38;

							num_received = 5;
							localConnectionPhase = 3;
						}
						else if (localConnectionPhase == 3)
						{
							recv_data[0] = 0x00;
							recv_data[1] = 0x00;
							recv_data[2] = 0x04;
							recv_data[3] = 0x01;
							recv_data[4] = 0x38;

							num_received = 5;
							localConnectionPhase = 4;
						}
						else if (localConnectionPhase == 4)
						{
							recv_data[0] = 0xce;
							recv_data[1] = 0x00;
							recv_data[2] = 0x88;
							recv_data[3] = 0x00;
							recv_data[4] = 0x38;

							num_received = 5;
							localConnectionPhase = 5;
						}
						else if (localConnectionPhase == 5)
						{
							recv_data[0] = 0xce;
							recv_data[1] = 0x00;
							recv_data[2] = 0x8c;
							recv_data[3] = 0x01;
							recv_data[4] = 0x38;

							num_received = 5;
							localConnectionPhase = 6;
						}
						else if (localConnectionPhase == 6)
						{
							recv_data[0] = 0xfe;
							recv_data[1] = 0xfe;
							recv_data[2] = 0xfe;
							recv_data[3] = 0xfe;
							recv_data[4] = 0x38;

							num_received = 5;
							localConnectionPhase = 7;
						}
						else if (localConnectionPhase == 7)
						{
							recv_data[0] = 0x06;
							recv_data[1] = 0x00;
							recv_data[2] = 0x00;
							recv_data[3] = 0x00;
							recv_data[4] = 0x38;

							num_received = 5;
							localConnectionPhase = 8;
						}
						else if (localConnectionPhase == 8)
						{
							recv_data[0] = 0x07;
							recv_data[1] = 0x00;
							recv_data[2] = 0x00;
							recv_data[3] = 0x00;
							recv_data[4] = 0x38;

							num_received = 5;
							localConnectionPhase = 9;
						}
						else if (localConnectionPhase == 9)
						{
							recv_data[0] = 0x04;
							recv_data[1] = 0x00;
							recv_data[2] = 0x00;
							recv_data[3] = 0x00;
							recv_data[4] = 0x38;

							num_received = 5;
							localConnectionPhase = 13;
						}
						else if (localConnectionPhase == 13)
						{
							if (Movie::tunerExecuteID == 10) //Bomb
							{
								recv_data[0] = 0x02;
								recv_data[1] = 0x0a; //Price = 10 rupees
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 11) //Balloon
							{
								recv_data[0] = 0x03;
								recv_data[1] = 0x1e; //Price = 30 Rupees
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 12) //Shield
							{
								recv_data[0] = 0x04;
								recv_data[1] = 0x28; //=40
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 13) //Kooloolimpah
							{
								recv_data[0] = 0x05;
								recv_data[1] = 0x00;
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;

								//frameTarget = Movie::g_currentFrame + 90;
								//nextRNGBlocking = true;
							}
							else if (Movie::tunerExecuteID == 14) //Red Ting
							{
								recv_data[0] = 0x08;
								recv_data[1] = 0x14; //Price = 20 Rupees
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 15) //Green Ting
							{
								recv_data[0] = 0x09;
								recv_data[1] = 0x28; //40 rupees
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 16) //Blue Ting
							{
								recv_data[0] = 0x0a;
								recv_data[1] = 0x50; //80 rupees
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 17) //Heyy Tingle
							{
								recv_data[0] = 0x10;
								recv_data[1] = 0x00;
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 20) //Blue Ting Discounted
							{
								recv_data[0] = 0x0a;
								recv_data[1] = 0x28; //40 rupees
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 21) //Balloon+Shield
							{
								recv_data[0] = 0x0c;
								recv_data[1] = 0x28; //40 rupees
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 22) //Extended Shield
							{
								recv_data[0] = 0x0d;
								recv_data[1] = 0x28; //40 rupees
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 23) //Nothing
							{
								recv_data[0] = 0x13;
								recv_data[1] = 0x28; //40 rupees
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 24) //Nothing (Loading)
							{
								recv_data[0] = 0x13;
								recv_data[1] = 0x00;
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}

							num_received = 5;
							localConnectionPhase = 14;
						}
						else if (localConnectionPhase == 14)
						{
							if (Movie::tunerExecuteID == 10) //Bomb
							{
								recv_data[0] = 0x02;
								recv_data[1] = 0x0a; //Price = 10 rupees
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 11) //Balloon
							{
								recv_data[0] = 0x03;
								recv_data[1] = 0x1e; //Price = 30 Rupees
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 12) //Shield
							{
								recv_data[0] = 0x04;
								recv_data[1] = 0x28; //40 rupees
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 13) //Kooloolimpah
							{
								recv_data[0] = 0x05;
								recv_data[1] = 0x00;
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 14) //Red Ting
							{
								recv_data[0] = 0x08;
								recv_data[1] = 0x14; //Price = 20 Rupees
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 15) //Green Ting
							{
								recv_data[0] = 0x09;
								recv_data[1] = 0x28; //40 rupees
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 16) //Blue Ting
							{
								recv_data[0] = 0x0a;
								recv_data[1] = 0x50; //80 rupees
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 17) //Heyy Tingle
							{
								recv_data[0] = 0x10;
								recv_data[1] = 0x00;
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 20) //Blue Ting Discounted
							{
								recv_data[0] = 0x0a;
								recv_data[1] = 0x28; //40 rupees
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 21) //Balloon+Shield
							{
								recv_data[0] = 0x0c;
								recv_data[1] = 0x28; //40 rupees
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 22) //Extended Shield
							{
								recv_data[0] = 0x0d;
								recv_data[1] = 0x28; //40 rupees
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 23) //Nothing
							{
								recv_data[0] = 0x13;
								recv_data[1] = 0x28; //40 rupees
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 24) //Nothing (Loading)
							{
								recv_data[0] = 0x13;
								recv_data[1] = 0x00;
								recv_data[2] = 0x1d;
								recv_data[3] = 0x08;
								recv_data[4] = 0x38;
							}

							num_received = 5;

							actionDataMode = 1;
							localConnectionPhase = 0;
						}
					}
					else if (Movie::tunerExecuteID < 10) //Cursor Movements (1-8) or Reset (9)
					{
						if (localConnectionPhase == 0)
						{
							recv_data[0] = 0x04;
							recv_data[1] = 0x00;
							recv_data[2] = 0x00;
							recv_data[3] = 0x00;
							recv_data[4] = 0x38;

							num_received = 5;
							localConnectionPhase = 1;
						}
						else if (localConnectionPhase == 1)
						{
							recv_data[0] = 0x07;
							recv_data[1] = 0x00;
							recv_data[2] = 0x00;
							recv_data[3] = 0x00;
							recv_data[4] = 0x38;

							num_received = 5;
							localConnectionPhase = 2;
						}
						else if (localConnectionPhase == 2)
						{
							recv_data[0] = 0x08;
							recv_data[1] = 0x00;
							recv_data[2] = 0x00;
							recv_data[3] = 0x00;
							recv_data[4] = 0x38;

							num_received = 5;
							localConnectionPhase = 3;
						}
						else if (localConnectionPhase == 3)
						{
							if (Movie::tunerExecuteID == 1) //Up
							{
								recv_data[0] = 0x40; //40 = up, 50 = up-right, 10 = right, 90 = down-right, 80 = down, a0 = down-left, 20 = left, 60 = up-left
								recv_data[1] = 0x00;
								recv_data[2] = 0x04;
								recv_data[3] = 0x01;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 2) //Up-Right
							{
								recv_data[0] = 0x50; //40 = up, 50 = up-right, 10 = right, 90 = down-right, 80 = down, a0 = down-left, 20 = left, 60 = up-left
								recv_data[1] = 0x00;
								recv_data[2] = 0x04;
								recv_data[3] = 0x01;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 3) //Right
							{
								recv_data[0] = 0x10; //40 = up, 50 = up-right, 10 = right, 90 = down-right, 80 = down, a0 = down-left, 20 = left, 60 = up-left
								recv_data[1] = 0x00;
								recv_data[2] = 0x04;
								recv_data[3] = 0x01;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 4) //Down-Right
							{
								recv_data[0] = 0x90; //40 = up, 50 = up-right, 10 = right, 90 = down-right, 80 = down, a0 = down-left, 20 = left, 60 = up-left
								recv_data[1] = 0x00;
								recv_data[2] = 0x04;
								recv_data[3] = 0x01;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 5) //Down
							{
								recv_data[0] = 0x80; //40 = up, 50 = up-right, 10 = right, 90 = down-right, 80 = down, a0 = down-left, 20 = left, 60 = up-left
								recv_data[1] = 0x00;
								recv_data[2] = 0x04;
								recv_data[3] = 0x01;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 6) //Down-Left
							{
								recv_data[0] = 0xa0; //40 = up, 50 = up-right, 10 = right, 90 = down-right, 80 = down, a0 = down-left, 20 = left, 60 = up-left
								recv_data[1] = 0x00;
								recv_data[2] = 0x04;
								recv_data[3] = 0x01;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 7) //Left
							{
								recv_data[0] = 0x20; //40 = up, 50 = up-right, 10 = right, 90 = down-right, 80 = down, a0 = down-left, 20 = left, 60 = up-left
								recv_data[1] = 0x00;
								recv_data[2] = 0x04;
								recv_data[3] = 0x01;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 8) //Up-Left
							{
								recv_data[0] = 0x60; //40 = up, 50 = up-right, 10 = right, 90 = down-right, 80 = down, a0 = down-left, 20 = left, 60 = up-left
								recv_data[1] = 0x00;
								recv_data[2] = 0x04;
								recv_data[3] = 0x01;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 9) //Reset
							{
								recv_data[0] = 0x00;
								recv_data[1] = 0x01;
								recv_data[2] = 0x04;
								recv_data[3] = 0x01;
								recv_data[4] = 0x38;
							}

							num_received = 5;
							localConnectionPhase = 4;
						}
						else if (localConnectionPhase == 4)
						{
							if (Movie::tunerExecuteID == 1) //Up
							{
								recv_data[0] = 0xc5;
								recv_data[1] = 0x00;
								recv_data[2] = 0x88;
								recv_data[3] = 0x00;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 2) //Up-Right
							{
								recv_data[0] = 0x08;
								recv_data[1] = 0x00;
								recv_data[2] = 0x11;
								recv_data[3] = 0x01;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 3) //Right
							{
								recv_data[0] = 0xc5;
								recv_data[1] = 0x00;
								recv_data[2] = 0x88;
								recv_data[3] = 0x00;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 4) //Down-Right
							{
								recv_data[0] = 0x00;
								recv_data[1] = 0x00;
								recv_data[2] = 0xff;
								recv_data[3] = 0x00;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 5) //Down
							{
								recv_data[0] = 0xc9;
								recv_data[1] = 0x00;
								recv_data[2] = 0x88;
								recv_data[3] = 0x00;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 6) //Down-Left
							{
								recv_data[0] = 0x10;
								recv_data[1] = 0x00;
								recv_data[2] = 0xf3;
								recv_data[3] = 0x00;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 7) //Left
							{
								recv_data[0] = 0xe7;
								recv_data[1] = 0x00;
								recv_data[2] = 0x88;
								recv_data[3] = 0x00;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 8) //Up-Left
							{
								recv_data[0] = 0x06;
								recv_data[1] = 0x00;
								recv_data[2] = 0x13;
								recv_data[3] = 0x01;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 9) //Reset
							{
								recv_data[0] = 0xc9;
								recv_data[1] = 0x00;
								recv_data[2] = 0x88;
								recv_data[3] = 0x00;
								recv_data[4] = 0x38;
							}

							num_received = 5;
							localConnectionPhase = 5;
						}
						else if (localConnectionPhase == 5)
						{
							if (Movie::tunerExecuteID == 1) //Up
							{
								recv_data[0] = 0x05;
								recv_data[1] = 0x01;
								recv_data[2] = 0x8c;
								recv_data[3] = 0x01;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 2) //Up-Right
							{
								recv_data[0] = 0x58;
								recv_data[1] = 0x00;
								recv_data[2] = 0x15;
								recv_data[3] = 0x02;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 3) //Right
							{
								recv_data[0] = 0xd5;
								recv_data[1] = 0x00;
								recv_data[2] = 0x8c;
								recv_data[3] = 0x01;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 4) //Down-Right
							{
								recv_data[0] = 0x90;
								recv_data[1] = 0x00;
								recv_data[2] = 0x03;
								recv_data[3] = 0x02;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 5) //Down
							{
								recv_data[0] = 0x49;
								recv_data[1] = 0x01;
								recv_data[2] = 0x8c;
								recv_data[3] = 0x01;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 6) //Down-Left
							{
								recv_data[0] = 0xb0;
								recv_data[1] = 0x00;
								recv_data[2] = 0xf7;
								recv_data[3] = 0x01;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 7) //Left
							{
								recv_data[0] = 0x07;
								recv_data[1] = 0x01;
								recv_data[2] = 0x8c;
								recv_data[3] = 0x01;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 8) //Up-Left
							{
								recv_data[0] = 0x66;
								recv_data[1] = 0x00;
								recv_data[2] = 0x17;
								recv_data[3] = 0x02;
								recv_data[4] = 0x38;
							}
							else if (Movie::tunerExecuteID == 9) //Reset
							{
								recv_data[0] = 0xc9;
								recv_data[1] = 0x01;
								recv_data[2] = 0x8c;
								recv_data[3] = 0x01;
								recv_data[4] = 0x38;
							}

							num_received = 5;
							localConnectionPhase = 6;
						}
						else if (localConnectionPhase == 6)
						{
							recv_data[0] = 0xfe;
							recv_data[1] = 0xfe;
							recv_data[2] = 0xfe;
							recv_data[3] = 0xfe;
							recv_data[4] = 0x38;

							num_received = 5;
							localConnectionPhase = 7;
						}
						else if (localConnectionPhase == 7)
						{
							recv_data[0] = 0x09;
							recv_data[1] = 0x00;
							recv_data[2] = 0x00;
							recv_data[3] = 0x00;
							recv_data[4] = 0x38;

							num_received = 5;
							localConnectionPhase = 8;
						}
						else if (localConnectionPhase == 8)
						{
							recv_data[0] = 0x00;
							recv_data[1] = 0x00;
							recv_data[2] = 0x00;
							recv_data[3] = 0x00;
							recv_data[4] = 0x38;

							num_received = 5;
							localConnectionPhase = 9;
						}
						else if (localConnectionPhase == 9)
						{
							recv_data[0] = 0xfe;
							recv_data[1] = 0xfe;
							recv_data[2] = 0xfe;
							recv_data[3] = 0xfe;
							recv_data[4] = 0x38;

							num_received = 5;
							localConnectionPhase = 10;
						}
						else if (localConnectionPhase == 10)
						{
							recv_data[0] = 0x03;
							recv_data[1] = 0x00;
							recv_data[2] = 0x00;
							recv_data[3] = 0x00;
							recv_data[4] = 0x38;

							num_received = 5;
							localConnectionPhase = 11;
						}
						else if (localConnectionPhase == 11)
						{
							recv_data[0] = 0x00;
							recv_data[1] = 0x00;
							recv_data[2] = 0x00;
							recv_data[3] = 0x00;
							recv_data[4] = 0x38;

							num_received = 5;

							localConnectionPhase = 0;
							actionPhase = 0;
							Movie::tunerExecuteID = 0;
						}
					}
				}
			}
		}

		else if (actionPhase == 2) //Status Update for Cursor Handling
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x3a;
				num_received = 1;
			}
			else
			{
				if (localConnectionPhase == 0)
				{
					recv_data[0] = 0x04;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 1;
				}
				else if (localConnectionPhase == 1)
				{
					recv_data[0] = 0x07;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 2;
				}
				else if (localConnectionPhase == 2)
				{
					recv_data[0] = 0x08;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 3;
				}
				else if (localConnectionPhase == 3)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x04;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 4;
				}
				else if (localConnectionPhase == 4)
				{
					recv_data[0] = 0x0c;
					recv_data[1] = 0x01;
					recv_data[2] = 0x9a;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 5;
				}
				else if (localConnectionPhase == 5)
				{
					recv_data[0] = 0x0c;
					recv_data[1] = 0x01;
					recv_data[2] = 0x9e;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 6;
				}
				else if (localConnectionPhase == 6)
				{
					recv_data[0] = 0xfe;
					recv_data[1] = 0xfe;
					recv_data[2] = 0xfe;
					recv_data[3] = 0xfe;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 7;
				}
				else if (localConnectionPhase == 7)
				{
					recv_data[0] = 0x09;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 8;
				}
				else if (localConnectionPhase == 8)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 9;
				}
				else if (localConnectionPhase == 9)
				{
					recv_data[0] = 0xfe;
					recv_data[1] = 0xfe;
					recv_data[2] = 0xfe;
					recv_data[3] = 0xfe;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 10;
				}
				else if (localConnectionPhase == 10)
				{
					recv_data[0] = 0x02;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 11;
				}
				else if (localConnectionPhase == 11)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 12;
				}
				else if (localConnectionPhase == 12)
				{
					recv_data[0] = 0xfe;
					recv_data[1] = 0xfe;
					recv_data[2] = 0xfe;
					recv_data[3] = 0xfe;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 13;
				}
				else if (localConnectionPhase == 13)
				{
					recv_data[0] = 0x01;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 14;
				}
				else if (localConnectionPhase == 14)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;

					localConnectionPhase = 0;
					actionPhase = 0;
				}
			}
		}

		else if (actionPhase == 3) //Cooldown after Action
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x32;
				num_received = 1;
			}
			else
			{
				recv_data[0] = 0x00;
				recv_data[1] = 0x00;
				recv_data[2] = 0x00;
				recv_data[3] = 0x00;
				recv_data[4] = 0x30;

				num_received = 5;

				if (send_data[4] == 0x30) //send_data[1] == 0xfe && send_data[2] == 0xfe && send_data[3] == 0xfe && send_data[4] == 0x30) //0xdc
				{
					actionPhase = 4;
					localConnectionPhase = 0;

					recv_data[0] = 0xfe;
					recv_data[1] = 0xfe;
					recv_data[2] = 0xfe;
					recv_data[3] = 0xfe;
					recv_data[4] = 0x38;
				}
			}
		}
		else if (actionPhase == 4)
		{
			if (cmd == CMD_WRITE)
			{
				recv_data[0] = 0x3a;
				num_received = 1;
			}
			else
			{
				if (localConnectionPhase == 0)
				{
					recv_data[0] = 0x04;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 1;
				}
				else if (localConnectionPhase == 1)
				{
					recv_data[0] = 0x07;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 2;
				}
				else if (localConnectionPhase == 2)
				{
					recv_data[0] = 0x08;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 3;
				}
				else if (localConnectionPhase == 3)
				{
					recv_data[0] = 0x02;
					recv_data[1] = 0x00;
					recv_data[2] = 0x04;
					recv_data[3] = 0x01;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 4;
				}
				else if (localConnectionPhase == 4)
				{
					recv_data[0] = 0xce;
					recv_data[1] = 0x00;
					recv_data[2] = 0x88;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 5;
				}
				else if (localConnectionPhase == 5)
				{
					recv_data[0] = 0xd0;
					recv_data[1] = 0x00;
					recv_data[2] = 0x8c;
					recv_data[3] = 0x01;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 6;
				}
				else if (localConnectionPhase == 6)
				{
					recv_data[0] = 0xfe;
					recv_data[1] = 0xfe;
					recv_data[2] = 0xfe;
					recv_data[3] = 0xfe;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 7;
				}
				else if (localConnectionPhase == 7)
				{
					recv_data[0] = 0x07;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;
					localConnectionPhase = 8;
				}
				else if (localConnectionPhase == 8)
				{
					recv_data[0] = 0x00;
					recv_data[1] = 0x00;
					recv_data[2] = 0x00;
					recv_data[3] = 0x00;
					recv_data[4] = 0x38;

					num_received = 5;

					localConnectionPhase = 0;
					actionPhase = 0;

					if (Movie::tunerExecuteID == 13) //Wait for RNG to be returned in Kooloolimpahs case
					{
						tingleRNG = true;
						frameTarget = Movie::g_currentFrame + 150;
					}
					else
					{
						Movie::tunerExecuteID = 0;
					}
				}
			}
		}

		//OBSOLETE
		/*
		else if (closeData27)
		{
		if (cmd == CMD_WRITE)
		{
		recv_data[0] = 0x32;
		num_received = 1;
		}
		else
		{
		if (phaseCounter == 1)
		{
		recv_data[0] = 0xce;
		recv_data[1] = 0x00;
		recv_data[2] = 0x8c;
		recv_data[3] = 0x01;
		recv_data[4] = 0x30;
		}

		num_received = 5;

		if (send_data[4] == 0x30) //send_data[1] == 0xfe && send_data[2] == 0xfe && send_data[3] == 0xfe && send_data[4] == 0x30) //0xdc
		{
		closeData27 = false;
		closeData28 = true;
		localConnectionPhase = 0;

		recv_data[0] = 0xfe;
		recv_data[1] = 0xfe;
		recv_data[2] = 0xfe;
		recv_data[3] = 0xfe;
		recv_data[4] = 0x38;
		}
		}
		}
		*/
		//OBSOLETE
		/*
		else if (closeData28)
		{
		if (cmd == CMD_WRITE)
		{
		recv_data[0] = 0x3a;
		num_received = 1;
		}
		else
		{
		if (localConnectionPhase == 0)
		{
		recv_data[0] = 0x09;
		recv_data[1] = 0x00;
		recv_data[2] = 0x00;
		recv_data[3] = 0x00;
		recv_data[4] = 0x38;

		num_received = 5;
		localConnectionPhase = 1;
		}
		else if (localConnectionPhase == 1)
		{
		recv_data[0] = 0x00;
		recv_data[1] = 0x00;
		recv_data[2] = 0x00;
		recv_data[3] = 0x00;
		recv_data[4] = 0x38;

		num_received = 5;
		localConnectionPhase = 2;
		}
		else if (localConnectionPhase == 2)
		{
		recv_data[0] = 0xfe;
		recv_data[1] = 0xfe;
		recv_data[2] = 0xfe;
		recv_data[3] = 0xfe;
		recv_data[4] = 0x38;

		num_received = 5;
		localConnectionPhase = 3;
		}
		else if (localConnectionPhase == 3)
		{
		recv_data[0] = 0x03;
		recv_data[1] = 0x00;
		recv_data[2] = 0x00;
		recv_data[3] = 0x00;
		recv_data[4] = 0x38;

		num_received = 5;
		localConnectionPhase = 4;
		}
		else if (localConnectionPhase == 4)
		{
		recv_data[0] = 0x00;
		recv_data[1] = 0x00;
		recv_data[2] = 0x00;
		recv_data[3] = 0x00;
		recv_data[4] = 0x38;

		num_received = 5;
		localConnectionPhase = 5;
		}
		else if (localConnectionPhase == 5)
		{
		recv_data[0] = 0xfe;
		recv_data[1] = 0xfe;
		recv_data[2] = 0xfe;
		recv_data[3] = 0xfe;
		recv_data[4] = 0x38;

		num_received = 5;
		localConnectionPhase = 6;
		}
		else if (localConnectionPhase == 6)
		{
		recv_data[0] = 0x0b;
		recv_data[1] = 0x00;
		recv_data[2] = 0x00;
		recv_data[3] = 0x00;
		recv_data[4] = 0x38;

		num_received = 5;
		localConnectionPhase = 7;
		}
		else if (localConnectionPhase == 7)
		{
		recv_data[0] = 0x00;
		recv_data[1] = 0x00;
		recv_data[2] = 0x00;
		recv_data[3] = 0x00;
		recv_data[4] = 0x38;

		num_received = 5;

		localConnectionPhase = 0;
		closeData28 = false;
		closeData22 = true;
		//Movie::tunerExecuteID = 10;
		}
		}
		}
		*/
	}

	//Logging of received data
	/*
	std::string output;

	if (num_received == 5)
	output = StringFromFormat("[< %02x %02x %02x %02x %02x]",
	(u8)recv_data[0], (u8)recv_data[1], (u8)recv_data[2],
	(u8)recv_data[3], (u8)recv_data[4]);
	else if (num_received == 4)
	output = StringFromFormat("[< %02x %02x %02x %02x]",
	(u8)recv_data[0], (u8)recv_data[1], (u8)recv_data[2],
	(u8)recv_data[3]);
	else if (num_received == 3)
	output = StringFromFormat("[< %02x %02x %02x]",
	(u8)recv_data[0], (u8)recv_data[1], (u8)recv_data[2]);
	else if (num_received == 2)
	output = StringFromFormat("[< %02x %02x]",
	(u8)recv_data[0], (u8)recv_data[1]);
	else if (num_received == 1)
	output = StringFromFormat("[< %02x]",
	(u8)recv_data[0]);
	else
	output = StringFromFormat("[< N/A ]");

	outputFile.WriteBytes(output.data(), output.size());
	*/

	if (num_received == 0)
	{
		PanicAlertT("No arguments error! isConnected: %i, isConnecting: %i, idlePhase: %i", isConnected, isConnecting, idlePhase);
		return 5;
	}

	for (int i = 0; i < 5; i++)
		si_buffer[i ^ 3] = recv_data[i];

	return (int)num_received;
}


CSIDevice_GBA::CSIDevice_GBA(SIDevices _device, int _iDeviceNumber)
	: ISIDevice(_device, _iDeviceNumber)
	, GBASockServer(_iDeviceNumber)
{
	waiting_for_response = false;


	//Connection variables
	isEnabled = true;
	isConnecting = true;
	idlePhase = true;

	globalConnectionPhase = 0;
	localConnectionPhase = 0;
	finalDataGlobalPhase = 0;

	//Action variables
	isConnected = false;
	tingleRNG = false;
	inLoading = false;

	actionPhase = 0;
	actionDataMode = 0;
	frameTarget = 0;

	//Report variables
	cyclesPerFrame = 0;
	totalActionCyclesRequired = 0;
	executionTime = 0;
	reportActive = false;
	currentFrame = 0;
	reportedActionID = 0;

	//Movie Stuff
	Movie::tunerStatus = 1;
	Movie::tunerExecuteID = 0;
}

CSIDevice_GBA::~CSIDevice_GBA()
{
	GBASockServer::Disconnect();
}

int CSIDevice_GBA::RunBuffer(u8* _pBuffer, int _iLength)
{
	//Handle Activation/Deactivation
	if (Movie::tunerExecuteID == 18) //Activate fake GBA
	{
		isEnabled = true;
		isConnecting = false;
		isConnected = true;

		num_data_received = 0;
		waiting_for_response = false;

		actionPhase = 0;
		actionDataMode = 0;
		Movie::tunerExecuteID = 0;
		Movie::tunerStatus = 1;
	}
	else if (Movie::tunerExecuteID == 19) //Deactivate fake GBA
	{
		isEnabled = false;
		isConnecting = false;
		isConnected = false;

		num_data_received = 0;
		waiting_for_response = false;

		Movie::tunerExecuteID = 0;
		Movie::tunerStatus = 0;
	}


	if (isEnabled) //Fake GBA
	{
		//Connection Phase needs to be slower
		if (!isConnected)
		{
			if (!waiting_for_response)
			{
				for (int i = 0; i < 5; i++)
					send_data[i] = _pBuffer[i ^ 3];

				num_data_received = 0;
				timestamp_sent = CoreTiming::GetTicks();
				waiting_for_response = true;
			}

			if (waiting_for_response && num_data_received == 0)
			{
				num_data_received = CreateFakeResponse(_pBuffer);
			}

			if ((GetTransferTime(send_data[0])) > (int)(CoreTiming::GetTicks() - timestamp_sent))
			{
				return 0;
			}
			else
			{
				if (num_data_received != 0)
					waiting_for_response = false;
				return num_data_received;
			}
		}
		else //Normal Action Phase
		{
			num_data_received = 0;
			waiting_for_response = false;

			num_data_received = CreateFakeResponse(_pBuffer);

			return num_data_received;
		}
		
	}
	else //Real GBA
	{
		if (!waiting_for_response)
		{
			for (int i = 0; i < 5; i++)
				send_data[i] = _pBuffer[i ^ 3];

			num_data_received = 0;
			ClockSync();
			Send(_pBuffer);
			timestamp_sent = CoreTiming::GetTicks();
			waiting_for_response = true;
		}

		if (waiting_for_response && num_data_received == 0)
		{
			num_data_received = Receive(_pBuffer);
		}

		if ((GetTransferTime(send_data[0])) > (int)(CoreTiming::GetTicks() - timestamp_sent))
		{
			return 0;
		}
		else
		{
			if (num_data_received != 0)
				waiting_for_response = false;
			return num_data_received;
		}
	}
}

int CSIDevice_GBA::TransferInterval()
{
	return GetTransferTime(send_data[0]);
}

// Dragonbane: Savestate support
void CSIDevice_GBA::DoState(PointerWrap& p)
{
	p.Do(isEnabled);
	p.Do(isConnecting);
	p.Do(idlePhase);

	p.Do(frameTarget);
	p.Do(globalConnectionPhase);
	p.Do(localConnectionPhase);
	p.Do(finalDataGlobalPhase);

	p.Do(handShake1);
	p.Do(handShake2);
	p.Do(handShake3);
	p.Do(handShake4);

	p.Do(isConnected);
	p.Do(inLoading);
	p.Do(tingleRNG);
	p.Do(actionPhase);
	p.Do(actionDataMode);

	//Connection Variables
	//p.Do(waiting_for_response);
	//p.Do(send_data);
	//p.Do(num_data_received);
	//p.Do(timestamp_sent);	
}