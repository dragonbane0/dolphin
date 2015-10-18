// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include <wx/bitmap.h>
#include <wx/defs.h>
#include <wx/dialog.h>
#include <wx/gdicmn.h>
#include <wx/image.h>
#include <wx/mstream.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/translation.h>
#include <wx/windowid.h>
#include <array>

#include "Common/Common.h"
#include "DolphinWX/TPSavefileManager.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"

#include "Common/StringUtil.h"
#include "Common/FileUtil.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/FileSystemGCWii.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeCreator.h"
#include "Core/ConfigManager.h"

#include "DolphinWX/ISOFile.h"
#include "DolphinWX/ISOProperties.h"

//Dragonbane
BEGIN_EVENT_TABLE(TPSavefileManager, wxDialog)

EVT_CHOICE(1, TPSavefileManager::OnSelectionChanged)
EVT_CHOICE(2, TPSavefileManager::OnSelectionChanged)
EVT_CHOICE(3, TPSavefileManager::OnSelectionChanged)
EVT_CHOICE(4, TPSavefileManager::OnSelectionChanged)

//EVT_TEXT(4, TPSavefileManager::OnTextChanged)

END_EVENT_TABLE()


struct StageMap
{
	std::string actualName;
	wxString displayedName;
};

struct RoomMap
{
	std::string stageRoomName;
	int roomID;
};


typedef std::array<StageMap, 24> OverworldArray;

OverworldArray OverworldList2 = { "F_SP102", wxT("Title Screen"), "F_SP103", wxT("Ordon Village"), "F_SP00", wxT("--Ordon Ranch"), 
"F_SP104", wxT("--Ordon Spring"), "F_SP108", wxT("Faron Woods"), "F_SP117", wxT("Sacred Grove"), "F_SP121", wxT("Hyrule Field"), 
"F_SP109", wxT("Kakariko Village"), "F_SP111", wxT("--Kakariko Graveyard"), "F_SP110", wxT("Death Mountain"), "F_SP122", wxT("Outside Castle Town"), 
"F_SP116", wxT("Castle Town"), "F_SP115", wxT("Lake Hylia"), "F_SP112", wxT("Zoras River"), "F_SP126", wxT("Upper Zoras River"), 
"F_SP127", wxT("Fishing Pond"), "F_SP113", wxT("Zoras Domain"), "F_SP123", wxT("Bublin 2"), "F_SP124", wxT("Gerudo Desert"), "F_SP118", wxT("Bublin Camp"), 
"F_SP125", wxT("Mirror Chamber"), "F_SP114", wxT("Snowpeak"), "F_SP128", wxT("Hidden Village"), "F_SP200", wxT("Hidden Skill") };


typedef std::array<StageMap, 13> InteriorArray;

InteriorArray InteriorList2 = { "R_SP01", wxT("Ordon Village"), "R_SP108", wxT("Faron Woods"), "R_SP107", wxT("Hyrule Castle Sewers"), 
"R_SP109", wxT("Kakariko Village"), "R_SP209", wxT("--Kakariko Graveyard"), "R_SP110", wxT("Death Mountain"), "R_SP160", wxT("Castle Town"), 
"R_SP161", wxT("--Star Game"), "R_SP116", wxT("--Telmas Bar"), "R_SP127", wxT("Fishing Pond"), "R_SP128", wxT("Hidden Village"), 
"R_SP300", wxT("Light Arrows Cutscene"), "R_SP301", wxT("Hyrule Castle Cutscenes") };


typedef std::array<StageMap, 30> DungeonArray;

DungeonArray DungeonList2 = { "D_MN05", wxT("Forest Temple"), "D_MN05B", wxT("--Ook"), "D_MN05A", wxT("--Diababa"), "D_MN04", wxT("Goron Mines"), 
"D_MN04B", wxT("--Dangoro"), "D_MN04A", wxT("--Fyrus"), "D_MN01", wxT("Lakebed Temple"), "D_MN01B", wxT("--Deku Toad"), "D_MN01A", wxT("--Morpheel"), 
"D_MN10", wxT("Arbiters Grounds"), "D_MN10B", wxT("--Death Sword"), "D_MN10A", wxT("--Stallord"), "D_MN11", wxT("Snowpeak Ruins"), 
"D_MN11B", wxT("--Darkhammer"), "D_MN11A", wxT("--Blizzeta"), "D_MN06", wxT("Temple of Time"), "D_MN06B", wxT("--Darknut"), "D_MN06A", wxT("--Armogohma"), 
"D_MN07", wxT("City in the Sky"), "D_MN07B", wxT("--Aeralfos"), "D_MN07A", wxT("--Argorok"), "D_MN08", wxT("Palace of Twilight"), 
"D_MN08B", wxT("--Phantom Zant 1"), "D_MN08C", wxT("--Phantom Zant 2"), "D_MN08A", wxT("--Zant Main Room"), "D_MN08D", wxT("--Zant Fight"),
"D_MN09", wxT("Hyrule Castle"), "D_MN09A", wxT("--Ganondorf Castle"), "D_MN09B", wxT("--Ganondorf Field"), "D_MN09C", wxT("--Ganondorf Defeated") };


typedef std::array<StageMap, 11> CaveArray;

CaveArray CaveList2 = { "D_SB10", wxT("Faron Woods Cave"), "D_SB02", wxT("Eldin Long Cave"), "D_SB04", wxT("Eldin Goron Stockcave"), 
"D_SB00", wxT("Lanayru Ice Puzzle Cave"), "D_SB03", wxT("Lake Hylia Long Cave"), "D_SB01", wxT("Cave of Ordeals"), "D_SB05", wxT("Grotto 1"), 
"D_SB06", wxT("Grotto 2"), "D_SB07", wxT("Grotto 3"), "D_SB08", wxT("Grotto 4"), "D_SB09", wxT("Grotto 5") };


