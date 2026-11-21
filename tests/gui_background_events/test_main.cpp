#include <catch2/catch_session.hpp>
#include <wx/app.h>
#include <wx/init.h>

namespace {
class EventTestApp final : public wxAppConsole {
public:
    bool OnInit() override { return true; }
};
}

int main(int argc, char **argv)
{
    wxAppConsole::SetInstance(new EventTestApp);
    if (!wxEntryStart(argc, argv)) return 2;
    if (!wxAppConsole::GetInstance()->CallOnInit()) { wxEntryCleanup(); return 3; }
    int result;
    { Catch::Session session; result = session.run(argc, argv); }
    wxAppConsole::GetInstance()->ProcessPendingEvents();
    wxAppConsole::GetInstance()->OnExit();
    wxEntryCleanup();
    return result;
}
