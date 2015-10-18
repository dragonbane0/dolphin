// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include <wx/bitmap.h>
#include <wx/defs.h>
#include <wx/button.h>
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
#include <wx/msgdlg.h>
#include <array>
#include <wx/filepicker.h>
#include <wx/utils.h>

#include "Common/Common.h"
#include "DolphinWX/TWWSuperswim.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Common/IniFile.h"

#include "Common/StringUtil.h"
#include "Common/FileUtil.h"
#include "DiscIO/Filesystem.h"

#include "DiscIO/FileSystemGCWii.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeCreator.h"
#include "Core/ConfigManager.h"
#include "Core/Movie.h"

#include "DolphinWX/ISOFile.h"
#include "DolphinWX/ISOProperties.h"
#include "DolphinWX/WxUtils.h"

//Dragonbane
BEGIN_EVENT_TABLE(TWWSuperswim, wxDialog)

EVT_BUTTON(1, TWWSuperswim::OnButtonPressed) //Pick Links Current Pos
EVT_CHOICE(2, TWWSuperswim::OnSelectionChanged) //Isle Selection
EVT_BUTTON(3, TWWSuperswim::OnButtonPressed) //Start
EVT_BUTTON(4, TWWSuperswim::OnButtonPressed) //Cancel

END_EVENT_TABLE()

struct IsleMap
{
	wxString isleName;
	float xPos;
	float zPos;
};

//Default List
typedef std::array<IsleMap, 34> IsleArray;

IsleArray isleListDefault = {
	wxT("Bird's Peak Rock"), 301517.8125f, 104112.1016f,
	wxT("Boating Course Chest"), 201092.2031f, 312988.3438f,
	wxT("Cliff Plateau"), 280011.0625f, 180844.3906f,
	wxT("Crescent Moon Island"), 100508.3125f, -297068.0f,
	wxT("Diamond Steppe"), -297923.75f, 199708.2969f,
	wxT("Dragon Roost East"), 200088.4688f, -198979.2031f,
	wxT("Dragon Roost West"), 196828.4844f, -199722.1562f,
	wxT("Eastern Fairy"), 119799.2969f, -80216.41406f,
	wxT("Eastern Triangle"), 200431.9062f, 101.9568634f,
	wxT("Five-Eye Reef Platform"), -189530.5938f, 204548.3438f,
	wxT("Forest Haven"), 217502.7812f, 194947.7969f,
	wxT("Forsaken Fortress"), -305887.5312f, -300624.25f,
	wxT("Gale Island"), 19721.27148f, -317009.5312f,
	wxT("Greatfish"), -201257.875f, -201257.875f,
	wxT("Headstone Island"), -80323.29688f, 323364.0625f,
	wxT("Ice Ring Isle"), 79861.92969f, 216436.1719f,
	wxT("Islet of Steel"), -180003.0469f, 81914.17188f,
	wxT("Mother and Child Isles"), -177817.0625f, -197830.1094f,
	wxT("Northern Fairy"), -79864.99219f, -279698.8438f,
	wxT("Northern Fairy Sub"), -120601.2969f, -308542.375f,
	wxT("Northern Triangle"), -217.5435181f, -100443.3359f,
	wxT("Needle Rock"), -278752.75f, 123612.8125f,
	wxT("Outset East"), -195227.5469f, 317115.75f,
	wxT("Outset West"), -202891.2656f, 314029.2188f,
	wxT("Overlook"), 299919.8438f, -298657.75f,
	wxT("Private Oasis"), 117374.9297f, 119317.1875f,
	wxT("Shark Island"), -100546.9844f, 221514.3906f,
	wxT("Southern Triangle"), -263.8293762f, 100381.1484f,
	wxT("Stonewatcher"), -121749.1953f, 98003.4375f,
	wxT("Stonewatcher Platform"), -90535.0f, 105583.2031f,
	wxT("Tingle Island"), -100022.4297f, -80407.86719f,
	wxT("Two-Eye Reef"), 2129.237305f, 274331.3438f,
	wxT("Windfall North"), -3736.883057f, -201206.5156f,
	wxT("Windfall South"), -926.9960938f, -197597.0625f
};