typedef std::array<std::string, 22> OneRoomZeroArray;

OneRoomZeroArray OneRoomZeroStages2 = { "D_SB10" , "D_SB02" , "D_SB00", "D_SB03", "D_SB05", "D_MN09B", "D_MN09C", "R_SP300", "R_SP301", "F_SP102", "F_SP00", 
"F_SP109", "F_SP111", "F_SP126", "F_SP127", "F_SP124", "F_SP128", "F_SP200" };


typedef std::array<RoomMap, 50> RoomArray;

RoomArray OneRoomStages2 = { "D_SB04", 10, "D_SB06", 1, "D_SB07", 2, "D_SB08", 3, "D_SB09", 4, "D_MN05B", 51, "D_MN05A", 50, "D_MN04B", 51, "D_MN04A", 50,
"D_MN01B", 51, "D_MN01A", 50, "D_MN10B", 51, "D_MN10A", 50, "D_MN11A", 50, "D_MN06B", 51, "D_MN06A", 50, "D_MN07B", 51, "D_MN07A", 50, "D_MN08B", 51,
"D_MN08C", 52, "D_MN08A", 10, "R_SP161", 7, "F_SP104", 1, "F_SP112", 1, "F_SP123", 13, "F_SP125", 4 };



//Individual Room Arrays generated by Code Generator

RoomArray F_SP103_2 = { "Outside Links House",
1, "Main Village",
0 };

RoomArray F_SP108_2 = { "South Faron Entrance",
0, "Faron Spring",
1, "Transition Spring-Coro",
2, "Gate before Coro",
3, "Coro Area",
4, "Coro Shortcut to Mist",
8, "Mist Area",
5, "Transition Mist-North Faron",
11, "Small Key Cave",
14, "North Faron",
6 };

RoomArray F_SP117_2 = { "Lost Woods",
3, "Master Sword",
1, "Temple of Time",
2 };

RoomArray F_SP121_2 = { "Faron Main Field",
6, "Faron-Eldin Transition ",
1, "Faron-Lanayru Gate",
15, "Faron-Lanayru Transition",
14, "Eldin Entrance",
2, "Eldin Gorge",
3, "Eldin Gorge-Main Transition 1",
4, "Eldin Gorge-Main Transition 2",
5, "Eldin Main Field",
0, "Eldin-Lanayru Transition",
7, "Lanayru Entrance",
9, "Lanayru Main Field",
10, "Lanayru Main-Bridge Transition 1",
11, "Lanayru Main-Bridge Transition 2",
12, "Lanayru Lake Hylia Bridge",
13 };

RoomArray F_SP110_2 = { "Entrance",
0, "Entrance-Mountain Transition 1",
1, "Entrance-Mountain Transition 2",
2, "Mountain",
3 };

RoomArray F_SP122_2 = { "West Field",
8, "South Field",
16, "East Field",
17 };

RoomArray F_SP116_2 = { "Central",
0, "North",
1, "West",
2, "South",
3, "East",
4 };

RoomArray F_SP115_2 = { "Lake",
0, "Fountain",
1 };

RoomArray F_SP113_2 = { "Outside",
1, "Inside",
0 };

RoomArray F_SP118_2 = { "Main Camp",
1, "Before Arbiters Grounds",
3, "Camp Geometry (dont use!)",
0, "Beta Camp",
2 };

RoomArray F_SP114_2 = { "First Half",
0, "Transition Cave",
2, "Second Half",
1 };

RoomArray R_SP01_2 = { "Bo House",
0, "Shop House",
1, "Shield House",
2, "Sword House",
5, "Links House",
4, "Links House Storage",
7 };

RoomArray R_SP108_2 = { "Coros House",
0 };

RoomArray R_SP107_2 = { "Prison",
0, "Sewers",
1, "Rooftops",
2, "Zeldas Tower",
3 };

RoomArray R_SP109_2 = { "Renardos Sanctuary",
0, "Malos Shop",
3, "Hotel",
2, "Bug House",
6, "Barnes Shop",
1, "Bomb House",
5, "Top House",
4 };

RoomArray R_SP209_2 = { "Sanctuary Cave (Sky Cannon)",
7 };

RoomArray R_SP110_2 = { "Goron Elder Cave",
0 };

RoomArray R_SP160_2 = { "Malos Shop",
0, "Goron Shops",
4, "Agitha House",
3, "Fortune Teller House",
1, "Jovani House",
5, "Doctor House",
2 };

RoomArray R_SP116_2 = { "Telmas Bar",
5, "Jovani-Sewers Transition",
6 };

RoomArray R_SP127_2 = { "Henas House",
0 };

RoomArray R_SP128_2 = { "Impaz House",
0 };

RoomArray D_MN05_2 = { "Entrance",
22, "Main Room",
0, "Outside",
4, "Right Wing Boss Key",
1, "2nd Monkey",
2, "Left Wing",
3, "3rd Monkey",
5, "4th Monkey",
7, "North Wing Turning Bridge",
9, "6th Monkey",
11, "7th Monkey",
19, "Before Diababa",
12, "Final Monkey",
10 };

RoomArray D_MN04_2 = { "Entrance",
1, "Magnet Room",
3, "Roll Clipping",
4, "Before 1st Elder",
5, "1st Elder",
14, "Clawshot Switch",
6, "Outside",
7, "Before Dangoro",
9, "2nd Elder",
17, "Bow",
11, "3rd Elder",
16, "Bow-Magnet Shortcut Room",
13, "Before Fyrus",
12 };

