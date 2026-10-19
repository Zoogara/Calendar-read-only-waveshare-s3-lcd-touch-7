/* calendar_ui.c compiled in here so the renderer can reach its private state (which view is
 * showing, the focused day) without changing the firmware source. */
#include "../../components/calendar_ui/calendar_ui.c"

void host_select_view(int v) { select_view((ui_view_t)v); }
void host_set_cursor(time_t t) { s_cursor = ui_start_of_day(t); render_current_view(); update_title(); }
