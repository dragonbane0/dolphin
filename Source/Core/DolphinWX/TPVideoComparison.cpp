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
#include "DolphinWX/TPVideoComparison.h"
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
BEGIN_EVENT_TABLE(TPVideoComparison, wxDialog)

EVT_CHOICE(1, TPVideoComparison::OnSelectionChanged)
EVT_BUTTON(2, TPVideoComparison::OnButtonPressed)

END_EVENT_TABLE()


TPVideoComparison::TPVideoComparison(wxWindow* parent, wxWindowID id, const wxString& title, const wxPoint& pos, const wxSize& size, long style) : wxDialog(parent, id, title, pos, size, style)
{
	SetSizeHints(wxDefaultSize, wxDefaultSize);

	wxBoxSizer* bSizer1;
	bSizer1 = new wxBoxSizer(wxVERTICAL);

	//Text
	m_staticText = new wxStaticText(this, wxID_ANY, wxT("Left Movie:"), wxDefaultPosition, wxDefaultSize, 0);
	m_staticText->Wrap(-1);
	m_staticText->SetFont(wxFont(12, 74, 90, 92, false, wxT("Arial")));
	bSizer1->Add(m_staticText, 0, wxALL, 5);

	//Dragonbane: Get last chosen Path
	IniFile settingsIni;
	IniFile::Section* iniPathSection;
	std::string lastCmpPath1 = "";
	std::string lastCmpPath2 = "";
	std::string lastSavedPath = "";
	bool fileLoaded = false;

	fileLoaded = settingsIni.Load(File::GetUserPath(F_DOLPHINCONFIG_IDX));

	//Get Path
	if (fileLoaded)
	{
		iniPathSection = settingsIni.GetOrCreateSection("RememberedPaths");
		iniPathSection->Get("LastComparedDTMPath1", &lastCmpPath1, "");
		iniPathSection->Get("LastComparedDTMPath2", &lastCmpPath2, "");
		iniPathSection->Get("LastComparedSavedPath", &lastSavedPath, "");
	}

	//File Picker1
	m_DTM1Path = new wxFilePickerCtrl(this, wxID_ANY,
		lastCmpPath1, _("Select A Recording File"),
		("Dolphin TAS Movies (*.dtm)") + wxString::Format("|*.dtm|%s", wxGetTranslation(wxALL_FILES)), 
		wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_OPEN);

	bSizer1->Add(m_DTM1Path, 0, wxALL, 5);

	//Text 4
	m_staticText4 = new wxStaticText(this, wxID_ANY, wxT("Description:"), wxDefaultPosition, wxDefaultSize, 0);
	m_staticText4->Wrap(-1);
	m_staticText4->SetFont(wxFont(12, 74, 90, 92, false, wxT("Arial")));
	bSizer1->Add(m_staticText4, 0, wxALL, 5);

	//Textbox Left Title
	titleLeft = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0);
	bSizer1->Add(titleLeft, 0, wxALL, 5);


	//Text0
	m_staticText0 = new wxStaticText(this, wxID_ANY, wxT("Right Movie:"), wxDefaultPosition, wxDefaultSize, 0);
	m_staticText0->Wrap(-1);
	m_staticText0->SetFont(wxFont(12, 74, 90, 92, false, wxT("Arial")));
	bSizer1->Add(m_staticText0, 0, wxALL, 5);

	//File Picker 2
	m_DTM2Path = new wxFilePickerCtrl(this, wxID_ANY,
		lastCmpPath2, _("Select A Recording File"),
		("Dolphin TAS Movies (*.dtm)") + wxString::Format("|*.dtm|%s", wxGetTranslation(wxALL_FILES)),
		wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_OPEN);

	bSizer1->Add(m_DTM2Path, 0, wxALL, 5);

	//Text 5
	m_staticText5 = new wxStaticText(this, wxID_ANY, wxT("Description:"), wxDefaultPosition, wxDefaultSize, 0);
	m_staticText5->Wrap(-1);
	m_staticText5->SetFont(wxFont(12, 74, 90, 92, false, wxT("Arial")));
	bSizer1->Add(m_staticText5, 0, wxALL, 5);

	//Textbox Right Title
	titleRight = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0);
	bSizer1->Add(titleRight, 0, wxALL, 5);

	//Text 1
	m_staticText1 = new wxStaticText(this, wxID_ANY, wxT("Video Resolution:"), wxDefaultPosition, wxDefaultSize, 0);
	m_staticText1->Wrap(-1);
	m_staticText1->SetFont(wxFont(12, 74, 90, 92, false, wxT("Arial")));
	bSizer1->Add(m_staticText1, 0, wxALL, 5);


	//Video Res Choice
	wxString m_choice_resChoices[] = { wxT("360p"), wxT("480p"), wxT("720p"), wxT("1080p") };
	int m_choice_resNChoices = sizeof(m_choice_resChoices) / sizeof(wxString);
	m_choice_res = new wxChoice(this, 1, wxDefaultPosition, wxDefaultSize, m_choice_resNChoices, m_choice_resChoices, 0);
	bSizer1->Add(m_choice_res, 0, wxALL, 5);


	//Text 2
	m_staticText2 = new wxStaticText(this, wxID_ANY, wxT("Video Width:"), wxDefaultPosition, wxDefaultSize, 0);
	m_staticText2->Wrap(-1);
	m_staticText2->SetFont(wxFont(12, 74, 90, 92, false, wxT("Arial")));
	bSizer1->Add(m_staticText2, 0, wxALL, 5);

	//Textbox Width
	resWidth = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0);
	bSizer1->Add(resWidth, 0, wxALL, 5);

	//Text 3
	m_staticText3 = new wxStaticText(this, wxID_ANY, wxT("Video Height:"), wxDefaultPosition, wxDefaultSize, 0);
	m_staticText3->Wrap(-1);
	m_staticText3->SetFont(wxFont(12, 74, 90, 92, false, wxT("Arial")));
	bSizer1->Add(m_staticText3, 0, wxALL, 5);

	//Textbox Height
	resHeight = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0);
	bSizer1->Add(resHeight, 0, wxALL, 5);

	//Text 6
	m_staticText6 = new wxStaticText(this, wxID_ANY, wxT("Output Path:"), wxDefaultPosition, wxDefaultSize, 0);
	m_staticText6->Wrap(-1);
	m_staticText6->SetFont(wxFont(12, 74, 90, 92, false, wxT("Arial")));
	bSizer1->Add(m_staticText6, 0, wxALL, 5);

	//File Picker 3
	m_savePath = new wxFilePickerCtrl(this, wxID_ANY,
		lastSavedPath, _("Save Comparison As"),
		("AVI Video (*.avi)") + wxString::Format("|*.avi|%s", wxGetTranslation(wxALL_FILES)),
		wxDefaultPosition, wxDefaultSize, wxFLP_USE_TEXTCTRL | wxFLP_SAVE);

	bSizer1->Add(m_savePath, 0, wxALL, 5);

	bSizer1->AddSpacer(10);

	m_button_start = new wxButton(this, 2, "Start Recording", wxDefaultPosition, wxDefaultSize);
	m_button_start->Enable();

	bSizer1->Add(m_button_start, 1, wxALIGN_CENTER, 5);

	bSizer1->AddSpacer(10);

	SetSizer(bSizer1);
	Layout();

	bSizer1->Fit(this);

	Centre(wxBOTH);

	Bind(wxEVT_CLOSE_WINDOW, &TPVideoComparison::OnCloseWindow, this);
}