RoomArray D_MN01_2 = { "Entrance",
0, "Stalactite Room",
1, "Central Room Outside",
2, "Central Room",
3, "Right Wing Lower",
8, "Before Deku Toad",
9, "Right Wing Upper",
7, "Right Wing Water Supply",
10, "Left Wing Lower",
12, "Left Wing Upper",
11, "Left Wing Water Supply",
13, "Before Boss Key",
5, "Boss Key",
6 };

RoomArray D_MN10_2 = { "Entrance",
0, "Before Main Room",
1, "Main Poe Room",
2, "Right Wing",
15, "2nd Poe",
3, "Before 3rd Poe",
4, "3rd Poe",
5, "After 3rd Poe",
14, "Left Wing 1",
6, "Left Wing 2",
7, "Before 4th Poe",
12, "4th Poe",
8, "Boss Key",
9, "Big Turning Room",
16, "Ooccoo",
10, "Before Death Sword",
11, "Epic Spinner Room",
13 };

RoomArray D_MN11_2 = { "Entrance",
0, "Yeta",
1, "Yeto",
2, "Ice Puzzle",
3, "Courtyard",
4, "Before Ordon Pumpkin",
13, "Ordon Pumpkin",
5, "Third Poe",
6, "Double Freezard",
7, "Double LJA",
8, "Ice Cannon Room",
9, "Boss Key",
11 };

RoomArray D_MN11B_2 = { "Darkhammer",
51, "Beta Room",
49 };

RoomArray D_MN06_2 = { "Entrance",
0, "First Staircase",
1, "Turning Platform",
2, "Statue Throws",
3, "Second Staircase",
4, "Scale Room",
5, "Boss Key",
6, "Third Staircase",
7, "Before Gohma",
8 };

RoomArray D_MN07_2 = { "Entrance",
0, "Oocca Shop",
16, "Before Main Room",
1, "Main Room",
2, "Left Wing Outside ",
6, "Left Wing Inside",
10, "Right Wing Outside",
3, "Right Wing Inside 1",
4, "Right Wing Inside 2",
7, "Right Wing Inside 3",
8, "Before Aeralfos",
5, "Big Baba Room",
11, "After Big Baba Outside",
12, "Before Boss Key Outside",
13, "North Wing Outside",
14, "North Wing Inside",
15 };

RoomArray D_MN08_2 = { "Entrance",
0, "Left Wing 1",
1, "Left Wing 2",
2, "Right Wing 1",
4, "Right Wing 2",
5, "Double Sol",
7, "Boss Key",
11, "Early Platform",
8, "Messengers before Zant",
9, "Beta Zant Room",
10 };

RoomArray D_MN08D_2 = { "Intro",
50, "Diababa Phase",
53, "Goron Mines Phase",
54, "Lakebed Phase",
55, "Ook Phase",
56, "Blizzeta Phase",
57, "Final Hyrule Phase",
60 };

RoomArray D_MN09_2 = { "Entrance",
11, "Outside Left Wing",
13, "Outside Right Wing",
14, "Graveyard",
9, "Inside Main Hall",
1, "Inside Darknut 1",
2, "Inside Left Wing 1",
3, "Inside Left Wing 2",
4, "Inside Right Wing 1",
5, "Inside Right Wing 2",
6, "Outside Boss Key",
15, "Inside Final Ascension",
12, "Treasure Room",
8 };

RoomArray D_MN09A_2 = { "Intro Outside",
51, "Fight Inside",
50 };

RoomArray D_SB01_2 = { "Room 1",
0, "Room 2",
1, "Room 3",
2, "Room 4",
3, "Room 5",
4, "Room 6",
5, "Room 7",
6, "Room 8",
7, "Room 9",
8, "Room 10",
9, "Room 11",
10, "Room 12",
11, "Room 13",
12, "Room 14",
13, "Room 15",
14, "Room 16",
15, "Room 17",
16, "Room 18",
17, "Room 19",
18, "Room 20",
19, "Room 21",
20, "Room 22",
21, "Room 23",
22, "Room 24",
23, "Room 25",
24, "Room 26",
25, "Room 27",
26, "Room 28",
27, "Room 29",
28, "Room 30",
29, "Room 31",
30, "Room 32",
31, "Room 33",
32, "Room 34",
33, "Room 35",
34, "Room 36",
35, "Room 37",
36, "Room 38",
37, "Room 39",
38, "Room 40",
39, "Room 41",
40, "Room 42",
41, "Room 43",
42, "Room 44",
43, "Room 45",
44, "Room 46",
45, "Room 47",
46, "Room 48",
47, "Room 49",
48, "Room 50",
49 };


//Global Array for Rooms

struct RoomArraysMap
{
	std::string stageName;
	RoomArray roomArray;
};

typedef std::array<RoomArraysMap, 50> RoomArrays;

RoomArrays AllRoomArrays2 = { "F_SP103", F_SP103_2, "F_SP108", F_SP108_2, "F_SP117", F_SP117_2, "F_SP121", F_SP121_2, "F_SP110", F_SP110_2, "F_SP122", F_SP122_2, 
"F_SP116", F_SP116_2, "F_SP115", F_SP115_2, "F_SP113", F_SP113_2, "F_SP118", F_SP118_2, "F_SP114", F_SP114_2, "R_SP01", R_SP01_2, "R_SP108", R_SP108_2, "R_SP107", R_SP107_2, 
"R_SP109", R_SP109_2, "R_SP209", R_SP209_2, "R_SP110", R_SP110_2, "R_SP160", R_SP160_2, "R_SP116", R_SP116_2, "R_SP127", R_SP127_2, "R_SP128", R_SP128_2, "D_MN05", D_MN05_2, 
"D_MN04", D_MN04_2, "D_MN01", D_MN01_2, "D_MN10", D_MN10_2, "D_MN11", D_MN11_2, "D_MN11B", D_MN11B_2, "D_MN06", D_MN06_2, "D_MN07", D_MN07_2, "D_MN08", D_MN08_2, 
"D_MN08D", D_MN08D_2, "D_MN09", D_MN09_2, "D_MN09A", D_MN09A_2, "D_SB01", D_SB01_2 };



