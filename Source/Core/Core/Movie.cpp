// Copyright 2009 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <mutex>
#include <mbedtls/config.h>
#include <mbedtls/md.h>

#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Hash.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Common/Timer.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Movie.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayProto.h"
#include "Core/State.h"
#include "Core/DSP/DSPCore.h"
#include "Core/HW/DVDInterface.h"
#include "Core/HW/EXI_Device.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/HW/WiimoteEmu/WiimoteHid.h"
#include "Core/HW/WiimoteEmu/Attachment/Classic.h"
#include "Core/HW/WiimoteEmu/Attachment/Nunchuk.h"
#include "Core/IPC_HLE/WII_IPC_HLE_Device_usb.h"
#include "Core/IPC_HLE/WII_IPC_HLE_WiiMote.h"
#include "Core/PowerPC/PowerPC.h"
#include "InputCommon/GCPadStatus.h"
#include "VideoCommon/VideoConfig.h"

//Dragonbane
#include "Core/Host.h"

// The chunk to allocate movie data in multiples of.
#define DTM_BASE_LENGTH (1024)

static std::mutex cs_frameSkip;

namespace Movie {

static bool s_bFrameStep = false;
static bool s_bFrameStop = false;
static bool s_bReadOnly = true;
static u32 s_rerecords = 0;
static PlayMode s_playMode = MODE_NONE;

static u32 s_framesToSkip = 0, s_frameSkipCounter = 0;

//Dragonbane
//static u8* tmpDynamicRange = nullptr;
bool badSettings = false;
bool lagWarning = false;
bool isVerifying = false;
bool isAutoVerifying = false;
bool autoSave = false;
int desyncCount = 0;
bool justStoppedRecording = false;

//Load syncing from old DTMs (Legacy)
bool nowLoading = false;
u64 lastLoadByte = 0;
u64 waitFrames = 0;
bool waited = false;
bool reverseWait = false;

static bool saveMemCard = false;

static std::string autoVerifyMovieFilename = File::GetUserPath(D_STATESAVES_IDX) + "AutoVerify.dtm";
static std::string autoVerifyStateFilename = File::GetUserPath(D_STATESAVES_IDX) + "AutoVerify.sav";
static std::string lastMovie = "";

//Dragonbane: Auto Roll Stuff
int roll_timer = 0;
bool roll_enabled = false;
bool first_roll = false;
bool checkSave = false;
bool uncheckSave = false;

//Dragonbane: Superswim Stuff
bool swimStarted = false;
bool swimInProgress = false;
float swimDestPosX = 0.0f;
float swimDestPosZ = 0.0f;


//Dragonbane: Video Comparison Stuff
bool cmp_requested = false;
bool cmp_isRunning = false;
bool cmp_leftFinished = false;
bool cmp_rightFinished = false;
bool cmp_loadState = false;
bool cmp_movieFinished = false;
bool cmp_justFinished = false;
std::string cmp_currentMovie = "";
std::string cmp_leftMovie = "";
std::string cmp_rightMovie = "";
std::string cmp_leftTitle = "";
std::string cmp_rightTitle = "";
std::string cmp_outputPath = "";
std::string cmp_currentBranch = "";
int cmp_width = 0;
int cmp_height = 0;
u64 cmp_startTimerFrame = 0;
u64 cmp_curentBranchFrame = 0;

bool updateMainFrame = false;

//Tuner Stuff
u8 tunerActionID = 0;
u8 tunerExecuteID = 0;
u8 tunerStatus = 1;
static u8 s_numGBAs; //Dragonbane


static u8 s_numPads = 0;
static ControllerState s_padState;
static DTMHeader tmpHeader;
static u8* tmpInput = nullptr;
static size_t tmpInputAllocated = 0;
static u64 s_currentByte = 0, s_totalBytes = 0;
u64 g_currentFrame = 0, g_totalFrames = 0; // VI
u64 g_currentLagCount = 0;
static u64 s_totalLagCount = 0; // just stats
u64 g_currentInputCount = 0, g_totalInputCount = 0; // just stats
static u64 s_totalTickCount = 0, s_tickCountAtLastInput = 0; // just stats

static u64 s_recordingStartTime; // seconds since 1970 that recording started
static bool s_bSaveConfig = false, s_bSkipIdle = false, s_bDualCore = false;
static bool s_bProgressive = false, s_bPAL60 = false;
static bool s_bDSPHLE = false, s_bFastDiscSpeed = false;
static bool s_bSyncGPU = false, s_bNetPlay = false;
static std::string s_videoBackend = "unknown";
static int s_iCPUCore = 1;
bool g_bClearSave = false;
bool g_bDiscChange = false;
bool g_bReset = false;
static std::string s_author = "";
std::string g_discChange = "";
u64 g_titleID = 0;
static u8 s_MD5[16];
static u8 s_bongos, s_memcards;
static u8 s_revision[20];
static u32 s_DSPiromHash = 0;
static u32 s_DSPcoefHash = 0;

static bool s_bRecordingFromSaveState = false;
static bool s_bPolled = false;

static std::string s_InputDisplay[8];

static GCManipFunction gcmfunc = nullptr;
static WiiManipFunction wiimfunc = nullptr;

static void EnsureTmpInputSize(size_t bound)
{
	if (tmpInputAllocated >= bound)
		return;
	// The buffer expands in powers of two of DTM_BASE_LENGTH
	// (standard exponential buffer growth).
	size_t newAlloc = DTM_BASE_LENGTH;
	while (newAlloc < bound)
		newAlloc *= 2;

	u8* newTmpInput = new u8[newAlloc];
	tmpInputAllocated = newAlloc;
	if (tmpInput != nullptr)
	{
		if (s_totalBytes > 0)
			memcpy(newTmpInput, tmpInput, (size_t)s_totalBytes);
		delete[] tmpInput;
	}
	tmpInput = newTmpInput;
}

static bool IsMovieHeader(u8 magic[4])
{
	return magic[0] == 'D' &&
	       magic[1] == 'T' &&
	       magic[2] == 'M' &&
	       magic[3] == 0x1A;
}

std::string GetInputDisplay()
{
	if (!IsMovieActive())
	{
		s_numPads = 0;
		for (int i = 0; i < 4; ++i)
		{
			if (SerialInterface::GetDeviceType(i) != SIDEVICE_NONE)
				s_numPads |= (1 << i);
			if (g_wiimote_sources[i] != WIIMOTE_SRC_NONE)
				s_numPads |= (1 << (i + 4));
		}
	}

	std::string inputDisplay = "";
	for (int i = 0; i < 8; ++i)
		if ((s_numPads & (1 << i)) != 0)
			inputDisplay.append(s_InputDisplay[i]);


	//Dragonbane
	std::string gameID = SConfig::GetInstance().GetUniqueID();
	std::string iniContent;

	bool success = File::ReadFileToString(File::GetExeDirectory() + "\\InfoDisplay\\" + gameID + ".ini", iniContent);

	if (success)
	{
		int lineCounter = 0;
		bool inProgress = true;
		inputDisplay.append("\n");

		while (inProgress)
		{
			lineCounter++;

			std::string lineName = StringFromFormat("Line%i", lineCounter);

			std::string::size_type loc = iniContent.find(lineName, 0);
			if (loc == std::string::npos)
			{
				inProgress = false;
				break;
			}

			iniContent = iniContent.substr(loc);
			iniContent = iniContent.substr(iniContent.find("\"", 0) + 1);

			std::string line = iniContent.substr(0, iniContent.find("\"", 0));
			std::string blockContent = iniContent.substr(0, iniContent.find("End Line", 0));

			std::string::size_type locNext = line.find("%", 0);
			std::string subLine = line;
			int argCounter = 0;

			while (locNext != std::string::npos)
			{
				argCounter++;

				std::string currSectionOutput = subLine.substr(0, locNext);
				subLine = subLine.substr(locNext + 1);
				std::string currIdenti = subLine.substr(0, 3);

				int numBytes = atoi(currIdenti.substr(1, 1).c_str());
				std::string identifier = "%" + currIdenti.substr(0, 1);
				u32 readAddress;

				subLine = subLine.substr(3);

				std::string nextArgName = StringFromFormat("Arg%i", argCounter);

				std::string::size_type locNextArg = blockContent.find(nextArgName, 0);

				if (locNextArg == std::string::npos)
					break;

				std::string argString = blockContent.substr(locNextArg);
				argString = argString.substr(argString.find("=", 0) + 1);
				argString = argString.substr(0, argString.find(";", 0));

				std::string::size_type locPlus = argString.find("+", 0);
				std::string::size_type locHint = argString.find(">>", 0);

				std::string currHint;

				if (locHint != std::string::npos)
					currHint = argString.substr(locHint + 3);

				if (locPlus == std::string::npos)
				{
					std::string arguString;

					if (locHint != std::string::npos)
					{
						arguString = argString.substr(0, locHint - 1);
					}
					else
					{
						arguString = argString;
					}

					readAddress = strtol(arguString.c_str(), nullptr, 16);
				}
				else
				{
					u32 pointerAddress;
					u32 offset;

					pointerAddress = strtol(argString.substr(0, locPlus - 1).c_str(), nullptr, 16);

					std::string arguString = argString.substr(locPlus + 2);

					if (locHint != std::string::npos)
					{
						locHint = arguString.find(">>", 0);

						arguString = arguString.substr(0, locHint - 1);
					}

					offset = strtol(arguString.c_str(), nullptr, 16);

					u32 pointer = Memory::Read_U32(pointerAddress);

					if (pointer > 0x80000000)
					{
						pointer -= 0x80000000;

						readAddress = pointer + offset;
					}
					else
					{
						inputDisplay.append(currSectionOutput + "N/A");
						locNext = subLine.find("%", 0);
						continue;
					}

				}

				std::string finalOutput;

				if (identifier.compare("%s") == 0)
				{
					std::string outputString = Memory::Read_String(readAddress, numBytes);

					finalOutput = StringFromFormat(identifier.c_str(), outputString.c_str());
				}
				else if (identifier.compare("%f") == 0)
				{
					float outputFloat = Memory::Read_F32(readAddress);
					finalOutput = StringFromFormat(identifier.c_str(), outputFloat);
				}
				else if (numBytes == 4)
				{
					u32 output4Bytes = Memory::Read_U32(readAddress);
					finalOutput = StringFromFormat(identifier.c_str(), output4Bytes);
				}
				else if (numBytes == 2)
				{
					u16 output2Bytes = Memory::Read_U16(readAddress);

					//Special Formatting for 2 Byte
					if (currHint.compare("Degrees") == 0)
					{
						double degrees = output2Bytes;
						degrees = (degrees / 182.04) + 0.5;

						int finalDegrees = (int)degrees;

						if (finalDegrees >= 360)
							finalDegrees = finalDegrees - 360;

						std::string newIdentifier = identifier;
						newIdentifier.append(" (%i DEG)");

						finalOutput = StringFromFormat(newIdentifier.c_str(), output2Bytes, finalDegrees);
					}
					else if (currHint.compare("Time") == 0)
					{
						//ToD
						int time = output2Bytes;
						int hours = time / 256;

						float minutes = (float)time / 256;
						minutes = (minutes - hours) * 256;

						int finalMinutes = (int)minutes;

						std::stringstream ss;

						ss << finalMinutes;
						std::string minutesString = ss.str();

						if (finalMinutes < 10)
							minutesString = "0" + minutesString;

						std::string newIdentifier = identifier;
						newIdentifier.append(":%s");

						finalOutput = StringFromFormat(newIdentifier.c_str(), hours, minutesString.c_str());
					}
					else
					{
						finalOutput = StringFromFormat(identifier.c_str(), output2Bytes);
					}
				}
				else if (numBytes == 1)
				{
					u8 output1Byte = Memory::Read_U8(readAddress);
					finalOutput = StringFromFormat(identifier.c_str(), output1Byte);
				}

				if (finalOutput.length() == 0)
				{
					finalOutput = "N/A";
				}
				else
				{
					if (locHint == std::string::npos)
					{
						if (identifier.compare("%X") == 0 || identifier.compare("%x") == 0)
						{
							if (finalOutput.length() < 2)
							{
								finalOutput = "0" + finalOutput;
							}

						}
					}
				}

				std::string completeOutput = StringFromFormat("%s%s", currSectionOutput.c_str(), finalOutput.c_str());

				inputDisplay.append(completeOutput);

				locNext = subLine.find("%", 0);
			}

			inputDisplay.append("\n");
		}

	}

	/*
	u32 charPointerAddress;
	u32 stageAddress;
	u32 roomAddress;
	u32 stateAddress;
	u32 spawnAddress;
	u32 todAddress;
	u32 bossAddress;

	bool isTP = false;
	bool isTWW = false;

	if (!gameID.compare("GZ2E01"))
	{
	charPointerAddress = 0x3dce54;
	stageAddress = 0x40afc0;
	roomAddress = 0x42d3e0;
	stateAddress = 0x3a66b3;
	spawnAddress = 0x40afc9;
	todAddress = 0x3dc410;
	bossAddress = 0x450c98;

	isTP = true;
	}
	else if (!gameID.compare("GZ2P01"))
	{
	charPointerAddress = 0x3dedf4;
	stageAddress = 0x40cf60;
	roomAddress = 0x42f3a0;
	stateAddress = 0x3a8393;
	spawnAddress = 0x40cf69;
	todAddress = 0x3de3b0;
	bossAddress = 0x452c58;

	isTP = true;
	}
	else if (!gameID.compare("GZLJ01")) //JP TWW
	{
	charPointerAddress = 0x3ad860;
	stageAddress = 0x3bd23c;
	roomAddress = 0x3e9f48;
	stateAddress = 0x3b8000;
	spawnAddress = 0x3bd245;
	todAddress = 0x396230;
	bossAddress = 0x3bd3a2; //Event State

	isTWW = true;
	}

	//TP Stats NTSC/PAL
	if (isTP)
	{
	//Read Character Info (by Rachel)
	std::string strSpeed = "";
	u32 characterpointer = Memory::Read_U32(charPointerAddress);

	//std::string strSpeed2;
	//strSpeed2 = StringFromFormat("\nPoint 1: %d\n", characterpointer);
	//inputDisplay.append(strSpeed2);

	if (characterpointer > 0x80000000)
	{
	characterpointer -= 0x80000000;

	//u32 characterpointer2 = characterpointer + 0x5c;

	float speed = Memory::Read_F32(characterpointer + 0x5c);
	u16 facing = Memory::Read_U16(characterpointer + 0x16);

	double degrees = facing;
	degrees = (degrees / 182.04) + 0.5;

	int finalDegrees = (int)degrees;

	if (finalDegrees >= 360)
	finalDegrees = finalDegrees - 360;

	//strSpeed = StringFromFormat("\nPoint 2: %d | Point 3: %d | Speed: %f | Facing: %d (%i Deg)\n", characterpointer, characterpointer2, speed, facing, finalDegrees);
	strSpeed = StringFromFormat("\nSpeed: %f | Facing: %d (%i DEG)\n", speed, facing, finalDegrees);
	}
	else
	{
	strSpeed = "\nSpeed: N/A | Facing: N/A\n";
	}
	inputDisplay.append(strSpeed);

	//Read Map Info
	std::string strMap = "";

	std::string stageName = Memory::Read_String(stageAddress, 7);
	u8 roomID = Memory::Read_U8(roomAddress);

	std::string stateID = Memory::Read_String(stateAddress, 1);
	u8 spawnID = Memory::Read_U8(spawnAddress);

	int spawnTemp = spawnID;
	std::stringstream ss;

	ss << std::hex << spawnTemp;
	std::string spawnString = ss.str();

	std::transform(spawnString.begin(), spawnString.end(), spawnString.begin(), ::toupper);

	if (spawnString.length() < 2)
	spawnString = StringFromFormat("0%s", spawnString);

	if (stageName.find('_') == std::string::npos)
	stageName = "N/A";

	strMap = StringFromFormat("Stage: %s | Room: %d | State: %s | SpawnPoint: %s\n", stageName, roomID, stateID, spawnString);
	inputDisplay.append(strMap);

	//Misc Info (Boss Flag, ToD)
	std::string strMisc = "";

	//ToD
	u16 ToD = Memory::Read_U16(todAddress);
	int time = ToD;
	int hours = time / 256;

	float minutes = (float)time / 256;
	minutes = (minutes - hours) *256;

	int finalMinutes = (int)minutes;

	std::stringstream ss2;

	ss2 << finalMinutes;
	std::string minutesString = ss2.str();

	if (finalMinutes < 10)
	minutesString = "0" + minutesString;

	//Boss Flag
	u8 bossFlag = Memory::Read_U8(bossAddress);

	strMisc = StringFromFormat("ToD: %d:%s | BossFlag: %d\n", hours, minutesString, bossFlag);
	inputDisplay.append(strMisc);
	}

	//TWW Stats JP
	if (isTWW)
	{
	//Read Character Info
	std::string strSpeed = "";
	u32 characterpointer = Memory::Read_U32(charPointerAddress);

	//std::string strSpeed2;
	//strSpeed2 = StringFromFormat("\nPoint 1: %d\n", characterpointer);
	//inputDisplay.append(strSpeed2);

	if (characterpointer > 0x80000000)
	{
	characterpointer -= 0x80000000;

	//u32 characterpointer2 = characterpointer + 0x5c;

	float speed = Memory::Read_F32(characterpointer + 0x34E4);
	u16 facing = Memory::Read_U16(0x3ea3d2);

	double degrees = facing;
	degrees = (degrees / 182.04) + 0.5;

	int finalDegrees = (int)degrees;

	if (finalDegrees >= 360)
	finalDegrees = finalDegrees - 360;

	//strSpeed = StringFromFormat("\nPoint 2: %d | Point 3: %d | Speed: %f | Facing: %d (%i Deg)\n", characterpointer, characterpointer2, speed, facing, finalDegrees);
	strSpeed = StringFromFormat("\nSpeed: %f | Facing: %d (%i DEG)\n", speed, facing, finalDegrees);
	}
	else
	{
	strSpeed = "\nSpeed: N/A | Facing: N/A\n";
	}
	inputDisplay.append(strSpeed);

	//Read Map Info
	std::string strMap = "";

	std::string stageName = Memory::Read_String(stageAddress, 7);
	u8 roomID = Memory::Read_U8(roomAddress);

	u8 stateID = Memory::Read_U8(stateAddress);
	u8 spawnID = Memory::Read_U8(spawnAddress);

	int spawnTemp = spawnID;
	std::stringstream ss;

	ss << std::hex << spawnTemp;
	std::string spawnString = ss.str();

	std::transform(spawnString.begin(), spawnString.end(), spawnString.begin(), ::toupper);

	if (spawnString.length() < 2)
	spawnString = StringFromFormat("0%s", spawnString);

	if (stageName.length() < 1)
	stageName = "N/A";


	int stateTemp = stateID;
	std::stringstream ss2;

	ss2 << std::hex << stateTemp;
	std::string layerString = ss2.str();

	if (layerString.length() > 1)
	layerString = layerString.substr(1);


	strMap = StringFromFormat("Stage: %s | Room: %d | Layer: %s | SpawnPoint: %s\n", stageName, roomID, layerString, spawnString);
	inputDisplay.append(strMap);

	//Misc Info (ToD)
	std::string strMisc = "";

	//ToD
	u16 ToD = Memory::Read_U16(todAddress);
	int time = ToD;
	int hours = time / 256;

	float minutes = (float)time / 256;
	minutes = (minutes - hours) * 256;

	int finalMinutes = (int)minutes;

	std::stringstream ss3;

	ss3 << finalMinutes;
	std::string minutesString = ss3.str();

	if (finalMinutes < 10)
	minutesString = "0" + minutesString;

	//Event State
	u8 eventState = Memory::Read_U8(bossAddress);

	strMisc = StringFromFormat("ToD: %d:%s | Event State: %d\n", hours, minutesString, eventState);
	inputDisplay.append(strMisc);
	}
	*/

	return inputDisplay;
}

void FrameUpdate()
{
	// TODO[comex]: This runs on the GPU thread, yet it messes with the CPU
	// state directly.  That's super sketchy.

	g_currentFrame++;
	cmp_curentBranchFrame++; // Dragonbane

	if (!s_bPolled)
		g_currentLagCount++;

	if (IsRecordingInput())
	{
		g_totalFrames = g_currentFrame;
		//s_totalLagCount = g_currentLagCount;

		//Dragonbane
		if (g_currentLagCount > 0)
		{
			Core::DisplayMessage("Lag occurred during recording. Desync incoming?", 10000);
			s_totalLagCount += g_currentLagCount;
			g_currentLagCount = 0;
		}
	}
	if (s_bFrameStep)
	{
		Core::SetState(Core::CORE_PAUSE);
		s_bFrameStep = false;
	}

	// ("framestop") the only purpose of this is to cause interpreter/jit Run() to return temporarily.
	// after that we set it back to CPU_RUNNING and continue as normal.
	if (s_bFrameStop)
		*PowerPC::GetStatePtr() = PowerPC::CPU_STEPPING;

	if (s_framesToSkip)
		FrameSkipping();

	s_bPolled = false;

	//Dragonbane
	if (cmp_isRunning && !cmp_movieFinished && IsPlayingInput() && g_currentFrame == (g_totalFrames - 1))
	{
		if (!GetNextComparisonMovie(false)) //Entire side is finished, stop dumping
		{
			SConfig::GetInstance().m_DumpFrames = false;
			SConfig::GetInstance().m_DumpAudio = false;
		}

		cmp_movieFinished = true;
	}
}

// called when game is booting up, even if no movie is active,
// but potentially after BeginRecordingInput or PlayInput has been called.
void Init()
{
	s_bPolled = false;
	s_bFrameStep = false;
	s_bFrameStop = false;
	s_bSaveConfig = false;
	justStoppedRecording = false; //Dragonbane
	s_iCPUCore = SConfig::GetInstance().iCPUCore;
	if (IsPlayingInput())
	{
		ReadHeader();
		std::thread md5thread(CheckMD5);
		md5thread.detach();
		if (strncmp((char *)tmpHeader.gameID, SConfig::GetInstance().GetUniqueID().c_str(), 6))
		{
			PanicAlertT("The recorded game (%s) is not the same as the selected game (%s)", tmpHeader.gameID, SConfig::GetInstance().GetUniqueID().c_str());
			EndPlayInput(false);
		}
	}

	if (IsRecordingInput())
	{
		GetSettings();
		std::thread md5thread(GetMD5);
		md5thread.detach();
		s_tickCountAtLastInput = 0;
	}

	s_frameSkipCounter = s_framesToSkip;
	memset(&s_padState, 0, sizeof(s_padState));
	if (!tmpHeader.bFromSaveState || !IsPlayingInput())
		Core::SetStateFileName("");

	for (auto& disp : s_InputDisplay)
		disp.clear();

	if (!IsMovieActive())
	{
		s_bRecordingFromSaveState = false;
		s_rerecords = 0;
		s_currentByte = 0;
		g_currentFrame = 0;
		g_currentLagCount = 0;
		g_currentInputCount = 0;

		cmp_curentBranchFrame = 0; //Dragonbane
	}
}

void InputUpdate()
{
	g_currentInputCount++;
	if (IsRecordingInput())
	{
		g_totalInputCount = g_currentInputCount;
		s_totalTickCount += CoreTiming::GetTicks() - s_tickCountAtLastInput;
		s_tickCountAtLastInput = CoreTiming::GetTicks();
	}

	if (IsPlayingInput() && g_currentInputCount == (g_totalInputCount - 1) && SConfig::GetInstance().m_PauseMovie)
		Core::SetState(Core::CORE_PAUSE);
}

void SetFrameSkipping(unsigned int framesToSkip)
{
	std::lock_guard<std::mutex> lk(cs_frameSkip);

	s_framesToSkip = framesToSkip;
	s_frameSkipCounter = 0;

	// Don't forget to re-enable rendering in case it wasn't...
	// as this won't be changed anymore when frameskip is turned off
	if (framesToSkip == 0)
		g_video_backend->Video_SetRendering(true);
}

void SetPolledDevice()
{
	s_bPolled = true;
}

void DoFrameStep()
{
	if (Core::GetState() == Core::CORE_PAUSE)
	{
		// if already paused, frame advance for 1 frame
		Core::SetState(Core::CORE_RUN);
		Core::RequestRefreshInfo();
		s_bFrameStep = true;
	}
	else if (!s_bFrameStep)
	{
		// if not paused yet, pause immediately instead
		Core::SetState(Core::CORE_PAUSE);
	}
}

void SetFrameStopping(bool bEnabled)
{
	s_bFrameStop = bEnabled;
}

void SetReadOnly(bool bEnabled)
{
	if (s_bReadOnly != bEnabled)
		Core::DisplayMessage(bEnabled ? "Read-only mode." :  "Read+Write mode.", 1000);

	s_bReadOnly = bEnabled;
}

void FrameSkipping()
{
	// Frameskipping will desync movie playback
	if (!IsMovieActive() || NetPlay::IsNetPlayRunning())
	{
		std::lock_guard<std::mutex> lk(cs_frameSkip);

		s_frameSkipCounter++;
		if (s_frameSkipCounter > s_framesToSkip || Core::ShouldSkipFrame(s_frameSkipCounter) == false)
			s_frameSkipCounter = 0;

		g_video_backend->Video_SetRendering(!s_frameSkipCounter);
	}
}

bool IsRecordingInput()
{
	return (s_playMode == MODE_RECORDING);
}

bool IsRecordingInputFromSaveState()
{
	return s_bRecordingFromSaveState;
}

bool IsJustStartingRecordingInputFromSaveState()
{
	return IsRecordingInputFromSaveState() && g_currentFrame == 0;
}

bool IsJustStartingPlayingInputFromSaveState()
{
	return IsRecordingInputFromSaveState() && g_currentFrame == 1 && IsPlayingInput();
}

bool IsPlayingInput()
{
	return (s_playMode == MODE_PLAYING);
}

bool IsMovieActive()
{
	return s_playMode != MODE_NONE;
}

bool IsReadOnly()
{
	return s_bReadOnly;
}

u64 GetRecordingStartTime()
{
	return s_recordingStartTime;
}

bool IsUsingPad(int controller)
{
	return ((s_numPads & (1 << controller)) != 0);
}

bool IsUsingBongo(int controller)
{
	return ((s_bongos & (1 << controller)) != 0);
}

//Dragonbane
bool IsUsingGBA(int controller)
{
	return ((s_numGBAs & (1 << controller)) != 0);
}


bool IsUsingWiimote(int wiimote)
{
	return ((s_numPads & (1 << (wiimote + 4))) != 0);
}

bool IsConfigSaved()
{
	return s_bSaveConfig;
}
bool IsDualCore()
{
	return s_bDualCore;
}

bool IsProgressive()
{
	return s_bProgressive;
}

bool IsPAL60()
{
	return s_bPAL60;
}

bool IsSkipIdle()
{
	return s_bSkipIdle;
}

bool IsDSPHLE()
{
	return s_bDSPHLE;
}

bool IsFastDiscSpeed()
{
	return s_bFastDiscSpeed;
}

int GetCPUMode()
{
	return s_iCPUCore;
}

bool IsStartingFromClearSave()
{
	return g_bClearSave;
}

bool IsUsingMemcard(int memcard)
{
	return (s_memcards & (1 << memcard)) != 0;
}
bool IsSyncGPU()
{
	return s_bSyncGPU;
}

bool IsNetPlayRecording()
{
	return s_bNetPlay;
}

void ChangePads(bool instantly)
{
	if (!Core::IsRunning())
		return;

	int controllers = 0;

	for (int i = 0; i < MAX_SI_CHANNELS; ++i)
		if (SConfig::GetInstance().m_SIDevice[i] == SIDEVICE_GC_CONTROLLER || SConfig::GetInstance().m_SIDevice[i] == SIDEVICE_GC_TARUKONGA)
			controllers |= (1 << i);

	if (instantly && (s_numPads & 0x0F) == controllers)
		return;

	for (int i = 0; i < MAX_SI_CHANNELS; ++i)
		if (instantly) // Changes from savestates need to be instantaneous
			SerialInterface::AddDevice(IsUsingPad(i) ? (IsUsingBongo(i) ? SIDEVICE_GC_TARUKONGA : SIDEVICE_GC_CONTROLLER) : SIDEVICE_NONE, i);
		else
			SerialInterface::ChangeDevice(IsUsingPad(i) ? (IsUsingBongo(i) ? SIDEVICE_GC_TARUKONGA : SIDEVICE_GC_CONTROLLER) : SIDEVICE_NONE, i);
}

void ChangeWiiPads(bool instantly)
{
	int controllers = 0;

	for (int i = 0; i < MAX_WIIMOTES; ++i)
		if (g_wiimote_sources[i] != WIIMOTE_SRC_NONE)
			controllers |= (1 << i);

	// This is important for Wiimotes, because they can desync easily if they get re-activated
	if (instantly && (s_numPads >> 4) == controllers)
		return;

	for (int i = 0; i < MAX_WIIMOTES; ++i)
	{
		g_wiimote_sources[i] = IsUsingWiimote(i) ? WIIMOTE_SRC_EMU : WIIMOTE_SRC_NONE;
		GetUsbPointer()->AccessWiiMote(i | 0x100)->Activate(IsUsingWiimote(i));
	}
}

bool BeginRecordingInput(int controllers)
{
	if (s_playMode != MODE_NONE || controllers == 0)
		return false;

	bool was_unpaused = Core::PauseAndLock(true);

	justStoppedRecording = false; //Dragonbane
	tunerActionID = 0;
	tunerExecuteID = 0;

	s_numPads = controllers;
	g_currentFrame = g_totalFrames = 0;
	g_currentLagCount = s_totalLagCount = 0;
	g_currentInputCount = g_totalInputCount = 0;
	s_totalTickCount = s_tickCountAtLastInput = 0;

	s_bongos = 0;
	s_numGBAs = 0; //Dragonbane
	s_memcards = 0;
	if (NetPlay::IsNetPlayRunning())
	{
		s_bNetPlay = true;
		s_recordingStartTime = g_netplay_initial_gctime;
	}
	else
	{
		s_recordingStartTime = Common::Timer::GetLocalTimeSinceJan1970();
	}

	s_rerecords = 0;
	s_currentByte = s_totalBytes = 0; //Dragonbane: Move this here so savestate doesnt get wrong information when recording multiple movies

	for (int i = 0; i < MAX_SI_CHANNELS; ++i)
		if (SConfig::GetInstance().m_SIDevice[i] == SIDEVICE_GC_TARUKONGA)
			s_bongos |= (1 << i);
		else if(SConfig::GetInstance().m_SIDevice[i] == SIDEVICE_GC_GBA) //Dragonbane
			s_numGBAs |= (1 << i);

	if (Core::IsRunningAndStarted())
	{
		const std::string save_path = File::GetUserPath(D_STATESAVES_IDX) + "dtm.sav";
		if (File::Exists(save_path))
			File::Delete(save_path);

		saveMemCard = true; //Dragonbane

		State::SaveAs(save_path);
		s_bRecordingFromSaveState = true;

		//Dragonbane
		lastMovie = "";
		saveMemCard = false;

		// This is only done here if starting from save state because otherwise we won't have the titleid. Otherwise it's set in WII_IPC_HLE_Device_es.cpp.
		// TODO: find a way to GetTitleDataPath() from Movie::Init()
		if (SConfig::GetInstance().bWii)
		{
			if (File::Exists(Common::GetTitleDataPath(g_titleID, Common::FROM_SESSION_ROOT) + "banner.bin"))
				Movie::g_bClearSave = false;
			else
				Movie::g_bClearSave = true;
		}
		std::thread md5thread(GetMD5);
		md5thread.detach();
		GetSettings();
	}
	s_playMode = MODE_RECORDING;
	s_author = SConfig::GetInstance().m_strMovieAuthor;
	EnsureTmpInputSize(1);

	Core::UpdateWantDeterminism();

	Core::PauseAndLock(false, was_unpaused);


	//Dragonbane: Create Dynamic Range
	/*
	std::string gameID = SConfig::GetInstance().m_LocalCoreStartupParameter.GetUniqueID();

	u8* newTmpInput = new u8[6316032];
	if (tmpDynamicRange != nullptr)
	{
	delete[] tmpDynamicRange;
	}
	tmpDynamicRange = newTmpInput;

	memset(&(tmpDynamicRange[0]), 0, 6316032);

	if (!gameID.compare("GZ2E01") || !gameID.compare("GZ2P01"))
	{
	const u8* const memptr = Memory::m_pRAM;
	if (memptr != nullptr && Core::IsRunningAndStarted())
	{
	memcpy(&(tmpDynamicRange[0]), memptr + 0xa0a000, 6316032);
	}
	}
	*/

	badSettings = true;

	Core::DisplayMessage("Starting movie recording", 2000);
	return true;
}

static std::string Analog2DToString(u8 x, u8 y, const std::string& prefix, u8 range = 255)
{
	u8 center = range / 2 + 1;
	if ((x <= 1 || x == center || x >= range) &&
	    (y <= 1 || y == center || y >= range))
	{
		if (x != center || y != center)
		{
			if (x != center && y != center)
			{
				return StringFromFormat("%s:%s,%s", prefix.c_str(), x < center ? "LEFT" : "RIGHT", y < center ? "DOWN" : "UP");
			}
			else if (x != center)
			{
				return StringFromFormat("%s:%s", prefix.c_str(), x < center ? "LEFT" : "RIGHT");
			}
			else
			{
				return StringFromFormat("%s:%s", prefix.c_str(), y < center ? "DOWN" : "UP");
			}
		}
		else
		{
			return "";
		}
	}
	else
	{
		return StringFromFormat("%s:%d,%d", prefix.c_str(), x, y);
	}
}

static std::string Analog1DToString(u8 v, const std::string& prefix, u8 range = 255)
{
	if (v > 0)
	{
		if (v == range)
		{
			return prefix;
		}
		else
		{
			return StringFromFormat("%s:%d", prefix.c_str(), v);
		}
	}
	else
	{
		return "";
	}
}

static void SetInputDisplayString(ControllerState padState, int controllerID)
{
	s_InputDisplay[controllerID] = StringFromFormat("P%d:", controllerID + 1);

	if (padState.A)
		s_InputDisplay[controllerID].append(" A");
	if (padState.B)
		s_InputDisplay[controllerID].append(" B");
	if (padState.X)
		s_InputDisplay[controllerID].append(" X");
	if (padState.Y)
		s_InputDisplay[controllerID].append(" Y");
	if (padState.Z)
		s_InputDisplay[controllerID].append(" Z");
	if (padState.Start)
		s_InputDisplay[controllerID].append(" START");

	if (padState.DPadUp)
		s_InputDisplay[controllerID].append(" UP");
	if (padState.DPadDown)
		s_InputDisplay[controllerID].append(" DOWN");
	if (padState.DPadLeft)
		s_InputDisplay[controllerID].append(" LEFT");
	if (padState.DPadRight)
		s_InputDisplay[controllerID].append(" RIGHT");
	if (padState.reset)
		s_InputDisplay[controllerID].append(" RESET");

	s_InputDisplay[controllerID].append(Analog1DToString(padState.TriggerL, " L"));
	s_InputDisplay[controllerID].append(Analog1DToString(padState.TriggerR, " R"));
	s_InputDisplay[controllerID].append(Analog2DToString(padState.AnalogStickX, padState.AnalogStickY, " ANA"));
	s_InputDisplay[controllerID].append(Analog2DToString(padState.CStickX, padState.CStickY, " C"));
	s_InputDisplay[controllerID].append("\n");
}

static void SetWiiInputDisplayString(int remoteID, u8* const data, const WiimoteEmu::ReportFeatures& rptf, int ext, const wiimote_key key)
{
	int controllerID = remoteID + 4;

	s_InputDisplay[controllerID] = StringFromFormat("R%d:", remoteID + 1);

	u8* const coreData = rptf.core ? (data + rptf.core) : nullptr;
	u8* const accelData = rptf.accel ? (data + rptf.accel) : nullptr;
	u8* const irData = rptf.ir ? (data + rptf.ir) : nullptr;
	u8* const extData = rptf.ext ? (data + rptf.ext) : nullptr;

	if (coreData)
	{
		wm_buttons buttons = *(wm_buttons*)coreData;
		if(buttons.left)
			s_InputDisplay[controllerID].append(" LEFT");
		if(buttons.right)
			s_InputDisplay[controllerID].append(" RIGHT");
		if(buttons.down)
			s_InputDisplay[controllerID].append(" DOWN");
		if(buttons.up)
			s_InputDisplay[controllerID].append(" UP");
		if(buttons.a)
			s_InputDisplay[controllerID].append(" A");
		if(buttons.b)
			s_InputDisplay[controllerID].append(" B");
		if(buttons.plus)
			s_InputDisplay[controllerID].append(" +");
		if(buttons.minus)
			s_InputDisplay[controllerID].append(" -");
		if(buttons.one)
			s_InputDisplay[controllerID].append(" 1");
		if(buttons.two)
			s_InputDisplay[controllerID].append(" 2");
		if(buttons.home)
			s_InputDisplay[controllerID].append(" HOME");
	}

	if (accelData)
	{
		wm_accel* dt = (wm_accel*)accelData;
		std::string accel = StringFromFormat(" ACC:%d,%d,%d",
			dt->x << 2 | ((wm_buttons*)coreData)->acc_x_lsb, dt->y << 2 | ((wm_buttons*)coreData)->acc_y_lsb << 1, dt->z << 2 | ((wm_buttons*)coreData)->acc_z_lsb << 1);
		s_InputDisplay[controllerID].append(accel);
	}

	if (irData)
	{
		u16 x = irData[0] | ((irData[2] >> 4 & 0x3) << 8);
		u16 y = irData[1] | ((irData[2] >> 6 & 0x3) << 8);
		std::string ir = StringFromFormat(" IR:%d,%d", x,y);
		s_InputDisplay[controllerID].append(ir);
	}

	// Nunchuk
	if (extData && ext == 1)
	{
		wm_nc nunchuk;
		memcpy(&nunchuk, extData, sizeof(wm_nc));
		WiimoteDecrypt(&key, (u8*)&nunchuk, 0, sizeof(wm_nc));
		nunchuk.bt.hex = nunchuk.bt.hex ^ 0x3;

		std::string accel = StringFromFormat(" N-ACC:%d,%d,%d",
			(nunchuk.ax << 2) | nunchuk.bt.acc_x_lsb, (nunchuk.ay << 2) | nunchuk.bt.acc_y_lsb, (nunchuk.az << 2) | nunchuk.bt.acc_z_lsb);

		if (nunchuk.bt.c)
			s_InputDisplay[controllerID].append(" C");
		if (nunchuk.bt.z)
			s_InputDisplay[controllerID].append(" Z");
		s_InputDisplay[controllerID].append(accel);
		s_InputDisplay[controllerID].append(Analog2DToString(nunchuk.jx, nunchuk.jy, " ANA"));
	}

	// Classic controller
	if (extData && ext == 2)
	{
		wm_classic_extension cc;
		memcpy(&cc, extData, sizeof(wm_classic_extension));
		WiimoteDecrypt(&key, (u8*)&cc, 0, sizeof(wm_classic_extension));
		cc.bt.hex = cc.bt.hex ^ 0xFFFF;

		if (cc.bt.regular_data.dpad_left)
			s_InputDisplay[controllerID].append(" LEFT");
		if (cc.bt.dpad_right)
			s_InputDisplay[controllerID].append(" RIGHT");
		if (cc.bt.dpad_down)
			s_InputDisplay[controllerID].append(" DOWN");
		if (cc.bt.regular_data.dpad_up)
			s_InputDisplay[controllerID].append(" UP");
		if (cc.bt.a)
			s_InputDisplay[controllerID].append(" A");
		if (cc.bt.b)
			s_InputDisplay[controllerID].append(" B");
		if (cc.bt.x)
			s_InputDisplay[controllerID].append(" X");
		if (cc.bt.y)
			s_InputDisplay[controllerID].append(" Y");
		if (cc.bt.zl)
			s_InputDisplay[controllerID].append(" ZL");
		if (cc.bt.zr)
			s_InputDisplay[controllerID].append(" ZR");
		if (cc.bt.plus)
			s_InputDisplay[controllerID].append(" +");
		if (cc.bt.minus)
			s_InputDisplay[controllerID].append(" -");
		if (cc.bt.home)
			s_InputDisplay[controllerID].append(" HOME");

		s_InputDisplay[controllerID].append(Analog1DToString(cc.lt1 | (cc.lt2 << 3), " L", 31));
		s_InputDisplay[controllerID].append(Analog1DToString(cc.rt, " R", 31));
		s_InputDisplay[controllerID].append(Analog2DToString(cc.regular_data.lx, cc.regular_data.ly, " ANA", 63));
		s_InputDisplay[controllerID].append(Analog2DToString(cc.rx1 | (cc.rx2 << 1) | (cc.rx3 << 3), cc.ry, " R-ANA", 31));
	}

	s_InputDisplay[controllerID].append("\n");
}

void CheckPadStatus(GCPadStatus* PadStatus, int controllerID)
{
	s_padState.A         = ((PadStatus->button & PAD_BUTTON_A) != 0);
	s_padState.B         = ((PadStatus->button & PAD_BUTTON_B) != 0);
	s_padState.X         = ((PadStatus->button & PAD_BUTTON_X) != 0);
	s_padState.Y         = ((PadStatus->button & PAD_BUTTON_Y) != 0);
	s_padState.Z         = ((PadStatus->button & PAD_TRIGGER_Z) != 0);
	s_padState.Start     = ((PadStatus->button & PAD_BUTTON_START) != 0);

	s_padState.DPadUp    = ((PadStatus->button & PAD_BUTTON_UP) != 0);
	s_padState.DPadDown  = ((PadStatus->button & PAD_BUTTON_DOWN) != 0);
	s_padState.DPadLeft  = ((PadStatus->button & PAD_BUTTON_LEFT) != 0);
	s_padState.DPadRight = ((PadStatus->button & PAD_BUTTON_RIGHT) != 0);

	s_padState.L         = ((PadStatus->button & PAD_TRIGGER_L) != 0);
	s_padState.R         = ((PadStatus->button & PAD_TRIGGER_R) != 0);
	s_padState.TriggerL  = PadStatus->triggerLeft;
	s_padState.TriggerR  = PadStatus->triggerRight;

	s_padState.AnalogStickX = PadStatus->stickX;
	s_padState.AnalogStickY = PadStatus->stickY;

	s_padState.CStickX   = PadStatus->substickX;
	s_padState.CStickY   = PadStatus->substickY;

	s_padState.disc = g_bDiscChange;
	g_bDiscChange = false;
	s_padState.reset = g_bReset;
	g_bReset = false;


	//Dragonbane: Record Tuner Events
	s_padState.tunerEvent = tunerActionID;

	if (tunerActionID > 0)
		tunerExecuteID = tunerActionID;

	tunerActionID = 0;

	//Dragonbane
	std::string gameID = SConfig::GetInstance().GetUniqueID();
	u32 isLoadingAdd;
	u32 charPointerAddress;

	bool isTP = false;

	if (!gameID.compare("GZ2E01"))
	{
		charPointerAddress = 0x3dce54;
		isLoadingAdd = 0x450ce0;

		isTP = true;
	}
	else if (!gameID.compare("GZ2P01"))
	{
		charPointerAddress = 0x3dedf4;
		isLoadingAdd = 0x452ca0;

		isTP = true;
	}

	//TP Stuff
	if (isTP)
	{
		u32 characterpointer = Memory::Read_U32(charPointerAddress);
		u32 isLoading = Memory::Read_U32(isLoadingAdd);

		if (isLoading > 0)
			s_padState.loading = true;
		else
			s_padState.loading = false;

		if (characterpointer > 0x80000000 && isLoading == 0)
		{
			characterpointer -= 0x80000000;

			s_padState.LinkX = Memory::Read_F32(characterpointer);
			s_padState.LinkZ = Memory::Read_F32(characterpointer + 0x8);
		}
	}


	//TWW Stuff
	bool isTWW = false;

	if (!gameID.compare("GZLJ01"))
	{
		isLoadingAdd = 0x3ad335;

		isTWW = true;
	}

	if (isTWW)
	{
		u8 isLoading = Memory::Read_U8(isLoadingAdd);

		if (isLoading > 0)
			s_padState.loading = true;
		else
			s_padState.loading = false;

		if (isLoading == 0)
		{
			u32 XAdd = 0x3d78fc;
			u32 ZAdd = 0x3d7904;

			s_padState.LinkX = Memory::Read_F32(XAdd);
			s_padState.LinkZ = Memory::Read_F32(ZAdd);
		}
	}


	SetInputDisplayString(s_padState, controllerID);
}

void RecordInput(GCPadStatus* PadStatus, int controllerID)
{
	if (!IsRecordingInput() || !IsUsingPad(controllerID))
		return;

	CheckPadStatus(PadStatus, controllerID);

	//Dragonbane: Check for bad settings
	if (badSettings)
	{
		if (SConfig::GetInstance().m_OCEnable || SConfig::GetInstance().bDSPHLE)
		{
			PanicAlertT("Your settings are not recommended for recording and will likely cause desync!\nPlease disable CPU Overclocking and use LLE audio.");
		}
		else if (SConfig::GetInstance().bCPUThread)
		{
			PanicAlertT("Info: Please disable Dual Core to prevent desync issues!");
		}
		badSettings = false;
	}

	//Dragonbane: New dangerous stuff
	/*
	std::string gameID = SConfig::GetInstance().m_LocalCoreStartupParameter.GetUniqueID();
	u32 isLoadingAdd;
	u32 charPointerAddress;

	bool isTP = false;

	if (!gameID.compare("GZ2E01"))
	{
	charPointerAddress = 0x3dce54;
	isLoadingAdd = 0x450ce0;

	isTP = true;
	}
	else if (!gameID.compare("GZ2P01"))
	{
	charPointerAddress = 0x3dedf4;
	isLoadingAdd = 0x452ca0;

	isTP = true;
	}

	bool updatedMemory = false;

	//TP Stuff
	if (isTP)
	{
	u32 characterpointer = Memory::Read_U32(charPointerAddress);
	u32 isLoading = Memory::Read_U32(isLoadingAdd);

	if (characterpointer > 0x80000000 && isLoading == 0)
	{
	characterpointer -= 0x80000000;

	const u8* const memptr = Memory::m_pRAM;
	//Memory::RAM_SIZE
	if (memptr != nullptr )
	{
	u8* newMemory;
	newMemory = new u8[12632068]; //6316032 * 2 + 4
	u32 newMemPos = 4;
	u32 countChanges = 0;
	u32 actualSize;

	int show = 0;

	for (u32 i = 0; i < 6316032 - 4; i += 4)
	{
	if (memcmp(&(tmpDynamicRange[i]), (memptr + 0xa0a000) + i, 4) != 0)
	{
	countChanges += 1;

	/*
	if (show < 3)
	{
	PanicAlertT("i:%d ; New Mem Pos: %d", i, newMemPos);
	show++;
	}
	*/
	/*

	memcpy(&(newMemory[newMemPos]), &i, 4);
	memcpy(&(newMemory[newMemPos + 4]), (memptr + 0xa0a000) + i, 4);

	memcpy(&(tmpDynamicRange[i]), (memptr + 0xa0a000) + i, 4);

	newMemPos += 8;
	}
	}

	if (countChanges > 0)
	{
	//PanicAlertT("Amount of changes:%d", countChanges);

	memcpy(&(newMemory[0]), &countChanges, 4);

	actualSize = 20 + (countChanges * 8);   //16 + 4

	EnsureTmpInputSize((size_t)(s_currentByte + actualSize)); //Dragonbane: 16
	memcpy(&(tmpInput[s_currentByte]), &s_padState, 16);

	memcpy(&(tmpInput[s_currentByte + 16]), &(newMemory[0]), actualSize - 16);

	s_currentByte += actualSize;

	updatedMemory = true;
	}

	//Clean Memory
	delete[] newMemory;
	newMemory = nullptr;
	}
	}
	}
	*/

	//if (!updatedMemory)
	//{
		EnsureTmpInputSize((size_t)(s_currentByte + 17)); //Dragonbane: 17
		memcpy(&(tmpInput[s_currentByte]), &s_padState, 17);

		//memset(&(tmpInput[s_currentByte+16]), 0, 4);
		s_currentByte += 17;
	//}

	s_totalBytes = s_currentByte;
}

void CheckWiimoteStatus(int wiimote, u8 *data, const WiimoteEmu::ReportFeatures& rptf, int ext, const wiimote_key key)
{
	SetWiiInputDisplayString(wiimote, data, rptf, ext, key);

	if (IsRecordingInput())
		RecordWiimote(wiimote, data, rptf.size);
}

void RecordWiimote(int wiimote, u8 *data, u8 size)
{
	if (!IsRecordingInput() || !IsUsingWiimote(wiimote))
		return;

	InputUpdate();
	EnsureTmpInputSize((size_t)(s_currentByte + size + 1));
	tmpInput[s_currentByte++] = size;
	memcpy(&(tmpInput[s_currentByte]), data, size);
	s_currentByte += size;
	s_totalBytes = s_currentByte;
}

void ReadHeader()
{
	s_numPads = tmpHeader.numControllers;
	s_recordingStartTime = tmpHeader.recordingStartTime;
	if (s_rerecords < tmpHeader.numRerecords)
		s_rerecords = tmpHeader.numRerecords;

	if (tmpHeader.bSaveConfig)
	{
		s_bSaveConfig = true;
		s_bSkipIdle = tmpHeader.bSkipIdle;
		s_bDualCore = tmpHeader.bDualCore;
		s_bProgressive = tmpHeader.bProgressive;
		s_bPAL60 = tmpHeader.bPAL60;
		s_bDSPHLE = tmpHeader.bDSPHLE;
		s_bFastDiscSpeed = tmpHeader.bFastDiscSpeed;
		s_iCPUCore = tmpHeader.CPUCore;
		g_bClearSave = tmpHeader.bClearSave;
		s_memcards = tmpHeader.memcards;
		s_bongos = tmpHeader.bongos;
		s_numGBAs = tmpHeader.numGBAs; //Dragonbane
		s_bSyncGPU = tmpHeader.bSyncGPU;
		s_bNetPlay = tmpHeader.bNetPlay;
		memcpy(s_revision, tmpHeader.revision, ArraySize(s_revision));
	}
	else
	{
		GetSettings();
	}

	s_videoBackend = (char*) tmpHeader.videoBackend;
	g_discChange = (char*) tmpHeader.discChange;
	s_author = (char*) tmpHeader.author;
	memcpy(s_MD5, tmpHeader.md5, 16);
	s_DSPiromHash = tmpHeader.DSPiromHash;
	s_DSPcoefHash = tmpHeader.DSPcoefHash;
}

bool PlayInput(const std::string& filename)
{
	if (s_playMode != MODE_NONE)
		return false;

	if (!File::Exists(filename))
		return false;

	File::IOFile g_recordfd;

	if (!g_recordfd.Open(filename, "rb"))
		return false;

	g_recordfd.ReadArray(&tmpHeader, 1);

	if (!IsMovieHeader(tmpHeader.filetype))
	{
		PanicAlertT("Invalid recording file");
		g_recordfd.Close();
		return false;
	}

	ReadHeader();
	g_totalFrames = tmpHeader.frameCount;
	s_totalLagCount = tmpHeader.lagCount;
	g_totalInputCount = tmpHeader.inputCount;
	s_totalTickCount = tmpHeader.tickCount;

	g_currentFrame = 0;
	g_currentLagCount = 0;
	g_currentInputCount = 0;

	s_playMode = MODE_PLAYING;

	isVerifying = false; //Dragonbane
	isAutoVerifying = false;
	justStoppedRecording = false;
	tunerActionID = 0;
	tunerExecuteID = 0;

	Core::UpdateWantDeterminism();

	s_totalBytes = g_recordfd.GetSize() - 256;
	EnsureTmpInputSize((size_t)s_totalBytes);
	g_recordfd.ReadArray(tmpInput, (size_t)s_totalBytes);
	s_currentByte = 0;
	g_recordfd.Close();

	// Load savestate (and skip to frame data)
	if (tmpHeader.bFromSaveState)
	{
		const std::string stateFilename = filename + ".sav";
		if (File::Exists(stateFilename))
			Core::SetStateFileName(stateFilename);
		s_bRecordingFromSaveState = true;
		Movie::LoadInput(filename);
	}

	//Make sure Auto Roll is off for playback/verification
	roll_enabled = false;
	checkSave = false;
	uncheckSave = true;

	//Core::SetIsFramelimiterTempDisabled(true);


	//Dragonbane: Create Dynamic Range
	/*
	std::string gameID = SConfig::GetInstance().m_LocalCoreStartupParameter.GetUniqueID();

	u8* newTmpInput = new u8[6316032];
	if (tmpDynamicRange != nullptr)
	{
	delete[] tmpDynamicRange;
	}
	tmpDynamicRange = newTmpInput;

	memset(&(tmpDynamicRange[0]), 0, 6316032);

	if (!gameID.compare("GZ2E01") || !gameID.compare("GZ2P01"))
	{
	const u8* const memptr = Memory::m_pRAM;
	if (memptr != nullptr && Core::IsRunningAndStarted())
	{
	memcpy(&(tmpDynamicRange[0]), memptr + 0xa0a000, 6316032);
	}
	}
	*/

	return true;
}

void DoState(PointerWrap &p)
{
	// many of these could be useful to save even when no movie is active,
	// and the data is tiny, so let's just save it regardless of movie state.
	p.Do(g_currentFrame);
	p.Do(s_currentByte);
	p.Do(g_currentLagCount);
	p.Do(g_currentInputCount);
	p.Do(s_bPolled);
	p.Do(s_tickCountAtLastInput);
	// other variables (such as s_totalBytes and g_totalFrames) are set in LoadInput

	//Dragonbane: Save Roll Stuff
	p.Do(roll_enabled);
	p.Do(first_roll);
	p.Do(roll_timer);

	if (roll_enabled)
	{
		checkSave = true;
		uncheckSave = false;
	}
	else
	{
		uncheckSave = true;
		checkSave = false;
	}

	//Dragonbane: Save current Tuner Action ID
	p.Do(tunerExecuteID);
}

void LoadInput(const std::string& filename)
{
	//Dragonbane
	if (isVerifying)
		return;

	File::IOFile t_record;
	if (!t_record.Open(filename, "r+b"))
	{
		PanicAlertT("Failed to read %s", filename.c_str());
		EndPlayInput(false);
		return;
	}

	t_record.ReadArray(&tmpHeader, 1);

	if (!IsMovieHeader(tmpHeader.filetype))
	{
		PanicAlertT("Savestate movie %s is corrupted, movie recording stopping...", filename.c_str());
		EndPlayInput(false);
		return;
	}
	ReadHeader();
	if (!s_bReadOnly)
	{
		s_rerecords++;
		tmpHeader.numRerecords = s_rerecords;
		t_record.Seek(0, SEEK_SET);
		t_record.WriteArray(&tmpHeader, 1);
	}

	ChangePads(true);
	if (SConfig::GetInstance().bWii)
		ChangeWiiPads(true);

	u64 totalSavedBytes = t_record.GetSize() - 256;

	bool afterEnd = false;
	// This can only happen if the user manually deletes data from the dtm.
	if (s_currentByte > totalSavedBytes)
	{
		PanicAlertT("Warning: You loaded a save whose movie ends before the current frame in the save (byte %u < %u) (frame %u < %u). You should load another save before continuing.", (u32)totalSavedBytes+256, (u32)s_currentByte+256, (u32)tmpHeader.frameCount, (u32)g_currentFrame);
		afterEnd = true;
	}

	if (!s_bReadOnly || tmpInput == nullptr)
	{
		g_totalFrames = tmpHeader.frameCount;
		s_totalLagCount = tmpHeader.lagCount;
		g_totalInputCount = tmpHeader.inputCount;
		s_totalTickCount = s_tickCountAtLastInput = tmpHeader.tickCount;
		s_bRecordingFromSaveState = tmpHeader.bFromSaveState; //Dragonbane

		//Dragonbane
		if (s_bRecordingFromSaveState && lastMovie.compare(filename))
		{
			std::string stateFilename = filename + ".sav";
			const std::string save_path = File::GetUserPath(D_STATESAVES_IDX) + "dtm.sav";

			if (File::Exists(save_path))
				File::Delete(save_path);

			File::Copy(stateFilename, save_path);

			lastMovie = filename;
		}

		EnsureTmpInputSize((size_t)totalSavedBytes);
		s_totalBytes = totalSavedBytes;
		t_record.ReadArray(tmpInput, (size_t)s_totalBytes);
	}
	else if (s_currentByte > 0)
	{
		if (s_currentByte > totalSavedBytes)
		{
		}
		else if (s_currentByte > s_totalBytes)
		{
			afterEnd = true;
			PanicAlertT("Warning: You loaded a save that's after the end of the current movie. (byte %u > %u) (frame %u > %u). You should load another save before continuing, or load this state with read-only mode off.", (u32)s_currentByte+256, (u32)s_totalBytes+256, (u32)g_currentFrame, (u32)g_totalFrames);
		}
		else if (s_currentByte > 0 && s_totalBytes > 0)
		{
			// verify identical from movie start to the save's current frame
			u32 len = (u32)s_currentByte;
			u8* movInput = new u8[len];
			t_record.ReadArray(movInput, (size_t)len);
			for (u32 i = 0; i < len; ++i)
			{
				if (movInput[i] != tmpInput[i])
				{
					// this is a "you did something wrong" alert for the user's benefit.
					// we'll try to say what's going on in excruciating detail, otherwise the user might not believe us.
					if (IsUsingWiimote(0))
					{
						// TODO: more detail
						PanicAlertT("Warning: You loaded a save whose movie mismatches on byte %d (0x%X). You should load another save before continuing, or load this state with read-only mode off. Otherwise you'll probably get a desync.", i+256, i+256);
						memcpy(tmpInput, movInput, s_currentByte);
					}
					else
					{
						int frame = i / 17; //Dragonbane: 17
						ControllerState curPadState;
						memcpy(&curPadState, &(tmpInput[frame * 17]), 17);
						ControllerState movPadState;
						memcpy(&movPadState, &(movInput[frame * 17]), 17);
						PanicAlertT("Warning: You loaded a save whose movie mismatches on frame %d. You should load another save before continuing, or load this state with read-only mode off. Otherwise you'll probably get a desync.\n\n"
							"More information: The current movie is %d frames long and the savestate's movie is %d frames long.\n\n"
							"On frame %d, the current movie presses:\n"
							"Start=%d, A=%d, B=%d, X=%d, Y=%d, Z=%d, DUp=%d, DDown=%d, DLeft=%d, DRight=%d, L=%d, R=%d, LT=%d, RT=%d, AnalogX=%d, AnalogY=%d, CX=%d, CY=%d"
							"\n\n"
							"On frame %d, the savestate's movie presses:\n"
							"Start=%d, A=%d, B=%d, X=%d, Y=%d, Z=%d, DUp=%d, DDown=%d, DLeft=%d, DRight=%d, L=%d, R=%d, LT=%d, RT=%d, AnalogX=%d, AnalogY=%d, CX=%d, CY=%d",
							(int)frame,
							(int)g_totalFrames, (int)tmpHeader.frameCount,
							(int)frame,
							(int)curPadState.Start, (int)curPadState.A, (int)curPadState.B, (int)curPadState.X, (int)curPadState.Y, (int)curPadState.Z, (int)curPadState.DPadUp, (int)curPadState.DPadDown, (int)curPadState.DPadLeft, (int)curPadState.DPadRight, (int)curPadState.L, (int)curPadState.R, (int)curPadState.TriggerL, (int)curPadState.TriggerR, (int)curPadState.AnalogStickX, (int)curPadState.AnalogStickY, (int)curPadState.CStickX, (int)curPadState.CStickY,
							(int)frame,
							(int)movPadState.Start, (int)movPadState.A, (int)movPadState.B, (int)movPadState.X, (int)movPadState.Y, (int)movPadState.Z, (int)movPadState.DPadUp, (int)movPadState.DPadDown, (int)movPadState.DPadLeft, (int)movPadState.DPadRight, (int)movPadState.L, (int)movPadState.R, (int)movPadState.TriggerL, (int)movPadState.TriggerR, (int)movPadState.AnalogStickX, (int)movPadState.AnalogStickY, (int)movPadState.CStickX, (int)movPadState.CStickY);

					}
					break;
				}
			}
			delete [] movInput;
		}
	}
	t_record.Close();

	s_bSaveConfig = tmpHeader.bSaveConfig;

	if (!afterEnd)
	{
		if (s_bReadOnly)
		{
			if (s_playMode != MODE_PLAYING)
			{
				s_playMode = MODE_PLAYING;
				Core::DisplayMessage("Switched to playback", 2000);
			}
		}
		else
		{
			if (s_playMode != MODE_RECORDING)
			{
				s_playMode = MODE_RECORDING;
				justStoppedRecording = false;
				Core::DisplayMessage("Switched to recording", 2000);
			}
		}
	}
	else
	{
		EndPlayInput(false);
	}
}

static void CheckInputEnd()
{
	if (g_currentFrame > g_totalFrames || s_currentByte >= s_totalBytes || (CoreTiming::GetTicks() > s_totalTickCount && !IsRecordingInputFromSaveState()))
	{
		EndPlayInput(!s_bReadOnly);
	}
}

void PlayController(GCPadStatus* PadStatus, int controllerID)
{
	// Correct playback is entirely dependent on the emulator polling the controllers
	// in the same order done during recording
	if (!IsPlayingInput() || !IsUsingPad(controllerID) || tmpInput == nullptr)
		return;


	//Dragonbane: Detect bad settings
	if (badSettings == false)
	{
		if (SConfig::GetInstance().m_OCEnable || SConfig::GetInstance().bDSPHLE)
		{
			PanicAlertT("This movie was recorded with unreliable settings and will likely desync!\nPlease disable CPU Overclocking and use LLE audio.");
		}
		else if (SConfig::GetInstance().bCPUThread)
		{
			PanicAlertT("This movie was recorded with inconsistent settings and might desync!\nPlease disable Dual Core to prevent desync issues.");
		}

		badSettings = true;
	}

	//Dragonbane: Lag warning
	if (lagWarning == false)
	{
		if (s_totalLagCount > 0)
		{
			PanicAlertT("Lag occurred during the recording of this movie. Desync possible...");
			lagWarning = true;
		}
	}

	//Dragonbane: Auto Save
	if (isAutoVerifying && s_currentByte + 85 == s_totalBytes) //Ask for auto save 5 frames before end
	{
		if (AskYesNoT("Set current frame as start position for next verification?"))
		{
			autoSave = true;
			updateMainFrame = true;
		}
		isAutoVerifying = false;
	}

	//Make sure Auto Roll is off for playback/verification
	roll_enabled = false;
	checkSave = false;
	uncheckSave = true;

	//TP Stuff
	std::string gameID = SConfig::GetInstance().GetUniqueID();
	u32 isLoadingAdd;
	u32 charPointerAddress;

	bool isTP = false;

	if (!gameID.compare("GZ2E01"))
	{
		charPointerAddress = 0x3dce54;
		isLoadingAdd = 0x450ce0;

		isTP = true;
	}
	else if (!gameID.compare("GZ2P01"))
	{
		charPointerAddress = 0x3dedf4;
		isLoadingAdd = 0x452ca0;

		isTP = true;
	}

	//TWW Stuff
	bool isTWW = false;

	if (!gameID.compare("GZLJ01"))
	{
		isLoadingAdd = 0x3ad335;

		isTWW = true;
	}


	/*
	bool memoryUpdated = false;

	//Dragonbane: Update RAM
	if (isTP)
	{
	u8* memptr = Memory::m_pRAM;

	if (memptr != nullptr)
	{
	u32 numChanges = 0;
	u32 memOffset = 0;

	u64 offsetPos = 0;

	memcpy(&numChanges, &(tmpInput[s_currentByte + 16]), 4); //Dragonbane

	if (numChanges > 0)
	{
	for (u32 i = 0; i < numChanges; i++)
	{
	offsetPos = s_currentByte + 20 + (i * 8);
	memcpy(&memOffset, &(tmpInput[offsetPos]), 4);

	/*
	if ((0xa0a000 + memOffset) == characterpointer)
	{
	float LinkX = Memory::Read_F32(characterpointer);
	float nextLinkX = 0.0f;

	Memory::Write_U32(0, characterpointer);

	//memcpy(&nextLinkX, &(tmpInput[offsetPos + 4]), 4);

	nextLinkX = Memory::Read_F32(characterpointer);
	}
	*/
	/*
	if (memOffset < 0xFC4C && memOffset > 0xA000)
	memcpy((memptr + 0xa0a000) + memOffset, &(tmpInput[offsetPos + 4]), 4);

	//memcpy(&(tmpDynamicRange[memOffset]), &(tmpInput[offsetPos + 4]), 4);
	}

	u32 sizeChange = 20 + (numChanges * 8);

	if (s_currentByte + sizeChange > s_totalBytes) //Dragonbane: 16
	{
	PanicAlertT("Premature movie end in PlayController. %u + %u > %u", (u32)s_currentByte, (u32)sizeChange, (u32)s_totalBytes);
	EndPlayInput(!s_bReadOnly);
	return;
	}

	memcpy(&s_padState, &(tmpInput[s_currentByte]), 16); //Dragonbane
	s_currentByte += sizeChange;

	memoryUpdated = true;
	}
	}
	}
	*/

	//if (!memoryUpdated)
	//{
		if (s_currentByte + 17 > s_totalBytes) //Dragonbane: 17
		{
			PanicAlertT("Premature movie end in PlayController. %u + 17 > %u", (u32)s_currentByte, (u32)s_totalBytes);
			EndPlayInput(!s_bReadOnly);
			return;
		}

		memcpy(&s_padState, &(tmpInput[s_currentByte]), 17); //Dragonbane
		s_currentByte += 17; //Dragonbane: 17
	//}

	
	// dtm files don't save the mic button or error bit. not sure if they're actually used, but better safe than sorry
	signed char e = PadStatus->err;
	memset(PadStatus, 0, sizeof(GCPadStatus));
	PadStatus->err = e;


	//Dragonbane: Check for desync
	//TP Stuff
	if (isTP)
	{
		u32 characterpointer = Memory::Read_U32(charPointerAddress);
		u32 isLoading = Memory::Read_U32(isLoadingAdd);


		//1070
		/*
		//Legacy load times syncing
		if (isLoading)
		{
		if (!nowLoading)
		{
		if (!s_padState.loading)
		PanicAlertT("RIP");

		if (s_padState.loading)
		{
		nowLoading = true;
		lastLoadByte = s_currentByte - 16;
		}
		}
		}
		else
		{
		if (nowLoading)
		{
		nowLoading = false;
		u64 saved = s_currentByte - 16;
		u64 currDiff = ((s_currentByte - 16) - lastLoadByte) / 16;

		s_currentByte = lastLoadByte;

		bool movieLoading = true;

		while (movieLoading)
		{
		memcpy(&s_padState, &(tmpInput[s_currentByte]), 16);
		s_currentByte += 16;

		movieLoading = s_padState.loading;
		}

		u64 recDiff = ((s_currentByte - 16) - lastLoadByte) / 16;

		if (currDiff > recDiff)
		{
		int framesAdd = currDiff - recDiff;

		g_totalFrames += framesAdd;
		}
		else if (currDiff < recDiff)
		{
		int framesAdd = recDiff - currDiff;
		g_currentFrame += framesAdd;
		//g_totalFrames += framesAdd;
		}
		}

		}

		if (!waited && g_currentFrame == 992) //After Memory card formatting delay
		{
		waitFrames = 1;
		waited = true;
		reverseWait = false;
		}

		if (g_currentFrame == 995)
		waited = false;




		if (!waited && g_currentFrame == 2989) //After BiT Void
		{
		waitFrames = 1;
		waited = true;
		reverseWait = false;
		}

		if (g_currentFrame == 2993)
		waited = false;



		if (!waited && g_currentFrame == 3193) //After Reset
		{
		waitFrames = 1;
		waited = true;
		reverseWait = false;
		}

		if (g_currentFrame == 3196)
		waited = false;




		if (!waited && g_currentFrame == 3475) //After Faron Load
		{
		waitFrames = 2;
		waited = true;
		reverseWait = true;
		}

		if (g_currentFrame == 3490)
		waited = false;





		if (waitFrames > 0)
		{
		if (reverseWait)
		{
		s_currentByte -= 16;

		s_currentByte += waitFrames*16;

		memcpy(&s_padState, &(tmpInput[s_currentByte]), 16);

		s_currentByte += 16;

		g_currentFrame += waitFrames;
		waitFrames = 0;
		}
		else
		{
		g_currentFrame--;
		waitFrames--;

		s_currentByte -= 16;

		memcpy(&s_padState, &(tmpInput[s_currentByte - 16]), 16);
		}
		}
		*/

		if (characterpointer > 0x80000000 && isLoading == 0)
		{
			characterpointer -= 0x80000000;

			float LinkX = Memory::Read_F32(characterpointer);
			float LinkZ = Memory::Read_F32(characterpointer + 0x8);

			if (s_padState.LinkX != LinkX || s_padState.LinkZ != LinkZ)
			{
				std::string message = StringFromFormat("Desync detected on frame: %d!", g_currentFrame);
				//std::string message = StringFromFormat("Desync detected! ExpX: %f, ExpLoad: %i, CurrX: %f, CurrLoad: %i", s_padState.LinkX, s_padState.loading, LinkX, isLoading);
				Core::DisplayMessage(message, 2000);

				desyncCount += 1;

				/*
				u8* memptr = Memory::m_pRAM;
				if (memptr != nullptr && Core::IsRunningAndStarted())
				{
				memcpy((memptr + 0xa0a000), &(tmpDynamicRange[0]), 1007616); //6316032);
				}
				*/

				//PanicAlertT("Desync occurred on frame: %d!\nMovie Link Pos: %.02f, %.02f | Curr Link Pos: %.02f, %.02f", g_currentFrame, s_padState.LinkX, s_padState.LinkZ, LinkX, LinkZ);

				/*
				bool matchFound = false;

				ControllerState pastPadState;
				memset(&pastPadState, 0, sizeof(pastPadState));
				memcpy(&pastPadState, &(tmpInput[s_currentByte - 32]), 16);

				ControllerState nextPadState;
				memset(&nextPadState, 0, sizeof(nextPadState));
				memcpy(&nextPadState, &(tmpInput[s_currentByte]), 16);

				if (pastPadState.LinkX == LinkX && pastPadState.LinkZ == LinkZ)
				{
				s_currentByte -= 32;
				memcpy(&s_padState, &(tmpInput[s_currentByte]), 16);
				s_currentByte += 16;

				matchFound = true;
				}
				else if (nextPadState.LinkX == LinkX && nextPadState.LinkZ == LinkZ)
				{
				memcpy(&s_padState, &(tmpInput[s_currentByte]), 16);
				s_currentByte += 16;

				matchFound = true;
				}
				if (matchFound)
				{
				//PanicAlertT("Trying to match with: \nMovie Link Pos: %.02f, %.02f | Curr Link Pos: %.02f, %.02f", s_padState.LinkX, s_padState.LinkZ, LinkX, LinkZ);
				}
				else
				{
				PanicAlertT("Unable to recover from desync, end playback!");
				EndPlayInput(!s_bReadOnly);
				return;
				}
				*/
			}
		}
	}


	//TWW Stuff
	if (isTWW)
	{
		u8 isLoading = Memory::Read_U8(isLoadingAdd);

		if (isLoading == 0)
		{
			u32 XAdd = 0x3d78fc;
			u32 ZAdd = 0x3d7904;

			float LinkX = Memory::Read_F32(XAdd);
			float LinkZ = Memory::Read_F32(ZAdd);

			if (s_padState.LinkX != LinkX || s_padState.LinkZ != LinkZ)
			{
				std::string message = StringFromFormat("Desync detected on frame: %d!", g_currentFrame);
				Core::DisplayMessage(message, 2000);

				desyncCount += 1;
			}
		}
	}


	PadStatus->triggerLeft = s_padState.TriggerL;
	PadStatus->triggerRight = s_padState.TriggerR;

	PadStatus->stickX = s_padState.AnalogStickX;
	PadStatus->stickY = s_padState.AnalogStickY;

	PadStatus->substickX = s_padState.CStickX;
	PadStatus->substickY = s_padState.CStickY;

	PadStatus->button |= PAD_USE_ORIGIN;

	if (s_padState.A)
	{
		PadStatus->button |= PAD_BUTTON_A;
		PadStatus->analogA = 0xFF;
	}
	if (s_padState.B)
	{
		PadStatus->button |= PAD_BUTTON_B;
		PadStatus->analogB = 0xFF;
	}
	if (s_padState.X)
		PadStatus->button |= PAD_BUTTON_X;
	if (s_padState.Y)
		PadStatus->button |= PAD_BUTTON_Y;
	if (s_padState.Z)
		PadStatus->button |= PAD_TRIGGER_Z;
	if (s_padState.Start)
		PadStatus->button |= PAD_BUTTON_START;

	if (s_padState.DPadUp)
		PadStatus->button |= PAD_BUTTON_UP;
	if (s_padState.DPadDown)
		PadStatus->button |= PAD_BUTTON_DOWN;
	if (s_padState.DPadLeft)
		PadStatus->button |= PAD_BUTTON_LEFT;
	if (s_padState.DPadRight)
		PadStatus->button |= PAD_BUTTON_RIGHT;

	if (s_padState.L)
		PadStatus->button |= PAD_TRIGGER_L;
	if (s_padState.R)
		PadStatus->button |= PAD_TRIGGER_R;
	if (s_padState.disc)
	{
		// This implementation assumes the disc change will only happen once. Trying to change more than that will cause
		// it to load the last disc every time. As far as i know though, there are no 3+ disc games, so this should be fine.
		Core::SetState(Core::CORE_PAUSE);
		bool found = false;
		std::string path;
		for (size_t i = 0; i < SConfig::GetInstance().m_ISOFolder.size(); ++i)
		{
			path = SConfig::GetInstance().m_ISOFolder[i];
			if (File::Exists(path + '/' + g_discChange))
			{
				found = true;
				break;
			}
		}
		if (found)
		{
			DVDInterface::ChangeDisc(path + '/' + g_discChange);
			Core::SetState(Core::CORE_RUN);
		}
		else
		{
			PanicAlertT("Change the disc to %s", g_discChange.c_str());
		}
	}

	if (s_padState.reset)
		ProcessorInterface::ResetButton_Tap();

	//Dragonbane: Execute Tuner Events
	if (s_padState.tunerEvent > 0)
	{
		tunerExecuteID = s_padState.tunerEvent;
	}

	SetInputDisplayString(s_padState, controllerID);
	CheckInputEnd();
}

bool PlayWiimote(int wiimote, u8 *data, const WiimoteEmu::ReportFeatures& rptf, int ext, const wiimote_key key)
{
	if (!IsPlayingInput() || !IsUsingWiimote(wiimote) || tmpInput == nullptr)
		return false;

	if (s_currentByte > s_totalBytes)
	{
		PanicAlertT("Premature movie end in PlayWiimote. %u > %u", (u32)s_currentByte, (u32)s_totalBytes);
		EndPlayInput(!s_bReadOnly);
		return false;
	}

	u8 size = rptf.size;

	u8 sizeInMovie = tmpInput[s_currentByte];

	if (size != sizeInMovie)
	{
		PanicAlertT("Fatal desync. Aborting playback. (Error in PlayWiimote: %u != %u, byte %u.)%s", (u32)sizeInMovie, (u32)size, (u32)s_currentByte,
					(s_numPads & 0xF)?" Try re-creating the recording with all GameCube controllers disabled (in Configure > GameCube > Device Settings)." : "");
		EndPlayInput(!s_bReadOnly);
		return false;
	}

	s_currentByte++;

	if (s_currentByte + size > s_totalBytes)
	{
		PanicAlertT("Premature movie end in PlayWiimote. %u + %d > %u", (u32)s_currentByte, size, (u32)s_totalBytes);
		EndPlayInput(!s_bReadOnly);
		return false;
	}

	memcpy(data, &(tmpInput[s_currentByte]), size);
	s_currentByte += size;

	g_currentInputCount++;

	CheckInputEnd();
	return true;
}

void EndPlayInput(bool cont)
{
	if (cont)
	{
		s_playMode = MODE_RECORDING;
		Core::DisplayMessage("Reached movie end. Resuming recording.", 2000);
		justStoppedRecording = false; //Dragonbane
	}
	else if (s_playMode != MODE_NONE)
	{
		s_rerecords = 0;
		s_currentByte = 0;
		s_playMode = MODE_NONE;
		Core::UpdateWantDeterminism();
		Core::DisplayMessage("Movie End.", 2000);
		s_bRecordingFromSaveState = false;
		// we don't clear these things because otherwise we can't resume playback if we load a movie state later
		//g_totalFrames = s_totalBytes = 0;
		//delete tmpInput;
		//tmpInput = nullptr;

		if (cmp_isRunning && cmp_movieFinished)
		{
			if (!GetNextComparisonMovie(true))
			{
				if (cmp_currentMovie == cmp_leftMovie)
				{
					//PanicAlertT("Left side done");
					cmp_currentMovie = cmp_rightMovie;
					cmp_leftFinished = true;
				}
				else
				{
					//PanicAlertT("Right side done");
					cmp_currentMovie = cmp_leftMovie;
					cmp_rightFinished = true;
				}

				if (!cmp_leftFinished || !cmp_rightFinished)
					cmp_startTimerFrame = GetDTMComparisonLength(cmp_currentBranch);

				cmp_currentBranch = cmp_currentMovie;
				cmp_curentBranchFrame = 0;
			}

			if (!cmp_leftFinished || !cmp_rightFinished)
			{
				RenderComparisonVideo(false);
			}
			else
			{
				CancelComparison();
				cmp_justFinished = true;
				PanicAlertT("Comparison Video successfully recorded!");
			}

			cmp_movieFinished = false;
		}

		if (Core::IsRunningAndStarted()) //Dragonbane
			updateMainFrame = true;
	}

	//Dragonbane
	if (isVerifying)
	{
		if (desyncCount > 0)
			PanicAlertT("Verifying process failed! %i desync(s) detected :(", desyncCount);
		else
			PanicAlertT("Verifying process succeeded :)");

		Core::SetIsFramelimiterTempDisabled(false);
		isVerifying = false;
		desyncCount = 0;
	}
}

void SaveRecording(const std::string& filename)
{
	File::IOFile save_record(filename, "wb");
	// Create the real header now and write it
	DTMHeader header;
	memset(&header, 0, sizeof(DTMHeader));

	header.filetype[0] = 'D'; header.filetype[1] = 'T'; header.filetype[2] = 'M'; header.filetype[3] = 0x1A;
	strncpy((char *)header.gameID, SConfig::GetInstance().GetUniqueID().c_str(), 6);
	header.bWii = SConfig::GetInstance().bWii;
	header.numControllers = s_numPads & (SConfig::GetInstance().bWii ? 0xFF : 0x0F);

	header.bFromSaveState = s_bRecordingFromSaveState;
	header.frameCount = g_totalFrames;
	header.lagCount = s_totalLagCount;
	header.inputCount = g_totalInputCount;
	header.numRerecords = s_rerecords;
	header.recordingStartTime = s_recordingStartTime;

	header.bSaveConfig = true;
	header.bSkipIdle = s_bSkipIdle;
	header.bDualCore = s_bDualCore;
	header.bProgressive = s_bProgressive;
	header.bPAL60 = s_bPAL60;
	header.bDSPHLE = s_bDSPHLE;
	header.bFastDiscSpeed = s_bFastDiscSpeed;
	strncpy((char *)header.videoBackend, s_videoBackend.c_str(),ArraySize(header.videoBackend));
	header.CPUCore = s_iCPUCore;
	header.bEFBAccessEnable = g_ActiveConfig.bEFBAccessEnable;
	header.bEFBCopyEnable = true;
	header.bSkipEFBCopyToRam = g_ActiveConfig.bSkipEFBCopyToRam;
	header.bEFBCopyCacheEnable = false;
	header.bEFBEmulateFormatChanges = g_ActiveConfig.bEFBEmulateFormatChanges;
	header.bUseXFB = g_ActiveConfig.bUseXFB;
	header.bUseRealXFB = g_ActiveConfig.bUseRealXFB;
	header.memcards = s_memcards;
	header.bClearSave = g_bClearSave;
	header.bSyncGPU = s_bSyncGPU;
	header.bNetPlay = s_bNetPlay;
	strncpy((char *)header.discChange, g_discChange.c_str(),ArraySize(header.discChange));
	strncpy((char *)header.author, s_author.c_str(),ArraySize(header.author));
	memcpy(header.md5,s_MD5,16);
	header.bongos = s_bongos;
	header.numGBAs = s_numGBAs; //Dragonbane
	memcpy(header.revision, s_revision, ArraySize(header.revision));
	header.DSPiromHash = s_DSPiromHash;
	header.DSPcoefHash = s_DSPcoefHash;
	header.tickCount = s_totalTickCount;

	// TODO
	header.uniqueID = 0;
	// header.audioEmulator;

	save_record.WriteArray(&header, 1);

	bool success = save_record.WriteArray(tmpInput, (size_t)s_totalBytes);

	if (success && s_bRecordingFromSaveState)
	{
		std::string stateFilename = filename + ".sav";
		success = File::Copy(File::GetUserPath(D_STATESAVES_IDX) + "dtm.sav", stateFilename);
	}

	if (success)
		Core::DisplayMessage(StringFromFormat("DTM %s saved", filename.c_str()), 2000);
	else
		Core::DisplayMessage(StringFromFormat("Failed to save %s", filename.c_str()), 2000);
}

void SetGCInputManip(GCManipFunction func)
{
	gcmfunc = func;
}
void SetWiiInputManip(WiiManipFunction func)
{
	wiimfunc = func;
}

void CallGCInputManip(GCPadStatus* PadStatus, int controllerID)
{
	if (gcmfunc)
		(*gcmfunc)(PadStatus, controllerID);
}
void CallWiiInputManip(u8* data, WiimoteEmu::ReportFeatures rptf, int controllerID, int ext, const wiimote_key key)
{
	if (wiimfunc)
		(*wiimfunc)(data, rptf, controllerID, ext, key);
}

void SetGraphicsConfig()
{
	g_Config.bEFBAccessEnable = tmpHeader.bEFBAccessEnable;
	g_Config.bSkipEFBCopyToRam = tmpHeader.bSkipEFBCopyToRam;
	g_Config.bEFBEmulateFormatChanges = tmpHeader.bEFBEmulateFormatChanges;
	g_Config.bUseXFB = tmpHeader.bUseXFB;
	g_Config.bUseRealXFB = tmpHeader.bUseRealXFB;
}

void GetSettings()
{
	s_bSaveConfig = true;
	s_bSkipIdle = SConfig::GetInstance().bSkipIdle;
	s_bDualCore = SConfig::GetInstance().bCPUThread;
	s_bProgressive = SConfig::GetInstance().bProgressive;
	s_bPAL60 = SConfig::GetInstance().bPAL60;
	s_bDSPHLE = SConfig::GetInstance().bDSPHLE;
	s_bFastDiscSpeed = SConfig::GetInstance().bFastDiscSpeed;
	s_videoBackend = g_video_backend->GetName();
	s_bSyncGPU = SConfig::GetInstance().bSyncGPU;
	s_iCPUCore = SConfig::GetInstance().iCPUCore;
	s_bNetPlay = NetPlay::IsNetPlayRunning();
	if (!SConfig::GetInstance().bWii)
		g_bClearSave = !File::Exists(SConfig::GetInstance().m_strMemoryCardA);
	s_memcards |= (SConfig::GetInstance().m_EXIDevice[0] == EXIDEVICE_MEMORYCARD) << 0;
	s_memcards |= (SConfig::GetInstance().m_EXIDevice[1] == EXIDEVICE_MEMORYCARD) << 1;
	unsigned int tmp;
	for (int i = 0; i < 20; ++i)
	{
		sscanf(&scm_rev_git_str[2 * i], "%02x", &tmp);
		s_revision[i] = tmp;
	}
	if (!s_bDSPHLE)
	{
		std::string irom_file = File::GetUserPath(D_GCUSER_IDX) + DSP_IROM;
		std::string coef_file = File::GetUserPath(D_GCUSER_IDX) + DSP_COEF;

		if (!File::Exists(irom_file))
			irom_file = File::GetSysDirectory() + GC_SYS_DIR DIR_SEP DSP_IROM;
		if (!File::Exists(coef_file))
			coef_file = File::GetSysDirectory() + GC_SYS_DIR DIR_SEP DSP_COEF;
		std::vector<u16> irom(DSP_IROM_SIZE);
		File::IOFile file_irom(irom_file, "rb");

		file_irom.ReadArray(irom.data(), DSP_IROM_SIZE);
		file_irom.Close();
		for (u32 i = 0; i < DSP_IROM_SIZE; ++i)
			irom[i] = Common::swap16(irom[i]);

		std::vector<u16> coef(DSP_COEF_SIZE);
		File::IOFile file_coef(coef_file, "rb");

		file_coef.ReadArray(coef.data(), DSP_COEF_SIZE);
		file_coef.Close();
		for (u32 i = 0; i < DSP_COEF_SIZE; ++i)
			coef[i] = Common::swap16(coef[i]);
		s_DSPiromHash = HashAdler32((u8*)irom.data(), DSP_IROM_BYTE_SIZE);
		s_DSPcoefHash = HashAdler32((u8*)coef.data(), DSP_COEF_BYTE_SIZE);
	}
	else
	{
		s_DSPiromHash = 0;
		s_DSPcoefHash = 0;
	}
}

static const mbedtls_md_info_t* s_md5_info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);