void TPVideoComparison::OnSelectionChanged(wxCommandEvent& event)
{
	if (event.GetId() == 1) //Video Res Choice
	{
		if (event.GetInt() == 0)
		{
			resWidth->SetValue("640");
			resHeight->SetValue("360");
		}
		else if (event.GetInt() == 1)
		{
			resWidth->SetValue("856");
			resHeight->SetValue("480");
		}
		else if (event.GetInt() == 2)
		{
			resWidth->SetValue("1280");
			resHeight->SetValue("720");
		}
		else if (event.GetInt() == 3)
		{
			resWidth->SetValue("1920");
			resHeight->SetValue("1080");
		}
	}

}
void TPVideoComparison::OnButtonPressed(wxCommandEvent& event)
{	
	if (event.GetId() == 2) //Start Button Pressed
	{
		if (Core::IsRunning())
		{
			wxMessageBox("Please close the game before attempting to create a video comparison!");
			return;
		}

		if (Movie::cmp_requested || Movie::cmp_isRunning)
		{
			wxMessageBox("Please stop the current comparison and close the game before attempting the next one!");
			return;
		}

		std::string path1 = WxStrToStr(m_DTM1Path->GetPath());
		std::string path2 = WxStrToStr(m_DTM2Path->GetPath());
		std::string path3 = WxStrToStr(m_savePath->GetPath());

		if (path1.empty() || path2.empty())
		{
			wxMessageBox("Empty paths!");
			return;
		}

		if (!File::Exists(path1) || !File::Exists(path2))
		{
			wxMessageBox("Files don't exist!");
			return;
		}

		if (path1 == path2)
		{
			wxMessageBox("DTMs are identical!");
			return;
		}

		//Dragonbane: Save last chosen Paths
		IniFile settingsIni;
		IniFile::Section* iniPathSection;
		bool fileLoaded = false;

		fileLoaded = settingsIni.Load(File::GetUserPath(F_DOLPHINCONFIG_IDX));

		//Save Path
		if (fileLoaded)
		{
			iniPathSection = settingsIni.GetOrCreateSection("RememberedPaths");

			std::string file, legalPathname, extension;

			SplitPathEscapeChar(WxStrToStr(path1), &legalPathname, &file, &extension);
			iniPathSection->Set("LastComparedDTMPath1", legalPathname);

			SplitPathEscapeChar(WxStrToStr(path2), &legalPathname, &file, &extension);
			iniPathSection->Set("LastComparedDTMPath2", legalPathname);

			SplitPathEscapeChar(WxStrToStr(path3), &legalPathname, &file, &extension);
			iniPathSection->Set("LastComparedSavedPath", legalPathname);

			settingsIni.Save(File::GetUserPath(F_DOLPHINCONFIG_IDX));
		}


		std::string vidWidthStr = WxStrToStr(resWidth->GetValue());
		std::string vidHeightStr = WxStrToStr(resHeight->GetValue());

		if (vidWidthStr.empty() || vidHeightStr.empty())
		{
			wxMessageBox("Invalid resolution!");
			return;
		}

		int vidWidth = 0;
		int vidHeight = 0;

		vidWidth = atoi(vidWidthStr.c_str());
		vidHeight = atoi(vidHeightStr.c_str());

		if (vidWidth < 428 || vidHeight < 240)
		{
			wxMessageBox("Minimum resolution is 428x240!");
			return;
		}

		if (vidWidth % 4 != 0 || vidHeight % 4 != 0)
		{
			wxMessageBox("Width and Height have to be multiples of 4!"); 
			return;
		}

		std::string leftTitle = WxStrToStr(titleLeft->GetValue());
		std::string rightTitle = WxStrToStr(titleRight->GetValue());

		if (SConfig::GetInstance().m_strVideoBackend.compare("Direct3D") && SConfig::GetInstance().m_strVideoBackend.compare("D3D"))
		{
			wxMessageBox("Comparison videos are currently only supported if you use the Direct3D renderer/backend!");
			return;
		}

		Movie::RequestVideoComparison(path1, path2, leftTitle, rightTitle, vidWidth, vidHeight, path3);
	}
}

void TPVideoComparison::OnCloseWindow(wxCloseEvent& event)
{
	if (event.CanVeto())
	{
		event.Skip(false);
		Show(false);
	}
}