//Read RARC Stuff
const int HeaderSize2 = 64;
const int NodeSize2 = 16;
const int EntrySize2 = 20;

//File Header Vars
std::string headerTag2;
u32 DataStartOffset2;
u32 NumNodes2;
u32 FileEntriesOffset2;
u32 StringTableOffset2;


TPSavefileManager::TPSavefileManager(wxWindow* parent, wxWindowID id, const wxString& title, const wxPoint& pos, const wxSize& size, long style) : wxDialog(parent, id, title, pos, size, style)
{
	SetSizeHints(wxDefaultSize, wxDefaultSize);

	wxBoxSizer* bSizer1;
	bSizer1 = new wxBoxSizer(wxVERTICAL);


	//Text 1
	m_staticText1 = new wxStaticText(this, wxID_ANY, wxT("Type:"), wxDefaultPosition, wxDefaultSize, 0);
	m_staticText1->Wrap(-1);
	m_staticText1->SetFont(wxFont(12, 74, 90, 92, false, wxT("Arial")));
	bSizer1->Add(m_staticText1, 0, wxALL, 5);


	//Type Choice
	wxString m_choice_typeChoices[] = { wxT("Overworld"), wxT("Interior"), wxT("Dungeon"), wxT("Cave") };
	int m_choice_typeNChoices = sizeof(m_choice_typeChoices) / sizeof(wxString);
	m_choice_type = new wxChoice(this, 1, wxDefaultPosition, wxDefaultSize, m_choice_typeNChoices, m_choice_typeChoices, 0);
	bSizer1->Add(m_choice_type, 0, wxALL, 5);


	//Text 2
	m_staticText11 = new wxStaticText(this, wxID_ANY, wxT("Stage:"), wxDefaultPosition, wxDefaultSize, 0);
	m_staticText11->Wrap(-1);
	m_staticText11->SetFont(wxFont(12, 74, 90, 92, false, wxT("Arial")));
	bSizer1->Add(m_staticText11, 0, wxALL, 5);


	//Stage Choice
	wxArrayString m_choice_stageChoices;
	m_choice_stage = new wxChoice(this, 2, wxDefaultPosition, wxSize(200, -1), m_choice_stageChoices, 0);
	bSizer1->Add(m_choice_stage, 0, wxALL, 5);


    //Text 3
	m_staticText2 = new wxStaticText(this, wxID_ANY, wxT("Room:"), wxDefaultPosition, wxDefaultSize, 0);
	m_staticText2->Wrap(-1);
	m_staticText2->SetFont(wxFont(12, 74, 90, 92, false, wxT("Arial")));
	bSizer1->Add(m_staticText2, 0, wxALL, 5);

	//Room Choice
	wxArrayString m_choice_roomChoices;
	m_choice_room = new wxChoice(this, 3, wxDefaultPosition, wxSize(200, -1), m_choice_roomChoices, 0);
	bSizer1->Add(m_choice_room, 0, wxALL, 5);

	//Text 4
	m_staticText21 = new wxStaticText(this, wxID_ANY, wxT("Spawn Point:"), wxDefaultPosition, wxDefaultSize, 0);
	m_staticText21->Wrap(-1);
	m_staticText21->SetFont(wxFont(12, 74, 90, 92, false, wxT("Arial")));
	bSizer1->Add(m_staticText21, 0, wxALL, 5);

	//Textbox Spawn
	//m_textCtrl_spawn = new wxTextCtrl(this, 4, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0);
	//m_textCtrl_spawn->SetMaxLength(2);
	//bSizer1->Add(m_textCtrl_spawn, 0, wxALL, 5);

	//Spawn Choice
	wxArrayString m_choice_spawnChoices;
	m_choice_spawn = new wxChoice(this, 4, wxDefaultPosition, wxSize(200, -1), m_choice_spawnChoices, wxCB_SORT);
	bSizer1->Add(m_choice_spawn, 0, wxALL, 5);


	SetSizer(bSizer1);
	Layout();

	bSizer1->Fit(this);

	Centre(wxBOTH);

	Bind(wxEVT_CLOSE_WINDOW, &TPSavefileManager::OnCloseWindow, this);
}