void CheckMD5()
{
	for (int i = 0, n = 0; i < 16; ++i)
	{
		if (tmpHeader.md5[i] != 0)
			continue;
		n++;
		if (n == 16)
			return;
	}
	Core::DisplayMessage("Verifying checksum...", 2000);

	unsigned char gameMD5[16];
	mbedtls_md_file(s_md5_info, SConfig::GetInstance().m_strFilename.c_str(), gameMD5);

	if (memcmp(gameMD5,s_MD5,16) == 0)
		Core::DisplayMessage("Checksum of current game matches the recorded game.", 2000);
	else
		Core::DisplayMessage("Checksum of current game does not match the recorded game!", 3000);
}

void GetMD5()
{
	Core::DisplayMessage("Calculating checksum of game file...", 2000);
	memset(s_MD5, 0, sizeof(s_MD5));
	mbedtls_md_file(s_md5_info, SConfig::GetInstance().m_strFilename.c_str(), s_MD5);
	Core::DisplayMessage("Finished calculating checksum.", 2000);
}

void Shutdown()
{
	g_currentInputCount = g_totalInputCount = g_totalFrames = s_totalBytes = s_tickCountAtLastInput = 0;
	delete [] tmpInput;
	tmpInput = nullptr;
	tmpInputAllocated = 0;

	//Dragonbane
	//delete[] tmpDynamicRange;
	//tmpDynamicRange = nullptr;
	badSettings = false;
	isVerifying = false;
}