std::list<IsleMap> isleList;
bool useDefaultList = false;

TWWSuperswim::TWWSuperswim(wxWindow* parent, wxWindowID id, const wxString& title, const wxPoint& pos, const wxSize& size, long style) : wxDialog(parent, id, title, pos, size, style)
{
	SetSizeHints(wxDefaultSize, wxDefaultSize);

	wxFlexGridSizer* fgSizer1;
	fgSizer1 = new wxFlexGridSizer(0, 2, 0, 0);
	fgSizer1->SetFlexibleDirection(wxBOTH);
	fgSizer1->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);

	m_button1 = new wxButton(this, 1, wxT("Use Links Location"), wxDefaultPosition, wxDefaultSize, 0);
	fgSizer1->Add(m_button1, 0, wxALL, 5);

	//Isle Choice
	wxArrayString m_choice_isleChoices;
	m_choice_isle = new wxChoice(this, 2, wxDefaultPosition, wxSize(200, -1), m_choice_isleChoices, 0);
	fgSizer1->Add(m_choice_isle, 0, wxALIGN_CENTER | wxALL, 5);

	//Get Isle List Contents
	std::string iniContent;
	bool success = File::ReadFileToString(File::GetExeDirectory() + "\\Scripts\\Superswim.ini", iniContent);

	if (!success)
	{
		//Fill Isle List with default
		for (IsleArray::const_iterator it = isleListDefault.begin(); it != isleListDefault.end(); ++it)
		{
			m_choice_isle->Append(it->isleName);
		}

		useDefaultList = true;
	}
	else
	{
		std::istringstream stream(iniContent);
		std::string line;
		IsleMap listEntry;
		bool readName = true;
		bool readX = false;
		bool readZ = false;
		bool readEmpty = false;

		while (std::getline(stream, line)) 
		{
			if (readName)
			{
				listEntry.isleName = StrToWxStr(line);

				readName = false;
				readX = true;
			}
			else if (readX)
			{
				listEntry.xPos = atof(line.c_str());

				readX = false;
				readZ = true;
			}
			else if (readZ)
			{
				listEntry.zPos = atof(line.c_str());

				readZ = false;
				readEmpty = true;

				//Add to list
				isleList.push_back(listEntry);
			}
			else if (readEmpty)
			{
				readEmpty = false;
				readName = true;
			}
		}

		for (std::list<IsleMap>::iterator it = isleList.begin(); it != isleList.end(); ++it)
		{
			m_choice_isle->Append(it->isleName);
		}

		useDefaultList = false;
	}


	fgSizer1->Add(0, 10, 1, wxEXPAND, 5);
	fgSizer1->Add(0, 10, 1, wxEXPAND, 5);


	m_staticText8 = new wxStaticText(this, wxID_ANY, wxT("Destination:"), wxDefaultPosition, wxDefaultSize, 0);
	m_staticText8->Wrap(-1);
	m_staticText8->SetFont(wxFont(wxNORMAL_FONT->GetPointSize(), 70, 90, 92, false, wxEmptyString));

	fgSizer1->Add(m_staticText8, 0, wxALL, 10);

	m_panel1 = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
	fgSizer1->Add(m_panel1, 1, wxEXPAND | wxALL, 5);

	m_staticText10 = new wxStaticText(this, wxID_ANY, wxT("X Pos"), wxDefaultPosition, wxDefaultSize, 0);
	m_staticText10->Wrap(-1);
	fgSizer1->Add(m_staticText10, 0, wxALIGN_RIGHT | wxALL, 10);

	m_textCtrl_XPos = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0);
	fgSizer1->Add(m_textCtrl_XPos, 0, wxALIGN_CENTER | wxALL, 5);

	m_staticText12 = new wxStaticText(this, wxID_ANY, wxT("Z Pos"), wxDefaultPosition, wxDefaultSize, 0);
	m_staticText12->Wrap(-1);
	fgSizer1->Add(m_staticText12, 0, wxALIGN_RIGHT | wxALL, 10);

	m_textCtrl_ZPos = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0);
	fgSizer1->Add(m_textCtrl_ZPos, 0, wxALIGN_CENTER | wxALL, 5);

	fgSizer1->Add(0, 10, 1, wxEXPAND, 5);
	fgSizer1->Add(0, 10, 1, wxEXPAND, 5);


	m_button4 = new wxButton(this, 3, wxT("Start"), wxDefaultPosition, wxDefaultSize, 0);
	fgSizer1->Add(m_button4, 0, wxALIGN_CENTER | wxALL, 10);

	m_button5 = new wxButton(this, 4, wxT("Cancel"), wxDefaultPosition, wxDefaultSize, 0);
	fgSizer1->Add(m_button5, 0, wxALIGN_CENTER | wxALL, 10);

	fgSizer1->AddSpacer(10);

	SetSizer(fgSizer1);
	Layout();

	fgSizer1->Fit(this);

	Centre(wxBOTH);

	Bind(wxEVT_CLOSE_WINDOW, &TWWSuperswim::OnCloseWindow, this);
}

