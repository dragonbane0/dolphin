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
#include "DolphinWX/TWWTunerInput.h"
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
BEGIN_EVENT_TABLE(TWWTunerInput, wxDialog)

EVT_BUTTON(1, TWWTunerInput::OnButtonPressed)

END_EVENT_TABLE()


TWWTunerInput::TWWTunerInput(wxWindow* parent, wxWindowID id, const wxString& title, const wxPoint& pos, const wxSize& size, long style) : wxDialog(parent, id, title, pos, size, style)
{
	SetSizeHints(wxDefaultSize, wxDefaultSize);

	wxBoxSizer* bSizer1;
	bSizer1 = new wxBoxSizer(wxVERTICAL);

	//Headline
	m_staticText = new wxStaticText(this, wxID_ANY, wxT("ID:"), wxDefaultPosition, wxDefaultSize, 0);
	m_staticText->Wrap(-1);
	m_staticText->SetFont(wxFont(12, 74, 90, 92, false, wxT("Arial")));
	bSizer1->Add(m_staticText, 0, wxALL, 5);

	//Textbox Input
	m_text_ctrl = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0);
	bSizer1->Add(m_text_ctrl, 0, wxALL, 5);

	bSizer1->AddSpacer(10);

	m_button_start = new wxButton(this, 1, "Execute Action", wxDefaultPosition, wxDefaultSize);
	m_button_start->Enable();

	bSizer1->Add(m_button_start, 1, wxALIGN_CENTER, 5);

	bSizer1->AddSpacer(10);

	SetSizer(bSizer1);
	Layout();

	bSizer1->Fit(this);

	Centre(wxBOTH);

	Bind(wxEVT_CLOSE_WINDOW, &TWWTunerInput::OnCloseWindow, this);
}

void TWWTunerInput::OnButtonPressed(wxCommandEvent& event)
{	
	if (event.GetId() == 1) //Action Button Pressed
	{
		if (!Core::IsRunningAndStarted())
		{
			wxMessageBox("Please start the game before trying to execute events!");
			return;
		}

		std::string gameID = SConfig::GetInstance().GetUniqueID();

		if (gameID.compare("GZLJ01") && gameID.compare("GZLE01") && gameID.compare("GZLP01"))
		{
			wxMessageBox("This is not Wind Waker!");
			return;
		}

		std::string idStr = WxStrToStr(m_text_ctrl->GetValue());

		if (idStr.empty() || !idStr.compare("?"))
		{
			wxMessageBox("1-8: Cursor movement clockwise\n  9: Reset Cursor\n10: Bomb\n11: Balloon\n12: Shield\n13: Kooloo-Limpah\n14: Red Ting\n15: Green Ting\n16: Blue Ting\n17: Hey Tingle\n18: Activate fake GBA\n19: Deactivate fake GBA");
			return;
		}

		int idTuner = 0;
		idTuner = atoi(idStr.c_str());

		if (idTuner < 1 || idTuner > 19)
		{
			wxMessageBox("Minimum ID is 1 and maximum ID is 19!");
			return;
		}

		if (Movie::IsPlayingInput())
		{
			wxMessageBox("Not available, movie is in Playback!");
			return;
		}

		if (Movie::tunerExecuteID > 0)
		{
			wxMessageBox("Action still running! Please retry later");
			return;
		}

		if (Movie::IsRecordingInput())
		{
			Movie::tunerActionID = idTuner;
		}
		else
		{
			Movie::tunerExecuteID = idTuner;
		}
	}
}

void TWWTunerInput::OnCloseWindow(wxCloseEvent& event)
{
	if (event.CanVeto())
	{
		event.Skip(false);
		Show(false);
	}
}