//Dragonbane: New Functions
bool VerifyRecording(const std::string& moviename, const std::string& statename, bool fromStart)
{
	isVerifying = false;
	isAutoVerifying = false;
	justStoppedRecording = false;

	if (s_playMode != MODE_NONE)
		return false;

	if (!File::Exists(moviename))
		return false;

	if (statename.size() > 1 && !File::Exists(statename))
		return false;

	if (statename.size() < 2 && Core::IsRunningAndStarted())
		return false;

	File::IOFile g_recordfd;

	if (!g_recordfd.Open(moviename, "rb"))
		return false;

	g_recordfd.ReadArray(&tmpHeader, 1);

	if (!IsMovieHeader(tmpHeader.filetype))
	{
		PanicAlertT("Invalid recording file");
		g_recordfd.Close();
		return false;
	}

	ReadHeader();
	g_totalFrames = tmpHeader.frameCount;
	s_totalLagCount = tmpHeader.lagCount;
	g_totalInputCount = tmpHeader.inputCount;
	s_totalTickCount = tmpHeader.tickCount;

	g_currentFrame = 0;
	g_currentLagCount = 0;
	g_currentInputCount = 0;

	s_playMode = MODE_PLAYING;

	Core::UpdateWantDeterminism();

	s_totalBytes = g_recordfd.GetSize() - 256;
	EnsureTmpInputSize((size_t)s_totalBytes);
	g_recordfd.ReadArray(tmpInput, (size_t)s_totalBytes);
	s_currentByte = 0;
	g_recordfd.Close();


	if (fromStart == true && statename.size() > 1 || fromStart == false && statename.size() > 1) // Load savestate (and skip to frame data)
	{
		const std::string stateFilename = statename;

		tmpHeader.bFromSaveState = true;
		s_bRecordingFromSaveState = true;

		isVerifying = true;

		if (Core::IsRunningAndStarted())
			State::LoadAs(stateFilename);
		else
			Core::SetStateFileName(stateFilename);

		isVerifying = false;

		File::IOFile t_record;
		t_record.Open(moviename, "r+b");

		ChangePads(true);
		if (SConfig::GetInstance().bWii)
			ChangeWiiPads(true);

		u64 totalSavedBytes = t_record.GetSize() - 256;

		bool afterEnd = false;
		// This can only happen if the user manually deletes data from the dtm.
		if (s_currentByte > totalSavedBytes)
		{
			PanicAlertT("Warning: You loaded a save whose movie ends before the current frame in the save (byte %u < %u) (frame %u < %u). You should load another save.", (u32)totalSavedBytes + 256, (u32)s_currentByte + 256, (u32)tmpHeader.frameCount, (u32)g_currentFrame);
			afterEnd = true;
		}

		if (s_currentByte > 0)
		{
			if (!File::Exists(stateFilename + ".dtm"))
			{
				PanicAlertT("Warning: There is no dtm file associated with this save. This might not work...");
			}
			else if (s_currentByte > totalSavedBytes)
			{
			}
			else if (s_currentByte > s_totalBytes)
			{
				afterEnd = true;
				PanicAlertT("Warning: You loaded a save that's after the end of the current movie. (byte %u > %u) (frame %u > %u). You should load another save.", (u32)s_currentByte + 256, (u32)s_totalBytes + 256, (u32)g_currentFrame, (u32)g_totalFrames);
			}
			else if (s_currentByte > 0 && s_totalBytes > 0)
			{
				// verify identical from movie start to the save's current frame
				u32 len = (u32)s_currentByte;
				u8* movInput = new u8[len];

				t_record.Seek(256, SEEK_SET);
				t_record.ReadArray(movInput, (size_t)len);

				File::IOFile t_record_state;
				t_record_state.Open(stateFilename + ".dtm", "r+b");
				u8* stateInput = new u8[len];

				t_record_state.Seek(256, SEEK_SET);
				t_record_state.ReadArray(stateInput, (size_t)len);

				t_record_state.Close();

				for (u32 i = 0; i < len; ++i)
				{
					if (movInput[i] != stateInput[i]) //Dragonbane
					{
						// this is a "you did something wrong" alert for the user's benefit.
						// we'll try to say what's going on in excruciating detail, otherwise the user might not believe us.
						if (IsUsingWiimote(0))
						{
							// TODO: more detail
							PanicAlertT("Warning: You loaded a save whose movie mismatches on byte %d (0x%X). You should load another save.", i + 256, i + 256);
							memcpy(tmpInput, stateInput, s_currentByte);
						}
						else
						{
							int frame = i / 17; //Dragonbane: 17
							ControllerState curPadState;
							memcpy(&curPadState, &(tmpInput[frame * 17]), 17);
							ControllerState movPadState;
							memcpy(&movPadState, &(stateInput[frame * 17]), 17);
							PanicAlertT("Warning: You loaded a save whose movie mismatches on frame %d. You should load another save.\n\n"
								"More information: The current movie is %d frames long and the savestate's movie is %d frames long.\n\n"
								"On frame %d, the current movie presses:\n"
								"Start=%d, A=%d, B=%d, X=%d, Y=%d, Z=%d, DUp=%d, DDown=%d, DLeft=%d, DRight=%d, L=%d, R=%d, LT=%d, RT=%d, AnalogX=%d, AnalogY=%d, CX=%d, CY=%d"
								"\n\n"
								"On frame %d, the savestate's movie presses:\n"
								"Start=%d, A=%d, B=%d, X=%d, Y=%d, Z=%d, DUp=%d, DDown=%d, DLeft=%d, DRight=%d, L=%d, R=%d, LT=%d, RT=%d, AnalogX=%d, AnalogY=%d, CX=%d, CY=%d",
								(int)frame,
								(int)g_totalFrames, (int)tmpHeader.frameCount,
								(int)frame,
								(int)curPadState.Start, (int)curPadState.A, (int)curPadState.B, (int)curPadState.X, (int)curPadState.Y, (int)curPadState.Z, (int)curPadState.DPadUp, (int)curPadState.DPadDown, (int)curPadState.DPadLeft, (int)curPadState.DPadRight, (int)curPadState.L, (int)curPadState.R, (int)curPadState.TriggerL, (int)curPadState.TriggerR, (int)curPadState.AnalogStickX, (int)curPadState.AnalogStickY, (int)curPadState.CStickX, (int)curPadState.CStickY,
								(int)frame,
								(int)movPadState.Start, (int)movPadState.A, (int)movPadState.B, (int)movPadState.X, (int)movPadState.Y, (int)movPadState.Z, (int)movPadState.DPadUp, (int)movPadState.DPadDown, (int)movPadState.DPadLeft, (int)movPadState.DPadRight, (int)movPadState.L, (int)movPadState.R, (int)movPadState.TriggerL, (int)movPadState.TriggerR, (int)movPadState.AnalogStickX, (int)movPadState.AnalogStickY, (int)movPadState.CStickX, (int)movPadState.CStickY);

						}
						break;
					}
				}
				delete[] movInput;
				delete[] stateInput;
			}
		}
		t_record.Close();

		s_bSaveConfig = tmpHeader.bSaveConfig;

		if (!afterEnd)
		{
			if (s_playMode != MODE_PLAYING)
			{
				s_playMode = MODE_PLAYING;
			}
		}
		else
		{
			EndPlayInput(false);
			return false;
		}

	}

	Core::SetIsFramelimiterTempDisabled(true);

	isVerifying = true;
	desyncCount = 0;

	//Make sure Auto Roll is off for playback/verification
	roll_enabled = false;
	checkSave = false;
	uncheckSave = true;

	Core::DisplayMessage("Switched to verifying", 5000);

	return true;
}
void CancelVerifying()
{
	desyncCount = 0;
	isVerifying = false;
	isAutoVerifying = false;
	badSettings = false;

	Core::SetIsFramelimiterTempDisabled(false);
}
void CancelRecording()
{
	desyncCount = 0;
	isVerifying = false;
	isAutoVerifying = false;
	badSettings = false;

	Core::SetIsFramelimiterTempDisabled(false);

	if (IsRecordingInput())
		justStoppedRecording = true;

	s_playMode = MODE_NONE;
	Core::UpdateWantDeterminism();

	if (cmp_isRunning || cmp_requested)
		CancelComparison();

	Core::DisplayMessage("Stop Recording/Playback", 4000);
}
bool IsAutoSave()
{
	if (autoSave)
	{
		autoSave = false;
		return true;
	}

	return false;
}
bool IsMovieFromSaveState(const std::string& moviename)
{
	if (!File::Exists(moviename))
		return false;

	File::IOFile g_recordfd;

	if (!g_recordfd.Open(moviename, "rb"))
		return false;

	DTMHeader tmpHead;

	g_recordfd.ReadArray(&tmpHead, 1);

	g_recordfd.Close();

	if (!IsMovieHeader(tmpHead.filetype))
		return false;


	if (tmpHead.bFromSaveState)
		return true;
	else
		return false;

}
bool AutoVerify()
{
	if (Movie::IsRecordingInput() || Movie::justStoppedRecording)
	{
		if (File::Exists(autoVerifyMovieFilename))
			File::Delete(autoVerifyMovieFilename);

		if (File::Exists(autoVerifyMovieFilename + ".sav"))
			File::Delete(autoVerifyMovieFilename + ".sav");

		CancelRecording();
		SaveRecording(autoVerifyMovieFilename);

		if (File::Exists(autoVerifyStateFilename))
		{
			VerifyRecording(autoVerifyMovieFilename, autoVerifyStateFilename, false);

			isAutoVerifying = true;
			return true;
		}

	}
	else
	{
		if (File::Exists(autoVerifyMovieFilename) && File::Exists(autoVerifyStateFilename))
		{
			CancelVerifying();
			VerifyRecording(autoVerifyMovieFilename, autoVerifyStateFilename, false);

			isAutoVerifying = true;
			return true;
		}
	}

	if (File::Exists(autoVerifyMovieFilename))
	{
		if (IsMovieFromSaveState(autoVerifyMovieFilename))
		{
			PanicAlertT("Unsupported feature sorry :/");
			return false;
		}
		else
		{
			if (!Core::IsRunningAndStarted())
			{
				VerifyRecording(autoVerifyMovieFilename, "", true);

				isAutoVerifying = true;
				return true;
			}
			else
			{
				PanicAlertT("You can only verify this movie from a fresh boot. Please close the game and try to verify again without the game running!");
				return false;
			}
		}
	}

	return false;
}
void RequestVideoComparison(const std::string& leftMovie, const std::string& rightMovie, const std::string& leftMovieTitle, const std::string& rightMovieTitle, int width, int height, const std::string& savePath)
{
	//Dragonbane: Set Variables for other systems
	cmp_leftMovie = leftMovie;
	cmp_rightMovie = rightMovie;
	cmp_currentMovie = "";
	cmp_leftTitle = leftMovieTitle;
	cmp_rightTitle = rightMovieTitle;
	cmp_width = width;
	cmp_height = height;
	cmp_outputPath = savePath;

	cmp_isRunning = false;
	cmp_leftFinished = false;
	cmp_rightFinished = false;
	cmp_justFinished = false;
	cmp_requested = true;

	//Set correct render settings
	Host_UpdateMainFrame();
}
bool StartVideoComparison()
{
	cmp_requested = false;

	//Find all DTMs, time and find faster side
	u64 leftTiming = 0, rightTiming = 0;

	leftTiming = GetDTMComparisonLength(cmp_leftMovie);
	//PanicAlertT("Left Timing: %i", leftTiming);

	rightTiming = GetDTMComparisonLength(cmp_rightMovie);
	//PanicAlertT("Right Timing: %i", rightTiming);

	if (leftTiming == 0 || rightTiming == 0)
	{
		PanicAlertT("Critical Error. Please check the DTM files!");
		return false;
	}

	if (leftTiming < rightTiming)
	{
		//Left side faster, render that one first
		cmp_currentMovie = cmp_leftMovie;
	}
	else
	{
		//Right side faster or equal (default)
		cmp_currentMovie = cmp_rightMovie;
	}

	cmp_isRunning = true;
	cmp_justFinished = false;
	cmp_currentBranch = cmp_currentMovie;
	cmp_curentBranchFrame = 0;

	//Start Dumping
	SConfig::GetInstance().m_PauseMovie = false;
	SConfig::GetInstance().m_DumpFrames = true;
	SConfig::GetInstance().m_DumpAudio = true;

	RenderComparisonVideo(true);

	return true;
}
void RenderComparisonVideo(bool schedule)
{
	File::IOFile g_recordfd;

	if (!g_recordfd.Open(cmp_currentMovie, "rb"))
		return;

	g_recordfd.ReadArray(&tmpHeader, 1);

	ReadHeader();
	g_totalFrames = tmpHeader.frameCount;
	s_totalLagCount = tmpHeader.lagCount;
	g_totalInputCount = tmpHeader.inputCount;
	s_totalTickCount = tmpHeader.tickCount;

	g_currentFrame = 0;
	g_currentLagCount = 0;
	g_currentInputCount = 0;

	s_playMode = MODE_PLAYING;

	Core::UpdateWantDeterminism();

	s_totalBytes = g_recordfd.GetSize() - 256;
	EnsureTmpInputSize((size_t)s_totalBytes);
	g_recordfd.ReadArray(tmpInput, (size_t)s_totalBytes);
	s_currentByte = 0;
	g_recordfd.Close();

	if (Core::IsRunningAndStarted() && !tmpHeader.bFromSaveState)
	{
		PanicAlertT("Only the first dtm is allowed to be from bootup!");
		cmp_isRunning = false;
		s_playMode = MODE_NONE;
		return;
	}

	if (tmpHeader.bFromSaveState) // Load savestate (and skip to frame data)
	{
		const std::string stateFilename = cmp_currentMovie + ".sav";

		s_bRecordingFromSaveState = true;

		if (Core::IsRunningAndStarted())
		{
			cmp_loadState = true;

			if (schedule)
				Host_UpdateMainFrame();
		}
		else
		{
			Core::SetStateFileName(stateFilename);
		}

		ChangePads(true);
		if (SConfig::GetInstance().bWii)
			ChangeWiiPads(true);

		s_bSaveConfig = tmpHeader.bSaveConfig;
	}

	//Make sure Auto Roll is off for recording
	roll_enabled = false;
	checkSave = false;
	uncheckSave = true;
}
u64 GetDTMComparisonLength(const std::string& movie)
{
	File::IOFile g_recordfd;
	DTMHeader dtmHeader;

	std::string file, legalPathname, extension;
	std::string currMovie = movie;
	int counter = 0;
	u64 timing = 0;


	while (File::Exists(currMovie))
	{
		//PanicAlertT("Curr: %s", currMovie.c_str());

		g_recordfd.Open(currMovie, "rb");

		g_recordfd.ReadArray(&dtmHeader, 1);

		timing += dtmHeader.frameCount;

		g_recordfd.Close();

		counter++;

		SplitPathEscapeChar(currMovie, &legalPathname, &file, &extension);

		size_t npos = file.find_last_of('_');
		if (npos == std::string::npos)
			break;

		if (npos + 3 != file.length())
			break;

		std::string counterString = StringFromInt(counter);

		if (counter < 10)
			counterString = "0" + counterString;

		currMovie = legalPathname + file.substr(0, npos + 1) + counterString + extension;
	}

	return timing;
}
bool GetNextComparisonMovie(bool update)
{
	std::string file, legalPathname, extension;
	std::string currMovie = cmp_currentMovie;

	SplitPathEscapeChar(currMovie, &legalPathname, &file, &extension);

	size_t npos = file.find_last_of('_');
	if (npos == std::string::npos)
		return false;

	if (npos + 3 != file.length())
		return false;

	int counter = atoi(file.substr(npos + 1).c_str());
	counter++;

	std::string counterString = StringFromInt(counter);

	if (counter < 10)
		counterString = "0" + counterString;

	currMovie = legalPathname + file.substr(0, npos + 1) + counterString + extension;

	if (File::Exists(currMovie))
	{
		if (update)
		{
			if (cmp_currentMovie == cmp_leftMovie)
				cmp_currentMovie = cmp_leftMovie = currMovie;
			else
				cmp_currentMovie = cmp_rightMovie = currMovie;
		}

		return true;
	}

	return false;
}
void CancelComparison()
{
	cmp_requested = false;
	cmp_isRunning = false;
	cmp_leftFinished = false;
	cmp_rightFinished = false;
	cmp_loadState = false;
	cmp_movieFinished = false;
	cmp_justFinished = false;
	cmp_startTimerFrame = 0;
	cmp_currentMovie = "";
	cmp_currentBranch = "";
	cmp_curentBranchFrame = 0;

	SConfig::GetInstance().m_DumpFrames = false;
	SConfig::GetInstance().m_DumpAudio = false;
}
bool SaveMemCard()
{
	return saveMemCard;
}
};