void TPSavefileManager::OnSelectionChanged(wxCommandEvent& event)
{
	if (!Core::IsRunning())
	{
		m_choice_stage->Clear();
		m_choice_room->Clear();
		m_choice_spawn->Clear();
		return;
	}

	std::string gameID = Memory::Read_String(0x0, 6);

	if (gameID.compare("GZ2E01") && gameID.compare("GZ2P01"))
	{
		m_choice_stage->Clear();
		m_choice_room->Clear();
		m_choice_spawn->Clear();
		return;
	}

	if (event.GetId() == 1) //Type Choice
	{
		m_choice_stage->Clear();
		m_choice_room->Clear();
		//m_textCtrl_spawn->Clear
		m_choice_spawn->Clear();

		FillStages(event.GetInt());
	}

	if (event.GetId() == 2) //Stage Choice
	{
		std::string finalStageName;
		u32 stageAddress;

		m_choice_room->Clear();
		//m_textCtrl_spawn->Clear();
		m_choice_spawn->Clear();
	
		if (m_choice_type->GetSelection() == 0)
			finalStageName = OverworldList2[event.GetInt()].actualName;
		else if (m_choice_type->GetSelection() == 1)
			finalStageName = InteriorList2[event.GetInt()].actualName;
		else if (m_choice_type->GetSelection() == 2)
			finalStageName = DungeonList2[event.GetInt()].actualName;
		else if (m_choice_type->GetSelection() == 3)
			finalStageName = CaveList2[event.GetInt()].actualName;

		if (!gameID.compare("GZ2E01"))
			stageAddress = 0x406218;
		else if (!gameID.compare("GZ2P01"))
			stageAddress = 0x4081b8;

		Memory::Write_String(finalStageName, stageAddress);

		if (finalStageName.length() < 7)
		{
			Memory::Write_U8(0x0, stageAddress + 0x6);
		}

		FillRooms(finalStageName);
	}

	if (event.GetId() == 3) //Room Choice
	{
		int finalRoomID;
		u32 roomAddress;
		int selectedStageID = m_choice_stage->GetSelection();
		std::string selectedStage;

		bool foundOneRoom = false;
		bool foundOneRoomZero = false;

		//m_textCtrl_spawn->Clear();
		m_choice_spawn->Clear();

		if (m_choice_type->GetSelection() == 0)
			selectedStage = OverworldList2[selectedStageID].actualName;
		else if (m_choice_type->GetSelection() == 1)
			selectedStage = InteriorList2[selectedStageID].actualName;
		else if (m_choice_type->GetSelection() == 2)
			selectedStage = DungeonList2[selectedStageID].actualName;
		else if (m_choice_type->GetSelection() == 3)
			selectedStage = CaveList2[selectedStageID].actualName;

		if (!gameID.compare("GZ2E01"))
			roomAddress = 0x406221;
		else if (!gameID.compare("GZ2P01"))
			roomAddress = 0x4081c1;


		for (OneRoomZeroArray::const_iterator it = OneRoomZeroStages2.begin(); it != OneRoomZeroStages2.end(); ++it)
		{
			if (!it->compare(selectedStage))
			{
				foundOneRoomZero = true;
				finalRoomID = 0;

				break;
			}
		}

		if (!foundOneRoomZero)
		{
			for (RoomArray::const_iterator it = OneRoomStages2.begin(); it != OneRoomStages2.end(); ++it)
			{
				if (it->stageRoomName.length() < 2)
					break;

				if (!it->stageRoomName.compare(selectedStage))
				{
					foundOneRoom = true;
					finalRoomID = it->roomID;

					break;
				}
			}
		}


		if (!foundOneRoomZero && !foundOneRoom)
		{
			for (RoomArrays::const_iterator it1 = AllRoomArrays2.begin(); it1 != AllRoomArrays2.end(); ++it1)
			{
				if (it1->stageName.length() < 2)
					break;

				if (!it1->stageName.compare(selectedStage))
				{
					for (RoomArray::const_iterator it2 = it1->roomArray.begin(); it2 != it1->roomArray.end(); ++it2)
					{
						if (it2->stageRoomName.length() < 2)
							break;

						if (!it2->stageRoomName.compare((std::string)m_choice_room->GetStringSelection()))
						{
							finalRoomID = it2->roomID;
							break;
						}
					}

					break;
				}
			}
		}

		Memory::Write_U8((u8)finalRoomID, roomAddress);


		//Get Spawn Points
		std::stringstream ss;

		ss << finalRoomID;
		std::string roomString = ss.str();

		if (roomString.length() < 2)
			roomString = StringFromFormat("0%s", roomString.c_str());

		//Open Room File
		std::string fullPath = StringFromFormat("res/Stage/%s/R%s_00.arc", selectedStage.c_str(), roomString.c_str());
		std::string ISOPath = SConfig::GetInstance().m_strFilename;

		static DiscIO::IVolume *OpenISO = nullptr;
		static DiscIO::IFileSystem *pFileSystem = nullptr;

		OpenISO = DiscIO::CreateVolumeFromFilename(ISOPath);
		pFileSystem = DiscIO::CreateFileSystem(OpenISO);

		u64 fileSize = pFileSystem->GetFileSize(fullPath);

		//Limit read size to 128 MB
		size_t readSize = (size_t)std::min(fileSize, (u64)0x08000000);

		std::vector<u8> inputBuffer(readSize);

		pFileSystem->ReadFile(fullPath, &inputBuffer[0], readSize);

		delete pFileSystem;
		delete OpenISO;

		std::string index = ReadString(&inputBuffer[0], 0, 4);
		u32 outputSize;
		size_t decompressedSize;
		std::vector<u8> outputBuffer;

		//Decompress if neccesary
		if (!index.compare("Yaz0"))
		{
			outputSize = Read32(&inputBuffer[0], 4);
			decompressedSize = outputSize;

			outputBuffer = std::vector<u8>(decompressedSize);
			DecompressYaz0(&inputBuffer[0], 0, &outputBuffer[0]);
		}
		else
		{
			outputSize = readSize;
			decompressedSize = outputSize;

			outputBuffer = inputBuffer;
		}

		
		//File::IOFile f("C:\\extract\\test.arc", "wb");
		//f.WriteBytes(&outputBuffer[0], decompressedSize);
	
		//File Header
		headerTag2 = ReadString(&outputBuffer[0], 0, 4);
		DataStartOffset2 = Read32(&outputBuffer[0], 12);
		NumNodes2 = Read32(&outputBuffer[0], 32);
		FileEntriesOffset2 = Read32(&outputBuffer[0], 44);
		StringTableOffset2 = Read32(&outputBuffer[0], 52);

		//Get Root and Files
		GetFileNode(&outputBuffer[0], 0, outputSize);
	}

	if (event.GetId() == 4) //Spawn Choice
	{
		u32 spawnAddress;
		std::string spawnPoint = (std::string)m_choice_spawn->GetStringSelection();

		spawnPoint = spawnPoint.substr(0, 2);

		std::transform(spawnPoint.begin(), spawnPoint.end(), spawnPoint.begin(), ::tolower);

		if (!gameID.compare("GZ2E01"))
			spawnAddress = 0x406220;
		else if (!gameID.compare("GZ2P01"))
			spawnAddress = 0x4081c0;


		int spawnPointFinal;
		std::istringstream buffer(spawnPoint);

		buffer >> std::hex >> spawnPointFinal;

		Memory::Write_U8((u8)spawnPointFinal, spawnAddress);
	}
}