void TWWSuperswim::OnSelectionChanged(wxCommandEvent& event)
{
	if (event.GetId() == 2) //Isle Selection
	{
		int selectedIsleID = m_choice_isle->GetSelection();

		float XPos, ZPos;

		if (useDefaultList)
		{
			XPos = isleListDefault[selectedIsleID].xPos;
			ZPos = isleListDefault[selectedIsleID].zPos;
		}
		else
		{
			int counter = 0;

			for (std::list<IsleMap>::iterator it = isleList.begin(); it != isleList.end(); ++it)
			{
				if (counter == selectedIsleID)
				{
					XPos = it->xPos;
					ZPos = it->zPos;

					break;
				}

				counter++;
			}
		}

		m_textCtrl_XPos->SetValue(wxString::Format(wxT("%f"), XPos));
		m_textCtrl_ZPos->SetValue(wxString::Format(wxT("%f"), ZPos));
	}
}

void TWWSuperswim::OnButtonPressed(wxCommandEvent& event)
{
	if (!Core::IsRunningAndStarted())
		return;

	std::string gameID = SConfig::GetInstance().GetUniqueID();
	u32 charPointerAdd;
	bool isTWW = false;

	if (!gameID.compare("GZLJ01"))
	{
		charPointerAdd = 0x3ad860;

		isTWW = true;
	}

	if (event.GetId() == 1) //Pick Links Current Location Button Pressed
	{
		if (isTWW)
		{
			u32 characterpointer = Memory::Read_U32(charPointerAdd);

			if (characterpointer > 0x80000000)
			{
				u32 XAdd = 0x3d78fc;
				u32 ZAdd = 0x3d7904;

				float LinkX = Memory::Read_F32(XAdd);
				float LinkZ = Memory::Read_F32(ZAdd);

				m_textCtrl_XPos->SetValue(wxString::Format(wxT("%f"), LinkX));
				m_textCtrl_ZPos->SetValue(wxString::Format(wxT("%f"), LinkZ));
			}
		}
	}

	if (event.GetId() == 3) //Start
	{
		if (isTWW && !Movie::swimInProgress && !Movie::swimStarted)
		{
			//wxMessageBox("Start Superswim!");

			wxString xPosStr = m_textCtrl_XPos->GetValue();
			wxString zPosStr = m_textCtrl_ZPos->GetValue();

			double xPos, zPos;

			xPosStr.ToDouble(&xPos);
			zPosStr.ToDouble(&zPos);

			Movie::swimDestPosX = xPos;
			Movie::swimDestPosZ = zPos;
			Movie::swimStarted = true;
		}
	}
	if (event.GetId() == 4) //Cancel
	{
		Movie::swimStarted = false;

		//wxMessageBox("Cancelled Superswim!");
	}
}

void TWWSuperswim::OnCloseWindow(wxCloseEvent& event)
{
	if (event.CanVeto())
	{
		event.Skip(false);
		Show(false);
	}
}