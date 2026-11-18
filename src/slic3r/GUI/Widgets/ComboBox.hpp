#ifndef slic3r_GUI_ComboBox_hpp_
#define slic3r_GUI_ComboBox_hpp_

#include "TextInput.hpp"
#include "DropDown.hpp"

#define CB_NO_DROP_ICON DD_NO_DROP_ICON
#define CB_NO_TEXT DD_NO_TEXT

class ComboBox : public wxWindowWithItems<TextInput, wxItemContainer>
{
    std::vector<wxString>         texts;
    std::vector<wxBitmapBundle>   icons;
    std::vector<void *>           datas;
    std::vector<wxClientDataType> types;

    DropDown               drop;
    bool     drop_down = false;
    bool     text_off = false;
    int      popup_selection = wxNOT_FOUND;
    wxString popup_value;
    bool     popup_committed = false;
    bool     m_publish_accessible = false;

public:
    ComboBox(wxWindow *      parent,
             wxWindowID      id,
             const wxString &value     = wxEmptyString,
             const wxPoint & pos       = wxDefaultPosition,
             const wxSize &  size      = wxDefaultSize,
             int             n         = 0,
             const wxString  choices[] = NULL,
             long            style     = 0);

    DropDown & GetDropDown() { return drop; }

    // Read-only choices hide TextInput's editor. They must receive focus
    // themselves instead of delegating keyboard navigation to that child.
    bool AcceptsFocus() const override
        { return HasFlag(wxCB_READONLY) || TextInput::AcceptsFocus(); }
    bool AcceptsFocusRecursively() const override
        { return HasFlag(wxCB_READONLY) || TextInput::AcceptsFocusRecursively(); }
    bool AcceptsFocusFromKeyboard() const override
        { return HasFlag(wxCB_READONLY) || TextInput::AcceptsFocusFromKeyboard(); }

    bool SetFont(wxFont const & font) override;

    bool SetBackgroundColour(const wxColour& colour) override;
    bool SetForegroundColour(const wxColour& colour) override;

    void SetBorderColor(StateColor const& color);

    int Append(const wxString &item, const wxBitmapBundle &bitmap = wxNullBitmap);
    int Append(const wxString &item, const wxBitmapBundle &bitmap, void *clientData);

    int Insert(const wxString& item, const wxBitmapBundle& bitmap, unsigned int pos);
    int Insert(const wxString& item, const wxBitmapBundle& bitmap,
        unsigned int pos, void* clientData);

    unsigned int GetCount() const override;

    int  GetSelection() const override;

    void SetSelection(int n) override;

    virtual void Rescale() override;

    wxString GetValue() const;
    void     SetValue(const wxString &value);

    void SetLabel(const wxString &label) override;
    wxString GetLabel() const override;

    void SetTextLabel(const wxString &label);
    wxString GetTextLabel() const;

    wxString GetString(unsigned int n) const override;
    void     SetString(unsigned int n, wxString const &value) override;

    wxBitmap GetItemBitmap(unsigned int n);

    void     OnKeyDown(wxKeyEvent& event);

    // Opt-in: publish name + current value (description) through ATK/AT-SPI (GTK only; no-op elsewhere).
    void     publish_accessible_value() { m_publish_accessible = true; update_accessible(); }
    void     update_accessible();
    void     SetName(const wxString &name) override { TextInput::SetName(name); update_accessible(); }

protected:
    virtual int  DoInsertItems(const wxArrayStringsAdapter &items,
                               unsigned int                 pos,
                               void **                      clientData,
                               wxClientDataType             type) override;
    void DoClear() override;

    void DoDeleteOneItem(unsigned int pos) override;

    void *DoGetItemClientData(unsigned int n) const override;
    void  DoSetItemClientData(unsigned int n, void *data) override;
    
    void OnEdit() override;

    // Natural size for combos created without an explicit size: widest entry + drop-down arrow.
    wxSize DoGetBestSize() const override;

#ifdef __WIN32__
    WXLRESULT MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam) override;
#endif

private:

    // some useful events
    void mouseDown(wxMouseEvent &event);
    void mouseWheelMoved(wxMouseEvent &event);
    void keyDown(wxKeyEvent &event);

    void sendComboBoxEvent();
    void openPopup();
    void closePopup(bool commit);

    DECLARE_EVENT_TABLE()
};

// App-owned read-only drop-down for dialogs and panels (replaces wxChoice / read-only wxComboBox, whose GTK popup closes on
// button release under GTK3 + XWayland/KWin). Optional name/tooltip; the name and the shown value are published through AT-SPI.
ComboBox *create_readonly_combo(wxWindow *parent, const wxString &name = wxString(), const wxString &tooltip = wxString(),
                                const wxArrayString &items = wxArrayString(), int selection = 0);

#endif // !slic3r_GUI_ComboBox_hpp_