void TPSavefileManager::GetFileNode(u8* Data, u32 Offset, u32 Size)
{
	//File Node
	std::string nodeTag;
	u32 FilenameOffset;
	u16 NumFileEntries;
	u32 FirstFileEntryOffset;

	std::string NodeName;

	Offset = HeaderSize2 + (Offset * NodeSize2);

	nodeTag = ReadString(Data, (int)Offset, 4);
	FilenameOffset = Read32(Data, (int)Offset + 4);
	NumFileEntries = Read16(Data, (int)Offset + 10);
	FirstFileEntryOffset = Read32(Data, (int)Offset + 12);

	NodeName = ReadStringFull(Data, (int)(FilenameOffset + StringTableOffset2 + 32), Size);

	for (int i = 0; i < NumFileEntries; ++i)
	{
		u32 ReadOffset = (u32)(FileEntriesOffset2 + (FirstFileEntryOffset * EntrySize2) + (i * EntrySize2) + 32);

		//File Entry
		u16 ID;
		u16 Unknown1;
		u16 Unknown2;
		u16 FilenameOffset_FileEntry;
		u32 DataOffset;
		u32 DataSize;

		std::string FileName;
		bool IsCompressed;

		ID = Read16(Data, (int)ReadOffset);
		Unknown1 = Read16(Data, (int)ReadOffset + 2);
		Unknown2 = Read16(Data, (int)ReadOffset + 4);
		FilenameOffset_FileEntry = Read16(Data, (int)ReadOffset + 6);
		DataOffset = Read32(Data, (int)ReadOffset + 8);
		DataSize = Read32(Data, (int)ReadOffset + 12);

		FileName = ReadStringFull(Data, (int)(FilenameOffset_FileEntry + StringTableOffset2 + 32), Size);


		if (ID == 0xFFFF || Unknown2 == 0x0200)         // 0x2000 correct???
		{
			if (FilenameOffset_FileEntry != 0 && FilenameOffset_FileEntry != 2)
				GetFileNode(Data, DataOffset, Size);
		}
		else
		{
			std::string fileExtension = FileName.substr(FileName.find_last_of('.'));

			if (!fileExtension.compare(".dzr")) //|| !fileExtension.compare(".dzs"))
			{
				u32 tempOffset = (DataStartOffset2 + DataOffset + 32);
				std::string tempString = ReadString(Data, (int)tempOffset, 4);

				if (!tempString.compare("Yaz0"))
					IsCompressed = true;
				else
					IsCompressed = false;

				std::vector<u8> fileData;
				u32 realSize;

				if (IsCompressed == true)
				{
					u32 fileDataSize = Read32(Data, (int)(tempOffset + 4));
					fileData = std::vector<u8>(fileDataSize);
					DecompressYaz0(Data, tempOffset, &fileData[0]);

					realSize = fileDataSize;
				}
				else
				{
					fileData = std::vector<u8>(DataSize);

					for (u32 i = 0; i < DataSize; i++)
					{
						u32 address = tempOffset + i;
						fileData[i] = Data[address];
					}

					realSize = DataSize;
				}

				/*FILE* file;

				char dest[255];
				sprintf(dest, "E:\\TP Stuff\\Maps\\Test\\%s", FileName.c_str());

				file = fopen(dest, "wb");

				fwrite(&fileData[0], 1, (size_t)realSize, file);

				fclose(file);*/
				

				//Check File Contents
				
				//DZx
				int dZxOffset = 0;

				u32 ChunkCount = Read32(&fileData[0], dZxOffset);
				if (ChunkCount == 0)
					continue;

				dZxOffset += 4;
				for (u32 i = 0; i < ChunkCount; i++) //Chunks
				{
					std::string Tag;
					u32 Elements, chunkOffset;

					Tag = ReadString(&fileData[0], dZxOffset, 4);
					Elements = Read32(&fileData[0], dZxOffset + 4);
					chunkOffset = Read32(&fileData[0], dZxOffset + 8);

					int chunkReadOffset = (int)chunkOffset;
					for (u32 i = 0; i < Elements; i++) //Chunk Elements
					{	
						//First Switch
						if (!Tag.compare("ACTR") || !Tag.compare("TGOB") || !Tag.compare("TRES"))
						{
							chunkReadOffset += 0x20;
							break;
						}
						else if (!Tag.compare("RPPN") || !Tag.compare("SHIP"))
						{
							chunkReadOffset += 0x10;
							break;
						}
						else if (!Tag.compare("Door"))
						{
							chunkReadOffset += 0x24; 
							break;
						}
						else if (!Tag.compare("LGTV"))
						{
							chunkReadOffset += 0x1C; 
							break;  /* ????? */
						}
						else if (!Tag.compare("MULT")) //Typically in DZS
						{
							chunkReadOffset += 0x0C; 
							break;
						}
						else if (!Tag.compare("PLYR")) //Typically in DZR
						{
							//Add Spawn Point to list
							AddSpawnPoint(&fileData[0], chunkReadOffset);

							chunkReadOffset += 0x20; 
							continue;
						}
						else if (!Tag.compare("SCLS"))
						{
							chunkReadOffset += 0x0D;
							break;
						}


						//Second Switch
						if (!Tag.substr(0, 3).compare("ACT") || !Tag.substr(0, 3).compare("TRE"))
						{
							chunkReadOffset += 0x20;
							break;
						}
						else if (!Tag.substr(0, 3).compare("SCO"))
						{
							chunkReadOffset += 0x24; 
							break;
						}
						else if (!Tag.substr(0, 3).compare("PLY"))
						{
							//Add Spawn Point to list
							AddSpawnPoint(&fileData[0], chunkReadOffset);

							chunkReadOffset += 0x20; 
							break;
						}
						else
						{
							break;
						}
					}

					dZxOffset += 12;
				}
			}
		}
	}
}

