// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#pragma once

//Dragonbane

#include <wx/defs.h>
#include <wx/dialog.h>
#include <wx/gdicmn.h>
#include <wx/string.h>
#include <wx/translation.h>
#include <wx/windowid.h>
#include <wx/artprov.h>
#include <wx/stattext.h>
#include <wx/font.h>
#include <wx/colour.h>
#include <wx/settings.h>
#include <wx/choice.h>
#include <wx/textctrl.h>
#include <wx/sizer.h>

#include "Common/CommonTypes.h"
#include <string>



class wxWindow;
class wxButton;
class wxFilePickerCtrl;

class TPVideoComparison : public wxDialog
{
private:

	DECLARE_EVENT_TABLE();

protected:

	wxStaticText* m_staticText;
	wxFilePickerCtrl *m_DTM1Path;
	wxStaticText* m_staticText0;
	wxFilePickerCtrl *m_DTM2Path;
	wxStaticText* m_staticText1;
	wxChoice* m_choice_res;
	wxStaticText* m_staticText2;
	wxTextCtrl* resWidth;
	wxStaticText* m_staticText3;
	wxTextCtrl* resHeight;
	wxStaticText* m_staticText4;
	wxTextCtrl* titleLeft;
	wxStaticText* m_staticText5;
	wxTextCtrl* titleRight;
	wxStaticText* m_staticText6;
	wxFilePickerCtrl *m_savePath;
	wxButton* m_button_start;

public:

	TPVideoComparison(wxWindow* parent, wxWindowID id = wxID_ANY, const wxString& title = wxT("Video Comparison by DB"), const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize, long style  = wxDEFAULT_DIALOG_STYLE | wxSTAY_ON_TOP);
	void OnSelectionChanged(wxCommandEvent& event);
	void OnButtonPressed(wxCommandEvent& event);
	void OnCloseWindow(wxCloseEvent& event);
};
