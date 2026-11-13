/* calendar_ui.c compiled in here so the renderer can reach its private state (which view is
 * showing, the focused day) without changing the firmware source. */
#include "../../components/calendar_ui/calendar_ui.c"

void host_open_settings_menu(void) { settings_btn_cb(NULL); }
void host_close_top(void)
{
    /* modal widgets (msgbox) sit on lv_layer_top() inside a backdrop; remove the topmost */
    lv_obj_t *top = lv_layer_top();
    uint32_t n = lv_obj_get_child_cnt(top);
    if (n > 0) {
        lv_obj_del(lv_obj_get_child(top, (int32_t)n - 1));
    }
}

void host_select_view(int v) { select_view((ui_view_t)v); }
void host_set_cursor(time_t t) { s_cursor = ui_start_of_day(t); render_current_view(); update_title(); }