void TPSavefileManager::AddSpawnPoint(u8* Data, u32 Offset)
{
	//std::string _Name;
	//u32 Parameters;
	//u16 rotX, rotY, rotZ;
	//u16 _Unknown;

	/*
	_Name = ReadString(Data, Offset, 8);
	_Parameters = Read32(Data, Offset + 8);

	rotX = Read16(Data, Offset + 0x18);
	rotY = Read16(Data, Offset + 0x1A);
	rotZ = Read16(Data, Offset + 0x1C); //Spawn ID

	_Unknown = Read16(Data, Offset + 0x1E);
	*/

	u32 Parameters;
	u16 spawnID;

	Parameters = Read32(Data, Offset + 8);
	spawnID = Read16(Data, Offset + 0x1C);

	int spawnTemp = spawnID;
	int parameterTemp = Parameters;

	std::stringstream ss;

	ss << std::hex << spawnTemp;
	std::string spawnString = ss.str();

	if (spawnString.length() < 2)
		spawnString = StringFromFormat("0%s", spawnString.c_str());

	std::transform(spawnString.begin(), spawnString.end(), spawnString.begin(), ::toupper);


	std::stringstream ss2;

	ss2 << std::hex << parameterTemp;
	std::string parameterString = ss2.str();

	while (parameterString.length() < 8)
	{
		parameterString = StringFromFormat("0%s", parameterString.c_str());
	}

	std::transform(parameterString.begin(), parameterString.end(), parameterString.begin(), ::toupper);

	std::string eventID = parameterString.substr(0, 2);

	std::string outputString = StringFromFormat("%s (Event ID: %s)", spawnString.c_str(), eventID.c_str());

	m_choice_spawn->Append(outputString);
}

u8 TPSavefileManager::Read8(u8* Data, int Offset)
{
	return (Data[Offset]);
}

u16 TPSavefileManager::Read16(u8* Data, int Offset)
{
	return (u16)((Data[Offset] << 8) | Data[Offset + 1]);
}

u32 TPSavefileManager::Read32(u8* Data, int Offset)
{
	return (u32)((Data[Offset] << 24) | (Data[Offset + 1] << 16) | (Data[Offset + 2] << 8) | Data[Offset + 3]);
}

std::string TPSavefileManager::ReadString(u8* Data, int Offset, int Length)
{
	std::string output = "";

	for (int i = 0; i < Length; i++)
	{
		int address = Offset + i;
		std::string result;

		u8 var = Data[address];

		result = var;

		output.append(result);
	}

	return output;
}

std::string TPSavefileManager::ReadStringFull(u8* Data, int Offset, u32 dataSize)
{
	if ((u32)Offset >= dataSize) return "";

	int startOffset;
	int Length = 0;

	while (Data[Offset] == 0) Offset++;

	startOffset = Offset;

	while (Data[Offset] != 0)
	{
		Offset++;
		Length++;
	}
	return ReadString(Data, startOffset, Length);
}

void TPSavefileManager::DecompressYaz0(u8* Input, u32 Offset, u8* Output)
 {
	u32 Size = Read32(Input, (int)(Offset + 4));

	int SrcPlace = (int)Offset + 0x10;
	u32	DstPlace = 0;

	u32 ValidBitCount = 0;
	u8 CodeByte = 0;
	while (DstPlace < Size)
	{
		if (ValidBitCount == 0)
		{
			CodeByte = Input[SrcPlace];
			++SrcPlace;
			ValidBitCount = 8;
		}

		if ((CodeByte & 0x80) != 0)
		{
			Output[DstPlace] = Input[SrcPlace];
			DstPlace++;
			SrcPlace++;
		}
		else
		{
			u8 Byte1 = Input[SrcPlace];
			u8 Byte2 = Input[SrcPlace + 1];
			SrcPlace += 2;

			u32 Dist = static_cast<u32>(((Byte1 & 0xF) << 8) | Byte2);
			u32 CopySource = static_cast<u32>(DstPlace - (Dist + 1));
			u32 NumBytes = static_cast<u32>(Byte1 >> 4);
			if (NumBytes == 0)
			{
				NumBytes = static_cast<u32>(Input[SrcPlace] + 0x12);
				SrcPlace++;
			}
			else
			{
				NumBytes += 2;
			}

			u32 i;
			for (i = 0; i < NumBytes; ++i)
			{
				Output[DstPlace] = Output[CopySource];
				CopySource++;
				DstPlace++;
			}
		}

		CodeByte <<= 1;
		ValidBitCount -= 1;
	 }
}

void TPSavefileManager::OnTextChanged(wxCommandEvent& event)
{
	/*
	if (!Core::IsRunning())
	{
		return;
	}

	std::string gameID = Memory::Read_String(0x0, 6);

	if (gameID.compare("GZ2E01") && gameID.compare("GZ2P01"))
	{
		return;
	}

	if (event.GetId() != 4)
		return;


	std::string textContent = (std::string)m_textCtrl_spawn->GetValue();
	std::string textContentVisible = textContent;

	long point = m_textCtrl_spawn->GetInsertionPoint();

	std::transform(textContentVisible.begin(), textContentVisible.end(), textContentVisible.begin(), ::toupper);

	m_textCtrl_spawn->ChangeValue((wxString)textContentVisible);
	m_textCtrl_spawn->SetInsertionPoint(point);


	if (m_textCtrl_spawn->GetValue().length() < 2)
		return;


	u32 spawnAddress;

	std::transform(textContent.begin(), textContent.end(), textContent.begin(), ::tolower);

	if (!gameID.compare("GZ2E01"))
		spawnAddress = 0x406220;
	else if (!gameID.compare("GZ2P01"))
		spawnAddress = 0x4081c0;


	int spawnPoint;
	std::istringstream buffer(textContent);

	buffer >> std::hex >> spawnPoint;

	Memory::Write_U8((u8)spawnPoint, spawnAddress);
	*/
}

void TPSavefileManager::OnCloseWindow(wxCloseEvent& event)
{
	if (event.CanVeto())
	{
		event.Skip(false);
		Show(false);
	}
}

void TPSavefileManager::FillStages(int selectedID)
{
	if (selectedID == 0) //Overworld
	{
		for (OverworldArray::const_iterator it = OverworldList2.begin(); it != OverworldList2.end(); ++it)
		{
			m_choice_stage->Append(it->displayedName);
		}

	}
	else if (selectedID == 1) //Interior
	{
		for (InteriorArray::const_iterator it = InteriorList2.begin(); it != InteriorList2.end(); ++it)
		{
			m_choice_stage->Append(it->displayedName);
		}

	}
	else if (selectedID == 2) //Dungeon
	{
		for (DungeonArray::const_iterator it = DungeonList2.begin(); it != DungeonList2.end(); ++it)
		{
			m_choice_stage->Append(it->displayedName);
		}

	}
	else if (selectedID == 3) //Cave
	{
		for (CaveArray::const_iterator it = CaveList2.begin(); it != CaveList2.end(); ++it)
		{
			m_choice_stage->Append(it->displayedName);
		}
	}
}

void TPSavefileManager::FillRooms(std::string selectedStage)
{
	bool foundOneRoom = false;
	std::string finalOneRoomName;
	std::string realStageName;

	for (OneRoomZeroArray::const_iterator it = OneRoomZeroStages2.begin(); it != OneRoomZeroStages2.end(); ++it)
	{
		if (!it->compare(selectedStage))
		{
			foundOneRoom = true;
			break;
		}
	}

	if (!foundOneRoom)
	{
		for (RoomArray::const_iterator it = OneRoomStages2.begin(); it != OneRoomStages2.end(); ++it)
		{
			if (it->stageRoomName.length() < 2)
				break;

			if (!it->stageRoomName.compare(selectedStage))
			{
				foundOneRoom = true;
				break;
			}
		}
	}

	if (foundOneRoom)
	{
		std::string displayedStageName = GetDisplayedStageName(selectedStage);
		std::remove_copy(displayedStageName.begin(), displayedStageName.end(), std::back_inserter(finalOneRoomName), '-');

		m_choice_room->Append(wxString(finalOneRoomName));
	}
	else
	{
		for (RoomArrays::const_iterator it1 = AllRoomArrays2.begin(); it1 != AllRoomArrays2.end(); ++it1)
		{
			if (it1->stageName.length() < 2)
				break;

			if (!it1->stageName.compare(selectedStage))
			{
				for (RoomArray::const_iterator it2 = it1->roomArray.begin(); it2 != it1->roomArray.end(); ++it2)
				{
					if (it2->stageRoomName.length() < 2)
						break;

					m_choice_room->Append(wxString(it2->stageRoomName));
				}

				break;
			}
		}
	}
}

std::string TPSavefileManager::GetDisplayedStageName(std::string stageName)
{
	int selectedID = m_choice_type->GetSelection();

	if (selectedID == 0) //Overworld
	{
		for (OverworldArray::const_iterator it = OverworldList2.begin(); it != OverworldList2.end(); ++it)
		{
			if (!it->actualName.compare(stageName))
			{
				return (std::string)it->displayedName;
			}
		}

	}
	else if (selectedID == 1) //Interior
	{
		for (InteriorArray::const_iterator it = InteriorList2.begin(); it != InteriorList2.end(); ++it)
		{
			if (!it->actualName.compare(stageName))
			{
				return (std::string)it->displayedName;
			}
		}

	}
	else if (selectedID == 2) //Dungeon
	{
		for (DungeonArray::const_iterator it = DungeonList2.begin(); it != DungeonList2.end(); ++it)
		{
			if (!it->actualName.compare(stageName))
			{
				return (std::string)it->displayedName;
			}
		}

	}
	else if (selectedID == 3) //Cave
	{
		for (CaveArray::const_iterator it = CaveList2.begin(); it != CaveList2.end(); ++it)
		{
			if (!it->actualName.compare(stageName))
			{
				return (std::string)it->displayedName;
			}
		}
	}

	return "Not Found";
}